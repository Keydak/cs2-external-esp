#include "Emotes.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/GameThread.hpp"
#include "core/features/EmoteDances.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/MaterialChams.hpp"
#include "core/features/Movement.hpp"
#include "core/features/Sounds.hpp"
#include "core/features/View.hpp"
#include "core/features/Visuals.hpp"
#include "core/offsets/Dumper.hpp"
#include "updater/http/HttpHelper.hpp"

namespace {
    struct Clip {
        const char* name;
        const char* path;
    };

    // Clips of the game on the skeleton of the players (worldmodel.vnmskel): the MVP banner, the main menu, the end of
    // the round, the inventory. The order is the one of the wheel
    constexpr Clip CLIPS[] = {
        { "Celebrate 1",    "animation/anims/ui_anims/mvp/banner_celebrate01.vnmclip" },
        { "Celebrate 2",    "animation/anims/ui_anims/mvp/banner_celebrate02.vnmclip" },
        { "Celebrate 3",    "animation/anims/ui_anims/mvp/banner_celebrate03.vnmclip" },
        { "Celebrate 4",    "animation/anims/ui_anims/mvp/banner_celebrate04.vnmclip" },
        { "Knife Backflip", "animation/anims/ui_anims/main_menu/ct/ct_main_menu_knife_backflip.vnmclip" },
        { "Knife Tricks",   "animation/anims/ui_anims/main_menu/t/t_main_menu_knife_fancymoves.vnmclip" },
        { "Shrug",          "animation/anims/ui_anims/main_menu/ct/ct_main_menu_elite_shrug.vnmclip" },
        { "Look At Watch",  "animation/anims/ui_anims/main_menu/ct/ct_main_menu_famas_lookatwatch.vnmclip" },
        { "Butt Scratch",   "animation/anims/ui_anims/main_menu/t/t_main_menu_pistol_buttscratch.vnmclip" },
        { "Head Scratch",   "animation/anims/ui_anims/main_menu/t/t_main_menu_pistol_headscratch.vnmclip" },
        { "Belly Rub",      "animation/anims/ui_anims/main_menu/t/t_main_menu_elite_bellyrub.vnmclip" },
        { "C4 Bobble",      "animation/anims/ui_anims/main_menu/t/t_main_menu_c4_bobble.vnmclip" },
        { "Victory 1",      "animation/anims/ui_anims/end_round/celebrate/ct/celebrate_ava_idle01.vnmclip" },
        { "Victory 2",      "animation/anims/ui_anims/end_round/celebrate/t/celebrate_darryl_idle01.vnmclip" },
        { "Defeat",         "animation/anims/ui_anims/end_round/defeat/ct/defeat_ava_idle01.vnmclip" },
        { "Pose 1",         "animation/anims/ui_anims/inventory_pose/ct/cu_ct_pose01.vnmclip" },
        { "Pose 2",         "animation/anims/ui_anims/inventory_pose/t/cu_t_pose01.vnmclip" },
        { "Flashed",        "animation/anims/world/shared/flashed.vnmclip" },
        { "Dead",           "animation/anims/world/shared/death_poses_body.vnmclip" },
    };
    constexpr int CLIP_COUNT = static_cast<int>(std::size(CLIPS));
    constexpr int DANCE_COUNT = static_cast<int>(std::size(emote_dances::DANCES));

    constexpr auto POLL = std::chrono::milliseconds(5);
    constexpr auto LOAD_POLL = std::chrono::milliseconds(100);
    constexpr auto LOAD_TIMEOUT = std::chrono::seconds(30);
    constexpr auto RETRY = std::chrono::seconds(10);
    constexpr auto CLICK_GRACE = std::chrono::milliseconds(300);    // A click this soon after the start picked it on the wheel
    constexpr auto STUB_SETTLE = std::chrono::milliseconds(50);     // A pose being sampled when it was turned off is done by then

    constexpr float STILL_CLIP = 1.f;   // Seconds: a shorter clip is a pose, its first frame held
    constexpr float POSE_HOLD = 5.f;    // How long a pose is held
    constexpr float MOVING = 10.f;      // Units per second, faster ends it
    // Units from where it started that end it: a fall, a landing, a slope or a push moves our player without much speed
    // along the ground, a dance stays on its spot & the camera would go with the hidden player
    constexpr float LEFT_SPOT = 32.f;
    constexpr uint32_t FL_ONGROUND = 1 << 0;

    // The Fortnite dances: the pack is looked for in this folder & up to 3 folders down (its zip can be extracted as
    // it is), again every few seconds until it is found
    const std::filesystem::path PACK_ROOT = "cache/emotes";
    constexpr int PACK_DEPTH = 3;
    constexpr auto PACK_CHECK = std::chrono::seconds(2);
    constexpr auto NO_PACK = "Fortnite dances: download the Emote Pack, or extract it into cache\\emotes";
    constexpr auto HIDE_TIMEOUT = std::chrono::seconds(3);          // Our player hidden by then, else no dance
    constexpr auto ALIVE_CHECK = std::chrono::milliseconds(250);    // The dance still ours, not removed by the game
    constexpr auto SCENE_WAIT = std::chrono::milliseconds(500);     // For the scene objects of the dance model, to hide them
    constexpr auto COPY_SETTLE = std::chrono::seconds(1);           // The copy of our agent drawn by then
    constexpr float SPAWN_DROP = 1024.f;    // Units under the floor it is spawned at, moved up once the dance model is hidden
    constexpr uint32_t BONEMERGE = 0x1C9748E6;  // Token of "!bonemerge": the attachment of a copy that follows the dance
    constexpr uint64_t SPAWN_FLAGS = 0x1FFFFFFFFFFFF;   // Every resource of the entity confirmed, as the portrait world asks
    constexpr uint32_t ADD_TO_TAIL = 1, SEARCH_PRIORITY = 2;    // How our folder is added to the GAME path

    // The pack from our assets on GitHub (emotes/ of it): only the files the dances use. The icons first, the wheel takes
    // them once the pack is found, then the model, then the songs (a dance without its song yet plays without music)
    constexpr auto PACK_TREE = "https://api.github.com/repos/Keydak/cs2-external-assets/git/trees/main?recursive=1";
    constexpr auto PACK_FILES = "https://raw.githubusercontent.com/Keydak/cs2-external-assets/main/";
    constexpr std::string_view PACK_REPOSITORY = "emotes/";
    constexpr uint64_t PACK_FILE_MAX = 64ull << 20;
    constexpr const char* SONG_FORMATS[] = { ".mp3", ".wav" };    // What a song of the pack may be, the first taken

    // IGameResourceService: its vtable entries, & the load the player preview of the menu asks for
    constexpr size_t CREATE_MANIFEST = 27, MANIFEST_LOADED = 29, DESTROY_MANIFEST = 31;
    constexpr uint32_t LOAD_ASYNC = 1, LOAD_PRIORITY = 2;

    // Page in reach of client.dll: the stub & its data, then the reason & names the manifests are made with (the game
    // may keep them, the page is never freed)
    constexpr size_t PAGE_SIZE = 0x2000;
    constexpr uintptr_t STUB = 0x000;
    constexpr uintptr_t DATA_ACTIVE = 0x400;      // uint32, 0 the stub leaves every pose alone
    constexpr uintptr_t DATA_PAWN = 0x408;        // uint64, our pawn, the owner of the skeleton the pose is for
    constexpr uintptr_t DATA_SKELETON = 0x410;    // uint64, CNmSkeleton of the clip, the pose has to be of it
    constexpr uintptr_t DATA_BONES = 0x418;       // uint32, its bones
    constexpr uintptr_t DATA_START = 0x41C;       // float, real time of the game at the start
    constexpr uintptr_t DATA_CLIP = 0x420;        // uint64, CNmClip
    constexpr uintptr_t DATA_RATE = 0x428;        // float, 1 over its length, 0 holds its first frame
    constexpr uintptr_t CONST_ZERO = 0x42C;       // float
    constexpr uintptr_t CONST_ONE = 0x430;        // float
    constexpr uintptr_t DATA_GLOBALS = 0x438;     // uint64, where the game keeps its global variables (real time first)
    constexpr uintptr_t DATA_GET_POSE = 0x440;    // uint64, CNmClip::GetPose
    constexpr uintptr_t PAGE_REASON = 0x480;      // "emotes"
    constexpr uintptr_t PAGE_NAMES = 0x500;       // const char* of each clip
    constexpr uintptr_t PAGE_PATHS = 0x600;       // The paths, PATH_SIZE each
    constexpr size_t PATH_SIZE = 0x80;
    constexpr uintptr_t PAGE_DANCE_MODEL = 0xF80; // The path of the dance model, PATH_SIZE
    constexpr uintptr_t PAGE_DANCE_NAMES = 0x1000;// const char* of it
    static_assert(PAGE_NAMES + CLIP_COUNT * 8 <= PAGE_PATHS && PAGE_PATHS + CLIP_COUNT * PATH_SIZE <= PAGE_DANCE_MODEL);
    static_assert(PAGE_DANCE_MODEL + PATH_SIZE <= PAGE_DANCE_NAMES && PAGE_DANCE_NAMES + 8 <= PAGE_SIZE);

    // Page of a call on the main thread: the code, what it gives back, the resource names it fills
    constexpr size_t JOB_SIZE = 0x4000;
    constexpr uintptr_t JOB_OUT = 0x1800;         // uint64 each
    constexpr uintptr_t JOB_NAMES = 0x1A00;       // CResourceName each
    constexpr size_t NAME_SIZE = 0xE0;            // CBufferStringN<200> & its two hashes
    constexpr uint32_t NAME_FLAGS = 0xC00000C8;   // Inline buffer of 200 bytes, as the game makes them

    // The dances, after it: what the spawn gives back, its key values & texts
    constexpr uintptr_t DANCE_OUT = 0x3000;       // uint64 the dance model, uint64 the copy, uint32 the attachment of the copy
    constexpr uintptr_t DANCE_MOVED = 0x3018;     // uint32, 1 once the move up onto our spot ran
    constexpr uintptr_t DANCE_INFO = 0x3020;      // EntitySpawnInfo_t { identity, key values, flags }
    constexpr uintptr_t DANCE_MATRIX = 0x3040;    // matrix3x4_t, the identity
    constexpr uintptr_t DANCE_KEYS = 0x3080;      // KV3MemberName { hash, -1, const char* } each
    constexpr size_t KEY_SIZE = 0x10;
    constexpr uintptr_t DANCE_TEXT = 0x3100;      // Texts, TEXT_SIZE each
    constexpr size_t TEXT_SIZE = 0x40;
    constexpr uintptr_t DANCE_AGENT = 0x3500;     // The model of our agent
    constexpr size_t AGENT_SIZE = 0x200;
    constexpr uintptr_t DANCE_POSITION = 0x3700;  // float[3]
    constexpr uintptr_t DANCE_ANGLES = 0x3710;    // float[3]
    constexpr uintptr_t DANCE_FOLDER = 0x3800;    // The folder added to the game, to the end
    static_assert(JOB_OUT + CLIP_COUNT * 8 <= JOB_NAMES && JOB_NAMES + CLIP_COUNT * NAME_SIZE <= DANCE_OUT);
    static_assert(DANCE_AGENT + AGENT_SIZE <= DANCE_POSITION && DANCE_OUT + 0x14 <= DANCE_MOVED && DANCE_MOVED + 4 <= DANCE_INFO);

    enum Text { TEXT_CLASS, TEXT_MODEL, TEXT_ORIGIN, TEXT_ANGLES, TEXT_SOLID, TEXT_SEQUENCE, TEXT_PATH_ID, TEXT_KEYS, TEXT_COUNT = TEXT_KEYS + 6 };
    enum Key { KEY_CLASS, KEY_MODEL, KEY_ORIGIN, KEY_ANGLES, KEY_SOLID, KEY_ANIMATION, KEY_COUNT };
    static_assert(DANCE_KEYS + KEY_COUNT * KEY_SIZE <= DANCE_TEXT && DANCE_TEXT + TEXT_COUNT * TEXT_SIZE <= DANCE_AGENT);

    // push rdi; push r13; push r14: the start of ApplyAnimGraphPose our jump goes over, run by the stub after
    constexpr uint8_t ORIGINAL_START[] = { 0x40, 0x57, 0x41, 0x55, 0x41, 0x56 };

    static_assert(offsets::emotes::poseBones < 0x80 && offsets::emotes::poseTransforms < 0x80 && offsets::emotes::poseState < 0x80);
    static_assert(offsets::emotes::entityIdentity < 0x80 && offsets::emotes::identityFlags < 0x80 && offsets::emotes::kvRefCount < 0x80);

    // The hash a key of CEntityKeyValues is found by: MurmurHash2 of its lowercase name, seed 0x31415926 (the
    // MakeStringToken of Source 2)
    constexpr uint32_t KeyHash(std::string_view text) {
        constexpr uint32_t M = 0x5BD1E995;
        auto lower = [&](size_t i) -> uint32_t {
            char ch = text[i];
            return static_cast<uint8_t>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        };
        size_t length = text.size(), i = 0;
        uint32_t h = 0x31415926u ^ static_cast<uint32_t>(length);
        for (; length - i >= 4; i += 4) {
            uint32_t k = lower(i) | lower(i + 1) << 8 | lower(i + 2) << 16 | lower(i + 3) << 24;
            k *= M;
            k ^= k >> 24;
            k *= M;
            h = h * M ^ k;
        }
        switch (length - i) {
        case 3: h ^= lower(i + 2) << 16; [[fallthrough]];
        case 2: h ^= lower(i + 1) << 8; [[fallthrough]];
        case 1: h ^= lower(i); h *= M;
        }
        h ^= h >> 13;
        h *= M;
        return h ^ h >> 15;
    }
    static_assert(KeyHash("classname") == 0xC61B1C62 && KeyHash("origin") == 0xE4200216 && KeyHash("angles") == 0xBA98DACF
        && KeyHash("model") == 0xC8BFBC06 && KeyHash("solid") == 0xCC9643E1 && KeyHash("DefaultAnim") == 0x87219ED5
        && KeyHash("StartingAnim") == 0xD1DB7AA5 && KeyHash("!bonemerge") == BONEMERGE, "the hash of the keys is not the game's");

    // Small x64 assembler of what the stub & the calls need
    struct Code {
        std::vector<uint8_t> bytes;

        void put(std::initializer_list<uint8_t> list) { bytes.insert(bytes.end(), list); }
        void put32(int32_t value) { for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); }
        void put64(uint64_t value) { for (int i = 0; i < 8; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); }
        size_t size() const { return bytes.size(); }

        // jcc / jmp rel32 to a label set later
        size_t jump(std::initializer_list<uint8_t> opcode) { put(opcode); put32(0); return size(); }
        void land(size_t jump_end) {
            int32_t rel = static_cast<int32_t>(size() - jump_end);
            std::memcpy(&bytes[jump_end - 4], &rel, 4);
        }
    };

    template<typename T>
    void Put(std::vector<uint8_t>& data, uintptr_t at, const T& value) {
        std::memcpy(&data[at], &value, sizeof(T));
    }

    void PutText(std::vector<uint8_t>& data, uintptr_t at, size_t room, const std::string& text) {
        std::memcpy(&data[at], text.c_str(), std::min(text.size(), room - 1));
    }

    // Scene objects the model of an entity is drawn with, -1 when that can't be read
    int SceneObjects(uintptr_t entity) {
        namespace vis = offsets::visuals;
        if (!entity || !vis::m_pSceneNode || !vis::sceneNodeCount)
            return -1;
        auto p = Engine::GetProcess();
        auto node = p->read<uintptr_t>(entity + vis::m_pSceneNode);
        return node ? p->read<int32_t>(node + vis::sceneNodeCount) : 0;
    }

    // The song of a dance in the pack, in any of its formats. Empty when it has none or it is not there
    std::filesystem::path FindSong(const std::filesystem::path& pack, const char* music) {
        std::error_code ec;
        if (pack.empty() || !music)
            return {};
        for (auto format : SONG_FORMATS) {
            auto path = pack / "music" / (std::string(music) + format);
            if (std::filesystem::exists(path, ec))
                return path;
        }
        return {};
    }

    // The folder holding csgo_addons with the dance model, in PACK_ROOT or up to PACK_DEPTH folders down (breadth
    // first). Empty when there is none
    std::filesystem::path SearchPack() {
        std::error_code ec;
        std::vector<std::filesystem::path> level{ PACK_ROOT };
        for (int depth = 0; depth <= PACK_DEPTH && !level.empty(); depth++) {
            std::vector<std::filesystem::path> next;
            for (const auto& folder : level) {
                if (std::filesystem::exists(folder / "csgo_addons" / emote_dances::MODEL_FILE, ec))
                    return folder;
                for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
                    if (entry.is_directory(ec))
                        next.push_back(entry.path());
            }
            level = std::move(next);
        }
        return {};
    }

    // Files of the pack in folder the dances use that are not there
    int CountMissing(const std::filesystem::path& folder) {
        std::error_code ec;
        int missing = std::filesystem::exists(folder / "csgo_addons" / emote_dances::MODEL_FILE, ec) ? 0 : 1;
        for (const auto& dance : emote_dances::DANCES) {
            if (!std::filesystem::exists(folder / "icons" / (std::string(dance.sequence) + ".png"), ec))
                missing++;
            if (dance.music && FindSong(folder, dance.music).empty())
                missing++;
        }
        return missing;
    }

    const char* NameOf(int id) {
        if (id >= 0 && id < CLIP_COUNT)
            return CLIPS[id].name;
        if (id >= CLIP_COUNT && id < CLIP_COUNT + DANCE_COUNT)
            return emote_dances::DANCES[id - CLIP_COUNT].name;
        return "?";
    }

    // The entity is still ours: the slot of its handle in the entity list holds it, with that handle (another serial
    // once it was removed). Each jump left in fails goes past what may only run then
    void EmitAlive(Code& c, uintptr_t list, uintptr_t entity, uint32_t handle, std::vector<size_t>& fails) {
        namespace em = offsets::emotes;
        uint32_t index = handle & 0x7FFF;
        c.put({ 0x48, 0xB8 }); c.put64(list);                                       // mov rax, &entity list
        c.put({ 0x48, 0x8B, 0x00 });                                                // mov rax, [rax]
        c.put({ 0x48, 0x85, 0xC0 });                                                // test rax, rax
        fails.push_back(c.jump({ 0x0F, 0x84 }));                                    // je fail
        c.put({ 0x48, 0x8B, 0x80 }); c.put32(static_cast<int32_t>(0x10 + 8 * (index >> 9))); // mov rax, [rax + chunk]
        c.put({ 0x48, 0x85, 0xC0 });                                                // test rax, rax
        fails.push_back(c.jump({ 0x0F, 0x84 }));                                    // je fail
        c.put({ 0x48, 0x05 }); c.put32(static_cast<int32_t>(em::identityStride * (index & 0x1FF))); // add rax, identity
        c.put({ 0x49, 0xBA }); c.put64(entity);                                     // mov r10, entity
        c.put({ 0x4C, 0x39, 0x10 });                                                // cmp [rax], r10
        fails.push_back(c.jump({ 0x0F, 0x85 }));                                    // jne fail
        c.put({ 0x81, 0x78, static_cast<uint8_t>(em::identityHandle) }); c.put32(static_cast<int32_t>(handle)); // cmp dword ptr [rax + handle], handle
        fails.push_back(c.jump({ 0x0F, 0x85 }));                                    // jne fail
    }

    // A client-only prop_dynamic_override spawned like the portrait world spawns its props: its keys in a
    // CEntityKeyValues, CreateEntityByName, its resources confirmed loaded, QueueSpawnEntity & ExecuteQueuedCreation.
    // The entity into out, 0 when it did not come. Uses rbx, rsi, rdi, r12 & r13, saved by the caller
    struct Spawn {
        uintptr_t mem_alloc;        // &g_pMemAlloc of tier0
        uintptr_t service;          // IGameResourceService
        uintptr_t job;
        std::vector<std::pair<Key, uintptr_t>> keys;   // Each with its text
        uintptr_t out;
    };
    void EmitSpawn(Code& c, const Spawn& s) {
        namespace em = offsets::emotes;
        auto client = Engine::GetClient().base;

        // kv = g_pMemAlloc->Alloc(size)
        c.put({ 0x48, 0xB9 }); c.put64(s.mem_alloc);                                // mov rcx, &g_pMemAlloc
        c.put({ 0x48, 0x8B, 0x09 });                                                // mov rcx, [rcx]
        c.put({ 0x48, 0x85, 0xC9 });                                                // test rcx, rcx
        auto no_allocator = c.jump({ 0x0F, 0x84 });                                 // je done
        c.put({ 0x48, 0x8B, 0x01 });                                                // mov rax, [rcx]
        c.put({ 0xBA }); c.put32(static_cast<int32_t>(em::kvSize));                // mov edx, size
        c.put({ 0xFF, 0x50, 0x08 });                                                // call [rax + 8] (Alloc)
        c.put({ 0x48, 0x85, 0xC0 });                                                // test rax, rax
        auto no_memory = c.jump({ 0x0F, 0x84 });                                    // je done
        c.put({ 0x48, 0x89, 0xC3 });                                                // mov rbx, rax
        c.put({ 0x31, 0xFF });                                                      // xor edi, edi

        // Constructed with no context (it allocates its own), one reference of ours
        c.put({ 0x48, 0x89, 0xD9 });                                                // mov rcx, rbx
        c.put({ 0x31, 0xD2 });                                                      // xor edx, edx
        c.put({ 0x45, 0x31, 0xC0 });                                                // xor r8d, r8d
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnKvConstruct);                 // mov rax, constructor
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        c.put({ 0x66, 0xFF, 0x43, static_cast<uint8_t>(em::kvRefCount) });         // inc word ptr [rbx + references]

        // Every key before the queue: it locks them
        for (auto [key, text] : s.keys) {
            c.put({ 0x48, 0x89, 0xD9 });                                            // mov rcx, rbx
            c.put({ 0x48, 0xBA }); c.put64(s.job + DANCE_KEYS + key * KEY_SIZE);   // mov rdx, member name
            c.put({ 0x48, 0xB8 }); c.put64(client + em::fnKvFind);                  // mov rax, FindOrCreate
            c.put({ 0xFF, 0xD0 });                                                  // call rax
            c.put({ 0x48, 0x85, 0xC0 });                                            // test rax, rax
            auto none = c.jump({ 0x0F, 0x84 });                                     // je next
            c.put({ 0x48, 0x89, 0xD9 });                                            // mov rcx, rbx
            c.put({ 0x48, 0x89, 0xC2 });                                            // mov rdx, rax
            c.put({ 0x49, 0xB8 }); c.put64(text);                                   // mov r8, text
            c.put({ 0x48, 0xB8 }); c.put64(client + em::fnKvSetString);             // mov rax, SetString
            c.put({ 0xFF, 0xD0 });                                                  // call rax
            c.land(none);
        }

        // The entity system & its spawn group
        c.put({ 0x48, 0xB8 }); c.put64(client + em::dwEntitySystem);                // mov rax, &entity system
        c.put({ 0x48, 0x8B, 0x30 });                                                // mov rsi, [rax]
        c.put({ 0x48, 0x85, 0xF6 });                                                // test rsi, rsi
        auto no_system = c.jump({ 0x0F, 0x84 });                                    // je release
        c.put({ 0x48, 0x89, 0xF1 });                                                // mov rcx, rsi
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnSpawnGroup);                  // mov rax, spawn group
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        c.put({ 0x41, 0x89, 0xC4 });                                                // mov r12d, eax

        // CreateEntityByName(system, group, class, client only, index -1, serial -1, false)
        c.put({ 0x48, 0x89, 0xF1 });                                                // mov rcx, rsi
        c.put({ 0x44, 0x89, 0xE2 });                                                // mov edx, r12d
        c.put({ 0x49, 0xB8 }); c.put64(s.job + DANCE_TEXT + TEXT_CLASS * TEXT_SIZE);// mov r8, class
        c.put({ 0x45, 0x31, 0xC9 });                                                // xor r9d, r9d
        c.put({ 0x48, 0xC7, 0x44, 0x24, 0x20 }); c.put32(-1);                       // mov qword ptr [rsp + 0x20], -1
        c.put({ 0x48, 0xC7, 0x44, 0x24, 0x28 }); c.put32(-1);                       // mov qword ptr [rsp + 0x28], -1
        c.put({ 0x48, 0xC7, 0x44, 0x24, 0x30 }); c.put32(0);                        // mov qword ptr [rsp + 0x30], 0
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnCreateEntity);                // mov rax, CreateEntityByName
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        c.put({ 0x48, 0x89, 0xC7 });                                                // mov rdi, rax
        c.put({ 0x48, 0x85, 0xFF });                                                // test rdi, rdi
        auto no_entity = c.jump({ 0x0F, 0x84 });                                    // je release
        c.put({ 0x4C, 0x8B, 0x6F, static_cast<uint8_t>(em::entityIdentity) });     // mov r13, [rdi + identity]
        c.put({ 0x4D, 0x85, 0xED });                                                // test r13, r13
        auto no_identity = c.jump({ 0x0F, 0x84 });                                  // je drop

        // Its resources confirmed loaded: { identity, key values, flags }, the identity matrix
        c.put({ 0x48, 0xB8 }); c.put64(s.job + DANCE_INFO);                         // mov rax, info
        c.put({ 0x4C, 0x89, 0x28 });                                                // mov [rax], r13
        c.put({ 0x48, 0x89, 0x58, 0x08 });                                          // mov [rax + 8], rbx
        c.put({ 0x49, 0xBA }); c.put64(SPAWN_FLAGS);                                // mov r10, flags
        c.put({ 0x4C, 0x89, 0x50, 0x10 });                                          // mov [rax + 0x10], r10
        c.put({ 0x48, 0xB9 }); c.put64(s.service);                                  // mov rcx, service
        c.put({ 0x44, 0x89, 0xE2 });                                                // mov edx, r12d
        c.put({ 0x41, 0xB8 }); c.put32(1);                                          // mov r8d, 1
        c.put({ 0x49, 0xB9 }); c.put64(s.job + DANCE_INFO);                         // mov r9, info
        c.put({ 0x48, 0xB8 }); c.put64(s.job + DANCE_MATRIX);                       // mov rax, matrix
        c.put({ 0x48, 0x89, 0x44, 0x24, 0x20 });                                    // mov [rsp + 0x20], rax
        c.put({ 0x48, 0x8B, 0x01 });                                                // mov rax, [rcx]
        c.put({ 0xFF, 0x90 }); c.put32(static_cast<int32_t>(em::precacheEntitiesIndex * 8)); // call [rax + precache]

        // Being deleted meanwhile: the game leaves it out of the queue too
        c.put({ 0x41, 0xF7, 0x45, static_cast<uint8_t>(em::identityFlags) }); c.put32(0x200); // test dword ptr [r13 + flags], 0x200
        auto deleted = c.jump({ 0x0F, 0x85 });                                      // jne drop
        c.put({ 0x48, 0x89, 0xF1 });                                                // mov rcx, rsi
        c.put({ 0x4C, 0x89, 0xEA });                                                // mov rdx, r13
        c.put({ 0x49, 0x89, 0xD8 });                                                // mov r8, rbx
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnQueueSpawn);                  // mov rax, QueueSpawnEntity
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        c.put({ 0x48, 0x89, 0xF1 });                                                // mov rcx, rsi
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnExecuteQueued);               // mov rax, ExecuteQueuedCreation
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        auto spawned = c.jump({ 0xE9 });                                            // jmp release
        c.land(no_identity);
        c.land(deleted);
        c.put({ 0x31, 0xFF });                                                      // drop: xor edi, edi
        c.land(spawned);
        c.land(no_system);
        c.land(no_entity);

        // release: our reference, the key values freed when it was the last (the queue keeps its own)
        c.put({ 0x66, 0xFF, 0x4B, static_cast<uint8_t>(em::kvRefCount) });         // dec word ptr [rbx + references]
        auto kept = c.jump({ 0x0F, 0x8F });                                         // jg kept
        c.put({ 0x48, 0x89, 0xD9 });                                                // mov rcx, rbx
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnKvDestruct);                  // mov rax, destructor
        c.put({ 0xFF, 0xD0 });                                                      // call rax
        c.put({ 0x48, 0xB9 }); c.put64(s.mem_alloc);                                // mov rcx, &g_pMemAlloc
        c.put({ 0x48, 0x8B, 0x09 });                                                // mov rcx, [rcx]
        c.put({ 0x48, 0x89, 0xDA });                                                // mov rdx, rbx
        c.put({ 0x48, 0x8B, 0x01 });                                                // mov rax, [rcx]
        c.put({ 0xFF, 0x50, 0x18 });                                                // call [rax + 0x18] (Free)
        c.land(kept);
        c.put({ 0x48, 0xB8 }); c.put64(s.out);                                      // mov rax, out
        c.put({ 0x48, 0x89, 0x38 });                                                // mov [rax], rdi
        c.land(no_allocator);
        c.land(no_memory);
    }
}

bool Emotes::Init() {
    return GetInstance().InitImpl();
}

bool Emotes::InitImpl() {
    if (!Engine::IsInsecure())
        return false;

    this->clips.assign(CLIP_COUNT, 0);
    SetStatus("Loads once you are alive in a match");
    std::thread(&Emotes::Thread, this).detach();
    return true;
}

bool Emotes::IsAvailable() {
    namespace em = offsets::emotes;
    return Engine::IsInsecure() && em::fnApplyPose && em::fnClipGetPose && em::fnResourceName && em::dwResourceSystem
        && em::fnCreateManifest && View::IsThirdPersonAvailable() && Freecam::IsAvailable() && !GetInstance().broken;
}

const char* Emotes::WhyUnavailable() {
    namespace em = offsets::emotes;
    if (!Engine::IsInsecure())
        return "Only available when the game is launched with -insecure";
    if (!em::fnApplyPose || !em::fnClipGetPose || !em::fnResourceName || !em::fnCreateManifest || GetInstance().broken)
        return "The animation code was not found in this game version";
    if (!View::IsThirdPersonAvailable())
        return "The third person code was not found in this game version";
    if (!Freecam::IsAvailable())
        return "The camera code was not found in this game version, the wheel takes the mouse through it";
    return nullptr;
}

std::string Emotes::GetStatus() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.status;
}

std::string Emotes::GetDanceStatus() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.dance_status;
}

void Emotes::SetStatus(const std::string& text) {
    std::lock_guard lock(this->status_mutex);
    this->status = text;
}

void Emotes::SetDanceStatus(const std::string& text) {
    std::lock_guard lock(this->status_mutex);
    this->dance_status = text;
}

const std::vector<Emotes::Entry>& Emotes::GetEntries(bool dances) {
    // The dances, the emotes of Fortnite, then the game's own
    static const auto lists = [] {
        std::array<std::vector<Entry>, 2> list;
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < DANCE_COUNT; k++) {
                const auto& dance = emote_dances::DANCES[k];
                if (dance.emote == (pass == 1))
                    list[1].push_back({ CLIP_COUNT + k, dance.name, dance.emote ? Kind::Emote : Kind::Dance, dance.rarity, dance.sequence });
            }
        }
        for (int k = 0; k < CLIP_COUNT; k++) {
            list[0].push_back({ k, CLIPS[k].name, Kind::Game, 0, nullptr });
            list[1].push_back({ k, CLIPS[k].name, Kind::Game, 0, nullptr });
        }
        return list;
    }();
    return lists[dances ? 1 : 0];
}

bool Emotes::CanDance() {
    return GetInstance().can_dance;
}

std::filesystem::path Emotes::GetPack() {
    auto& i = GetInstance();
    std::lock_guard lock(i.pack_mutex);
    return i.pack;
}

void Emotes::DownloadPack() {
    auto& i = GetInstance();
    if (i.downloading.exchange(true))
        return;
    std::thread(&Emotes::DownloadThread, &i).detach();
}

bool Emotes::IsDownloadingPack() {
    return GetInstance().downloading;
}

int Emotes::GetPackMissing() {
    return GetInstance().pack_missing;
}

std::string Emotes::GetPackStatus() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.pack_status;
}

bool Emotes::CanPlay() {
    return GetInstance().can_play;
}

void Emotes::Play(int id) {
    if (id >= 0 && id < CLIP_COUNT + DANCE_COUNT)
        GetInstance().request = id;
}

void Emotes::Stop() {
    GetInstance().stop_request = true;
}

bool Emotes::IsPlaying() {
    return GetInstance().playing >= 0;
}

void Emotes::Thread() {
    while (!this->stopping) {
        {
            std::lock_guard lock(this->update_mutex);
            if (!this->stopping)
                Update();
        }
        std::this_thread::sleep_for(POLL);
    }
}

void Emotes::Update() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    auto snapshot = Cache::Current();
    std::string current_map(snapshot->globals.map_name, strnlen(snapshot->globals.map_name, sizeof(snapshot->globals.map_name)));
    bool in_match = snapshot->globals.in_match;

    auto local = in_match ? Engine::GetLocalPawn() : 0;
    bool alive = local && p->read<int>(local + offsets::pawn::m_iHealth) > 0;
    bool wanted = cfg::view::emotes && IsAvailable();

    // A new map took the dance with the old one
    if (current_map != this->map) {
        this->map = current_map;
        ForgetDance();
        End("map change");
    }

    // Only alive, our own camera: the free cam & watching someone show another view
    if (!wanted || !alive || Freecam::GetMode() != Freecam::Mode::Off) {
        End(!wanted ? "turned off" : !alive ? "dead" : "camera");
        this->can_play = false;
        this->can_dance = false;
        this->request = -1;
        this->stop_request = false;
        if (wanted && this->state == LoadState::Idle)
            SetStatus("Loads once you are alive in a match");
        // The menu tells where the pack goes before a match too
        if (wanted && !this->mounted && DancesUsable())
            FindPack();
        return;
    }

    this->clips_ready = UpdateClips();
    bool dances = UpdateDances();
    this->can_dance = dances;
    this->can_play = this->clips_ready || dances;

    auto now = std::chrono::steady_clock::now();
    auto client = Engine::GetClient().base;
    auto rules = p->read<uintptr_t>(client + offsets::rules::dwGameRules);
    uint8_t round = rules ? p->read<uint8_t>(rules + offsets::rules::m_nRoundStartCount) : 0;

    if (this->stop_request.exchange(false))
        End("key");

    auto node = p->read<uintptr_t>(local + offsets::pawn::m_pGameSceneNode);
    auto origin = node ? p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) : Vec3_t{};
    bool zoomed = Visuals::IsZoomed(local);

    if (this->playing >= 0) {
        auto velocity = p->read<Vec3_t>(local + offsets::pawn::m_vecAbsVelocity);
        bool clicked = Movement::IsPlaying() && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000));

        // Zoomed (a sniper zooms in again by itself after a shot) the view is the scope, not the camera around us
        const char* why = local != this->pawn ? "new pawn"
            : round != this->round ? "new round"
            : std::hypot(velocity.x, velocity.y) > MOVING ? "moved"
            : node && (origin - this->anchor).length() > LEFT_SPOT ? "left its spot"
            : zoomed ? "zoomed"
            : clicked && now - this->started > CLICK_GRACE ? "attack"
            : std::chrono::duration<float>(now - this->started).count() >= this->length ? "finished"
            : nullptr;
        if (why)
            End(why);
        else if (this->phase != DancePhase::None)
            StepDance();
    }

    // Only standing on the ground: a dance stays where it starts, in the air it would float there after the landing
    int id = this->request.exchange(-1);
    if (id >= 0 && this->playing < 0) {
        bool on_ground = p->read<uint32_t>(local + offsets::pawn::m_fFlags) & FL_ONGROUND;
        if (!node || !on_ground || zoomed)
            LOGF(INFO, "Emote: {} not started ({})", NameOf(id), !node ? "no scene node" : !on_ground ? "not on the ground" : "zoomed");
        else if (id < CLIP_COUNT && this->clips_ready && Start(id, local)) {
            this->round = round;
            this->anchor = origin;
        }
        else if (id >= CLIP_COUNT && dances) {
            this->round = round;
            this->anchor = origin;
            BeginDance(id - CLIP_COUNT, local);
        }
    }
}

bool Emotes::EnsureService() {
    namespace em = offsets::emotes;
    if (this->service_checked)
        return this->service != 0;

    // The service the player preview loads its models with, its entries the code that was found
    auto p = Engine::GetProcess();
    auto engine = Engine::GetEngine();
    auto service = p->FindInterface("engine2.dll", "GameResourceServiceClientV001");
    auto table = service ? p->read<uintptr_t>(service) : 0;
    if (!table)
        return false;

    this->service_checked = true;
    if (p->read<uintptr_t>(table + CREATE_MANIFEST * 8) != engine.base + em::fnCreateManifest
        || p->read<uintptr_t>(table + MANIFEST_LOADED * 8) != engine.base + em::fnManifestLoaded
        || p->read<uintptr_t>(table + DESTROY_MANIFEST * 8) != engine.base + em::fnDestroyManifest) {
        LOGF(WARNING, "Emotes: the resource service of the game is not what was expected, they are disabled");
        this->broken = true;
        SetStatus("");
        return false;
    }
    this->service = service;
    return true;
}

uintptr_t Emotes::MakeManifest(uintptr_t names, int count) {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();

    // CreateGameResourceManifest(service, count, names, async, reason, priority) into the first slot out
    std::vector<uint8_t> data(JOB_SIZE, 0);
    Code c;
    c.put({ 0x48, 0x83, 0xEC, 0x38 });                                      // sub rsp, 0x38
    c.put({ 0x48, 0xB9 }); c.put64(this->service);                          // mov rcx, service
    c.put({ 0xBA }); c.put32(count);                                        // mov edx, count
    c.put({ 0x49, 0xB8 }); c.put64(names);                                  // mov r8, names
    c.put({ 0x41, 0xB9 }); c.put32(LOAD_ASYNC);                             // mov r9d, async
    c.put({ 0x48, 0xB8 }); c.put64(this->page + PAGE_REASON);               // mov rax, reason
    c.put({ 0x48, 0x89, 0x44, 0x24, 0x20 });                                // mov [rsp + 0x20], rax
    c.put({ 0x48, 0xC7, 0x44, 0x24, 0x28 }); c.put32(LOAD_PRIORITY);        // mov qword ptr [rsp + 0x28], priority
    c.put({ 0x48, 0xB8 }); c.put64(Engine::GetEngine().base + em::fnCreateManifest); // mov rax, CreateGameResourceManifest
    c.put({ 0xFF, 0xD0 });                                                  // call rax
    c.put({ 0x48, 0xB9 }); c.put64(this->job + JOB_OUT);                    // mov rcx, out
    c.put({ 0x48, 0x89, 0x01 });                                            // mov [rcx], rax
    c.put({ 0x48, 0x83, 0xC4, 0x38 });                                      // add rsp, 0x38
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    if (!CallJob(c.bytes, data))
        return 0;
    return p->read<uintptr_t>(this->job + JOB_OUT);
}

bool Emotes::UpdateClips() {
    // The manifest once, the clips of it on each map
    if (!Load())
        return false;
    if (this->found_map != this->map) {
        if (!FindClips())
            return false;
        this->found_map = this->map;
    }
    return true;
}

bool Emotes::Load() {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();

    // Again after a while: the manifest made stays, it may still finish
    if (this->state == LoadState::Failed) {
        if (now < this->next_check)
            return false;
        this->state = this->manifest ? LoadState::Loading : LoadState::Idle;
        this->load_started = now;
    }

    if (this->state == LoadState::Ready)
        return true;

    if (!EnsureService()) {
        if (!this->broken)
            Fail("The resource service of the game was not found");
        return false;
    }

    if (!GameThread::Ensure() || !BuildPage())
        return false;

    if (this->state == LoadState::Idle) {
        this->manifest = MakeManifest(this->page + PAGE_NAMES, CLIP_COUNT);
        if (!this->manifest) {
            Fail("The game did not take the load of the animations");
            return false;
        }

        LOGF(INFO, "Emotes: the game loads {} animations", CLIP_COUNT);
        SetStatus("Loading the animations...");
        this->state = LoadState::Loading;
        this->load_started = now;
        this->next_check = now;
    }

    // Loading: what IsManifestLoaded reads
    if (now < this->next_check)
        return false;
    this->next_check = now + LOAD_POLL;

    if (p->read<uintptr_t>(this->manifest + em::manifestPending) != 0) {
        if (now - this->load_started > LOAD_TIMEOUT)
            Fail("The game did not load the animations in time");
        return false;
    }

    LOGF(INFO, "Emotes: the animations are loaded ({} ms)",
        std::chrono::duration_cast<std::chrono::milliseconds>(now - this->load_started).count());
    this->state = LoadState::Ready;
    this->found_map.clear();
    return true;
}

bool Emotes::FindClips() {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient().base;

    if (!BuildPage())
        return false;

    // For each: if (CResourceName::Set(&name, path)) out = resource system->Find(&name, 0)
    std::vector<uint8_t> data(JOB_SIZE, 0);
    Code c;
    c.put({ 0x48, 0x83, 0xEC, 0x28 });                                      // sub rsp, 0x28
    for (int k = 0; k < CLIP_COUNT; k++) {
        auto name = this->job + JOB_NAMES + k * NAME_SIZE;
        Put<uint32_t>(data, JOB_NAMES + k * NAME_SIZE + 4, NAME_FLAGS);

        c.put({ 0x48, 0xB9 }); c.put64(name);                               // mov rcx, name
        c.put({ 0x48, 0xBA }); c.put64(this->page + PAGE_PATHS + k * PATH_SIZE); // mov rdx, path
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnResourceName);        // mov rax, CResourceName::Set
        c.put({ 0xFF, 0xD0 });                                              // call rax
        c.put({ 0x84, 0xC0 });                                              // test al, al
        auto unnamed = c.jump({ 0x0F, 0x84 });                              // je next
        c.put({ 0x48, 0xB8 }); c.put64(client + em::dwResourceSystem);      // mov rax, &resource system
        c.put({ 0x48, 0x8B, 0x08 });                                        // mov rcx, [rax]
        c.put({ 0x48, 0x85, 0xC9 });                                        // test rcx, rcx
        auto no_system = c.jump({ 0x0F, 0x84 });                            // je next
        c.put({ 0x48, 0xBA }); c.put64(name);                               // mov rdx, name
        c.put({ 0x45, 0x31, 0xC0 });                                        // xor r8d, r8d
        c.put({ 0x48, 0x8B, 0x01 });                                        // mov rax, [rcx]
        c.put({ 0xFF, 0x90 }); c.put32(static_cast<int32_t>(em::resourceFind)); // call [rax + find]
        c.put({ 0x48, 0xB9 }); c.put64(this->job + JOB_OUT + k * 8);        // mov rcx, out
        c.put({ 0x48, 0x89, 0x01 });                                        // mov [rcx], rax
        c.land(unnamed);
        c.land(no_system);
    }
    c.put({ 0x48, 0x83, 0xC4, 0x28 });                                      // add rsp, 0x28
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    if (c.size() > JOB_OUT) {
        Fail("The lookup of the animations is too large");
        return false;
    }

    if (!CallJob(c.bytes, data))
        return false;

    // The binding of each: its data unless it failed to load. Played only when it fits the pose the stub gets
    int usable = 0;
    for (int k = 0; k < CLIP_COUNT; k++) {
        this->clips[k] = 0;

        auto binding = p->read<uintptr_t>(this->job + JOB_OUT + k * 8);
        auto clip = binding && !(p->read<uint8_t>(binding + em::bindingFlags) & 2) ? p->read<uintptr_t>(binding) : 0;
        if (!clip)
            continue;

        auto handle = p->read<uintptr_t>(clip + em::m_skeleton);
        auto skeleton = handle ? p->read<uintptr_t>(handle) : 0;
        auto bones = skeleton ? p->read<int32_t>(skeleton + em::skeletonBones) : 0;
        auto duration = p->read<float>(clip + em::m_flDuration);
        auto frames = p->read<int32_t>(clip + em::m_nNumFrames);

        bool fits = skeleton && bones > 0 && bones < 1024 && bones == p->read<int32_t>(skeleton + em::skeletonReferenceBones)
            && !p->read<bool>(clip + em::m_bIsAdditive) && std::isfinite(duration) && duration >= 0.f && duration < 600.f
            && frames > 0 && frames < 100000;
        if (fits) {
            this->clips[k] = clip;
            usable++;
        }
        else {
            LOGF(VERBOSE, "Emotes: {} can't be played (skeleton 0x{:X}, {} bones, {:.2f} s, {} frames)", CLIPS[k].name,
                skeleton, bones, duration, frames);
        }
    }

    if (!usable) {
        Fail("The game gave none of the animations");
        return false;
    }

    LOGF(INFO, "Emotes: {} of {} animations can be played on {}", usable, CLIP_COUNT, this->map);
    SetStatus(usable < CLIP_COUNT ? std::format("{} of {} animations could be loaded", usable, CLIP_COUNT) : "");
    return true;
}

bool Emotes::BuildPage() {
    if (this->page && this->job)
        return true;

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (!this->job)
        this->job = p->allocate_remote(JOB_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->job || this->page)
        return this->job && this->page;

    // Below client.dll, close enough for a jmp rel32 from ApplyAnimGraphPose to reach it
    constexpr uintptr_t GRANULARITY = 0x10000;
    uintptr_t page = 0;
    for (uintptr_t below = GRANULARITY; below < 0x70000000 && !page; below += GRANULARITY) {
        auto at = (client.base & ~(GRANULARITY - 1)) - below;
        page = reinterpret_cast<uintptr_t>(VirtualAllocEx(p->handle_, reinterpret_cast<void*>(at), PAGE_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (!page) {
        Fail("No memory near the game code for the emotes");
        return false;
    }

    namespace em = offsets::emotes;
    const uintptr_t start = client.base + em::fnApplyPose;
    std::vector<uint8_t> data(PAGE_SIZE, 0);

    // rcx the skeleton instance (a scene node), rdx the pose. Ours while one plays: the pose of our player, of the
    // skeleton of the clip with all of its bones, set
    Code c;
    std::vector<size_t> to_original;
    c.put({ 0x48, 0xB8 }); c.put64(page);                                   // mov rax, page
    c.put({ 0x83, 0xB8 }); c.put32(DATA_ACTIVE); c.put({ 0x00 });           // cmp dword ptr [rax + active], 0
    to_original.push_back(c.jump({ 0x0F, 0x84 }));                          // je original
    c.put({ 0x48, 0x85, 0xC9 });                                            // test rcx, rcx
    to_original.push_back(c.jump({ 0x0F, 0x84 }));                          // je original
    c.put({ 0x4C, 0x8B, 0x91 }); c.put32(static_cast<int32_t>(offsets::econ::m_pOwner)); // mov r10, [rcx + owner]
    c.put({ 0x4C, 0x3B, 0x90 }); c.put32(DATA_PAWN);                        // cmp r10, [rax + pawn]
    to_original.push_back(c.jump({ 0x0F, 0x85 }));                          // jne original
    c.put({ 0x48, 0x85, 0xD2 });                                            // test rdx, rdx
    to_original.push_back(c.jump({ 0x0F, 0x84 }));                          // je original
    c.put({ 0x4C, 0x8B, 0x12 });                                            // mov r10, [rdx] (its skeleton)
    c.put({ 0x4C, 0x3B, 0x90 }); c.put32(DATA_SKELETON);                    // cmp r10, [rax + skeleton]
    to_original.push_back(c.jump({ 0x0F, 0x85 }));                          // jne original
    c.put({ 0x44, 0x8B, 0x52, static_cast<uint8_t>(em::poseBones) });       // mov r10d, [rdx + bones]
    c.put({ 0x44, 0x3B, 0x90 }); c.put32(DATA_BONES);                       // cmp r10d, [rax + bones]
    to_original.push_back(c.jump({ 0x0F, 0x85 }));                          // jne original
    c.put({ 0x48, 0x83, 0x7A, static_cast<uint8_t>(em::poseTransforms), 0x00 }); // cmp qword ptr [rdx + transforms], 0
    to_original.push_back(c.jump({ 0x0F, 0x84 }));                          // je original
    c.put({ 0x83, 0x7A, static_cast<uint8_t>(em::poseState), 0x00 });       // cmp dword ptr [rdx + state], 0
    to_original.push_back(c.jump({ 0x0F, 0x84 }));                          // je original

    // GetPose(clip, percent, pose, all bones, no float channels): the percent is the time since the start over the
    // length, 0 to 1, the game plays it once
    c.put({ 0x51, 0x52 });                                                  // push rcx; push rdx
    c.put({ 0x48, 0x83, 0xEC, 0x28 });                                      // sub rsp, 0x28
    c.put({ 0x0F, 0x57, 0xC0 });                                            // xorps xmm0, xmm0
    c.put({ 0x4C, 0x8B, 0x90 }); c.put32(DATA_GLOBALS);                     // mov r10, [rax + globals]
    c.put({ 0x4D, 0x8B, 0x12 });                                            // mov r10, [r10]
    c.put({ 0x4D, 0x85, 0xD2 });                                            // test r10, r10
    auto no_globals = c.jump({ 0x0F, 0x84 });                               // je sample
    c.put({ 0xF3, 0x41, 0x0F, 0x10, 0x02 });                                // movss xmm0, [r10] (real time)
    c.put({ 0xF3, 0x0F, 0x5C, 0x80 }); c.put32(DATA_START);                 // subss xmm0, [rax + start]
    c.put({ 0xF3, 0x0F, 0x59, 0x80 }); c.put32(DATA_RATE);                  // mulss xmm0, [rax + rate]
    c.put({ 0xF3, 0x0F, 0x5F, 0x80 }); c.put32(CONST_ZERO);                 // maxss xmm0, [rax + 0]
    c.put({ 0xF3, 0x0F, 0x5D, 0x80 }); c.put32(CONST_ONE);                  // minss xmm0, [rax + 1]
    c.land(no_globals);
    c.put({ 0x49, 0x89, 0xD0 });                                            // mov r8, rdx
    c.put({ 0x48, 0x8B, 0x88 }); c.put32(DATA_CLIP);                        // mov rcx, [rax + clip]
    c.put({ 0x66, 0x0F, 0x7E, 0xC2 });                                      // movd edx, xmm0
    c.put({ 0x41, 0xB9 }); c.put32(1);                                      // mov r9d, 1
    c.put({ 0x48, 0xC7, 0x44, 0x24, 0x20 }); c.put32(0);                    // mov qword ptr [rsp + 0x20], 0
    c.put({ 0xFF, 0x90 }); c.put32(DATA_GET_POSE);                          // call [rax + GetPose]

    // The clips of the menu walk away from their spot: x & y of the root stay where the player is
    c.put({ 0x48, 0x8B, 0x54, 0x24, 0x28 });                                // mov rdx, [rsp + 0x28] (the pose)
    c.put({ 0x4C, 0x8B, 0x52, static_cast<uint8_t>(em::poseTransforms) });  // mov r10, [rdx + transforms]
    c.put({ 0x4D, 0x85, 0xD2 });                                            // test r10, r10
    auto no_root = c.jump({ 0x0F, 0x84 });                                  // je restore
    c.put({ 0x49, 0xC7, 0x02 }); c.put32(0);                                // mov qword ptr [r10], 0
    c.land(no_root);
    c.put({ 0x48, 0x83, 0xC4, 0x28 });                                      // add rsp, 0x28
    c.put({ 0x5A, 0x59 });                                                  // pop rdx; pop rcx

    // original: what our jump went over, then the rest of it
    for (auto j : to_original)
        c.land(j);
    c.put({ ORIGINAL_START[0], ORIGINAL_START[1], ORIGINAL_START[2], ORIGINAL_START[3], ORIGINAL_START[4], ORIGINAL_START[5] });
    c.put({ 0xE9 });                                                        // jmp start + 6
    c.put32(static_cast<int32_t>(static_cast<int64_t>(start + sizeof(ORIGINAL_START)) - static_cast<int64_t>(page + STUB + c.size() + 4)));

    if (STUB + c.size() > DATA_ACTIVE) {
        LOGF(WARNING, "Emotes: the stub is too large");
        this->broken = true;
        return false;
    }
    std::copy(c.bytes.begin(), c.bytes.end(), data.begin() + STUB);

    Put<float>(data, CONST_ZERO, 0.f);
    Put<float>(data, CONST_ONE, 1.f);
    Put<uint64_t>(data, DATA_GLOBALS, client.base + offsets::globalVars);
    Put<uint64_t>(data, DATA_GET_POSE, client.base + em::fnClipGetPose);

    std::memcpy(&data[PAGE_REASON], "emotes", 7);
    for (int k = 0; k < CLIP_COUNT; k++) {
        auto at = PAGE_PATHS + k * PATH_SIZE;
        std::memcpy(&data[at], CLIPS[k].path, std::min(std::strlen(CLIPS[k].path), PATH_SIZE - 1));
        Put<uint64_t>(data, PAGE_NAMES + k * 8, page + at);
    }
    PutText(data, PAGE_DANCE_MODEL, PATH_SIZE, emote_dances::MODEL);
    Put<uint64_t>(data, PAGE_DANCE_NAMES, page + PAGE_DANCE_MODEL);

    p->write_bytes(page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(page), PAGE_SIZE);
    this->page = page;
    return true;
}

bool Emotes::Install() {
    if (this->installed)
        return true;
    if (!this->page || this->broken)
        return false;

    auto p = Engine::GetProcess();
    auto start = Engine::GetClient().base + offsets::emotes::fnApplyPose;

    uint8_t bytes[sizeof(ORIGINAL_START)]{};
    if (!p->read_raw(start, bytes, sizeof(bytes)) || !std::equal(std::begin(bytes), std::end(bytes), std::begin(ORIGINAL_START))) {
        LOGF(WARNING, "Emotes: the start of ApplyAnimGraphPose is not what was expected, they are disabled");
        this->broken = true;
        return false;
    }

    // jmp rel32 to the stub & a nop, over the first 6 bytes
    auto distance = static_cast<int64_t>(this->page + STUB) - static_cast<int64_t>(start + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) {
        LOGF(WARNING, "Emotes: the stub is out of reach of the game code, they are disabled");
        this->broken = true;
        return false;
    }

    uint8_t jump[sizeof(ORIGINAL_START)] = { 0xE9, 0, 0, 0, 0, 0x90 };
    auto rel = static_cast<int32_t>(distance);
    std::memcpy(&jump[1], &rel, sizeof(rel));
    if (!p->patch_code(start, jump, sizeof(jump))) {
        LOGF(WARNING, "Emotes: failed to put the jump to the stub ({})", GetLastError());
        return false;
    }

    this->installed = true;
    LOGF(INFO, "Emotes: the poses go through our stub");
    return true;
}

bool Emotes::CallJob(const std::vector<uint8_t>& code, std::vector<uint8_t> data) {
    auto p = Engine::GetProcess();
    if (!this->job || code.size() > data.size())
        return false;

    std::copy(code.begin(), code.end(), data.begin());
    p->write_bytes(this->job, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->job), data.size());

    if (!GameThread::Call(this->job, 1000)) {
        // Might still run later, never written over: another page next time
        LOGF(WARNING, "Emotes: the call did not run in time");
        this->job = 0;
        return false;
    }
    return true;
}

bool Emotes::Start(int emote, uintptr_t local) {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();

    if (emote < 0 || emote >= CLIP_COUNT || !this->clips[emote] || !Install())
        return false;

    auto clip = this->clips[emote];
    auto handle = p->read<uintptr_t>(clip + em::m_skeleton);
    auto skeleton = handle ? p->read<uintptr_t>(handle) : 0;
    auto bones = skeleton ? p->read<int32_t>(skeleton + em::skeletonBones) : 0;
    auto duration = p->read<float>(clip + em::m_flDuration);
    auto frames = p->read<int32_t>(clip + em::m_nNumFrames);
    if (!local || !skeleton || bones <= 0 || !std::isfinite(duration))
        return false;

    auto globals = p->read<uintptr_t>(Engine::GetClient().base + offsets::globalVars);
    float realtime = globals ? p->read<float>(globals) : 0.f;
    bool still = duration < STILL_CLIP || frames < 2;

    // Everything before it is turned on
    p->write<uint32_t>(this->page + DATA_ACTIVE, 0);
    p->write<uint64_t>(this->page + DATA_PAWN, local);
    p->write<uint64_t>(this->page + DATA_SKELETON, skeleton);
    p->write<uint32_t>(this->page + DATA_BONES, static_cast<uint32_t>(bones));
    p->write<float>(this->page + DATA_START, realtime);
    p->write<uint64_t>(this->page + DATA_CLIP, clip);
    p->write<float>(this->page + DATA_RATE, still ? 0.f : 1.f / duration);
    p->write<uint32_t>(this->page + DATA_ACTIVE, 1);

    this->pawn = local;
    this->started = std::chrono::steady_clock::now();
    this->length = still ? POSE_HOLD : duration;
    this->playing = emote;
    Freecam::SetEmoteCamera(true);   // The mouse turns the camera around us, not our player
    LOGF(INFO, "Emote: {} ({:.1f} s)", CLIPS[emote].name, this->length);
    return true;
}

void Emotes::End(const char* why) {
    if (this->playing < 0)
        return;

    if (this->phase != DancePhase::None)
        EndDance();
    else if (this->page)
        Engine::GetProcess()->write<uint32_t>(this->page + DATA_ACTIVE, 0);
    Freecam::SetEmoteCamera(false);

    LOGF(INFO, "Emote ended: {} ({})", NameOf(this->playing), why);
    this->playing = -1;
    this->pawn = 0;
}

void Emotes::Fail(const std::string& why) {
    LOGF(WARNING, "Emotes: {}, tried again in {} s", why, std::chrono::duration_cast<std::chrono::seconds>(RETRY).count());
    SetStatus(why + ", tried again soon");
    this->state = LoadState::Failed;
    this->next_check = std::chrono::steady_clock::now() + RETRY;
}

bool Emotes::DancesUsable() {
    namespace em = offsets::emotes;
    if (this->dances_broken)
        return false;
    if (!em::fnCreateEntity || !em::fnPrecacheEntities || !em::dwEntitySystem) {
        SetDanceStatus("The dance code was not found in this game version");
        return false;
    }
    if (!MaterialChams::IsAvailable()) {
        SetDanceStatus("The drawing code of the game was not found, your player could not be hidden while dancing");
        return false;
    }
    return true;
}

void Emotes::FindPack() {
    auto now = std::chrono::steady_clock::now();
    bool changed = this->pack_changed.exchange(false);
    if (now < this->pack_checked && !changed)
        return;
    this->pack_checked = now + PACK_CHECK;

    std::error_code ec;
    std::filesystem::create_directories(PACK_ROOT, ec);
    auto found = SearchPack();

    // What the download button of the menu would bring
    this->pack_missing = CountMissing(found.empty() ? PACK_ROOT : found);

    {
        std::lock_guard lock(this->pack_mutex);
        bool told = this->pack_told;
        if (found != this->pack || !told) {
            this->pack = found;
            this->pack_told = true;
            auto text = std::filesystem::absolute(found.empty() ? PACK_ROOT : found, ec).make_preferred().u8string();
            if (found.empty())
                LOGF(INFO, "Emotes: no Emote Pack yet, the Fortnite dances come once it is downloaded or extracted into {}", std::string(text.begin(), text.end()));
            else
                LOGF(INFO, "Emotes: the Emote Pack is in {}", std::string(text.begin(), text.end()));
        }
    }

    if (found.empty()) {
        SetDanceStatus(NO_PACK);
        return;
    }
    std::lock_guard lock(this->status_mutex);
    if (this->dance_status == NO_PACK)
        this->dance_status.clear();
}

void Emotes::DownloadThread() {
    auto set_status = [&](const std::string& text) {
        std::lock_guard lock(this->status_mutex);
        this->pack_status = text;
    };
    auto finish = [&](const std::string& text) {
        set_status(text);
        this->downloading = false;
    };

    // Into the pack found (it may be extracted a few folders down, the loader asks before the dances looked for it).
    // Nothing missing: no need for the network
    auto folder = GetPack();
    if (folder.empty())
        folder = SearchPack();
    if (folder.empty())
        folder = PACK_ROOT;
    this->pack_missing = CountMissing(folder);
    if (this->pack_missing == 0) {
        finish("");
        return;
    }

    set_status("Getting the list of its files...");
    nlohmann::json tree;
    int http_status = HttpHelper::Get(PACK_TREE, tree);
    if (http_status != 200 || !tree.is_object() || !tree.contains("tree") || !tree["tree"].is_array()) {
        LOGF(WARNING, "Emotes: could not get the files of the Emote Pack (status {})", http_status);
        finish(std::format("Could not get the list of its files (status {}), Download in Emotes tries again", http_status));
        return;
    }

    // What the dances use, by its path in the pack: the icons, the model, the songs by name in any format
    std::unordered_map<std::string, int> wanted{ { std::string("csgo_addons/") + emote_dances::MODEL_FILE, 1 } };
    std::unordered_set<std::string> songs;
    for (const auto& dance : emote_dances::DANCES) {
        wanted.emplace(std::string("icons/") + dance.sequence + ".png", 0);
        if (dance.music)
            songs.insert(dance.music);
    }

    struct File {
        std::string path;
        uint64_t size;
        int order;      // 0 icon, 1 model, 2 song
        size_t format;  // Of a song, in SONG_FORMATS
    };
    std::vector<File> files;
    std::unordered_map<std::string, size_t> song_files;    // Name of a song, its entry in files
    for (const auto& item : tree["tree"]) {
        if (!item.is_object() || item.value("type", "") != "blob")
            continue;
        auto repo_path = item.value("path", "");
        if (!repo_path.starts_with(PACK_REPOSITORY))
            continue;

        // Plain names only, nothing out of the folder
        auto path = repo_path.substr(PACK_REPOSITORY.size());
        if (path.empty() || path.find("..") != std::string::npos
            || path.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./") != std::string::npos)
            continue;
        uint64_t size = item.value("size", uint64_t{ 0 });

        if (auto it = wanted.find(path); it != wanted.end()) {
            files.push_back({ path, size, it->second, 0 });
            continue;
        }

        // A song: one per name, the format coming first in SONG_FORMATS
        auto file = std::filesystem::path(path);
        if (!path.starts_with("music/") || !songs.contains(file.stem().string()))
            continue;
        auto extension = file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        size_t format = std::find_if(std::begin(SONG_FORMATS), std::end(SONG_FORMATS), [&](const char* f) { return extension == f; }) - std::begin(SONG_FORMATS);
        if (format == std::size(SONG_FORMATS))
            continue;
        auto [it, added] = song_files.emplace(file.stem().string(), files.size());
        if (added)
            files.push_back({ path, size, 2, format });
        else if (format < files[it->second].format)
            files[it->second] = { path, size, 2, format };
    }
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.order < b.order; });

    // What is there already kept: a song in another format too
    std::erase_if(files, [&](const File& file) {
        std::error_code ec;
        if (file.order == 2)
            return !FindSong(folder, std::filesystem::path(file.path).stem().string().c_str()).empty();
        return std::filesystem::exists(folder / file.path, ec);
    });

    uint64_t total = 0, done = 0;
    for (const auto& file : files)
        total += file.size;
    auto megabytes = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };

    int added = 0, failed = 0;
    for (const auto& file : files) {
        if (this->stopping)
            break;

        // Under another name until it is whole: a file of the pack is never half there
        std::error_code ec;
        auto target = folder / file.path;
        auto part = target;
        part += ".part";
        std::filesystem::create_directories(target.parent_path(), ec);
        int status = HttpHelper::Download(PACK_FILES + std::string(PACK_REPOSITORY) + file.path, part, PACK_FILE_MAX,
            [&](uint64_t now, uint64_t) {
                set_status(std::format("Downloading {:.1f} / {:.1f} MB", megabytes(done + now), megabytes(total)));
                return !this->stopping;
            });
        if (status == 200)
            std::filesystem::rename(part, target, ec);
        if (status != 200 || ec) {
            LOGF(WARNING, "Emotes: could not download {} of the Emote Pack (status {})", file.path, status);
            std::filesystem::remove(part, ec);
            failed++;
            continue;
        }

        added++;
        done += file.size;
        // The icons & the model there: the dances load while the songs come
        if (file.order == 1)
            this->pack_changed = true;
    }
    if (this->stopping) {
        this->downloading = false;
        return;
    }

    this->pack_missing = CountMissing(folder);
    this->pack_changed = true;

    std::error_code ec;
    auto text = std::filesystem::absolute(folder, ec).make_preferred().u8string();
    LOGF(INFO, "Emotes: {} files of the Emote Pack downloaded into {}, {} failed", added, std::string(text.begin(), text.end()), failed);
    if (failed)
        finish(std::format("{} of its files could not be downloaded, Download in Emotes tries again", failed));
    else if (!added)
        finish("Some of its files are not in the download");
    else
        finish("");
}

bool Emotes::MountPack() {
    if (this->mounted)
        return true;

    auto p = Engine::GetProcess();
    auto file_system = p->FindInterface("filesystem_stdio.dll", "VFileSystem017");
    if (!file_system || !GameThread::Ensure() || !BuildPage()) {
        DanceFail("The file system of the game was not found");
        return false;
    }

    std::error_code ec;
    auto folder = std::filesystem::absolute(GetPack() / "csgo_addons", ec).make_preferred().u8string();
    std::string text(folder.begin(), folder.end());
    if (ec || text.size() + 1 > JOB_SIZE - DANCE_FOLDER) {
        DanceFail("The folder of the Emote Pack can't be used");
        return false;
    }

    // file system->AddSearchPath(folder, "GAME", to the tail, priority, 0): the model is found like one of the game's
    std::vector<uint8_t> data(JOB_SIZE, 0);
    PutText(data, DANCE_FOLDER, JOB_SIZE - DANCE_FOLDER, text);
    PutText(data, DANCE_TEXT + TEXT_PATH_ID * TEXT_SIZE, TEXT_SIZE, "GAME");

    auto slot = static_cast<int32_t>(offsets::sounds::fileSystemAddSearchPath * 8);
    Code c;
    c.put({ 0x48, 0x83, 0xEC, 0x38 });                                      // sub rsp, 0x38
    c.put({ 0x48, 0xB9 }); c.put64(file_system);                            // mov rcx, file system
    c.put({ 0x48, 0xBA }); c.put64(this->job + DANCE_FOLDER);               // mov rdx, folder
    c.put({ 0x49, 0xB8 }); c.put64(this->job + DANCE_TEXT + TEXT_PATH_ID * TEXT_SIZE); // mov r8, "GAME"
    c.put({ 0x41, 0xB9 }); c.put32(ADD_TO_TAIL);                            // mov r9d, to the tail
    c.put({ 0x48, 0xC7, 0x44, 0x24, 0x20 }); c.put32(SEARCH_PRIORITY);      // mov qword ptr [rsp + 0x20], priority
    c.put({ 0x48, 0xC7, 0x44, 0x24, 0x28 }); c.put32(0);                    // mov qword ptr [rsp + 0x28], 0
    c.put({ 0x48, 0x8B, 0x01 });                                            // mov rax, [rcx]
    c.put({ 0xFF, 0x90 }); c.put32(slot);                                   // call [rax + AddSearchPath]
    c.put({ 0x48, 0x83, 0xC4, 0x38 });                                      // add rsp, 0x38
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    if (!CallJob(c.bytes, data)) {
        DanceFail("The game did not take the folder of the Emote Pack");
        return false;
    }

    this->mounted = true;
    LOGF(INFO, "Emotes: the game finds the files of the Emote Pack in {}", text);
    Sounds::PrepareMusic();
    return true;
}

bool Emotes::LoadDanceModel() {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();
    auto now = std::chrono::steady_clock::now();

    if (this->dance_state == LoadState::Ready)
        return true;

    if (!EnsureService()) {
        DanceFail("The resource service of the game was not found");
        return false;
    }

    // The one call of the spawn that goes through a vtable, & the entity system of the spawn the one of the list
    auto table = p->read<uintptr_t>(this->service);
    auto client = Engine::GetClient().base;
    auto system = p->read<uintptr_t>(client + em::dwEntitySystem);
    auto list = p->read<uintptr_t>(client + offsets::entityList);
    if (p->read<uintptr_t>(table + em::precacheEntitiesIndex * 8) != Engine::GetEngine().base + em::fnPrecacheEntities
        || !system || system != list) {
        LOGF(WARNING, "Emotes: the spawn of the game is not what was expected (system 0x{:X}, list 0x{:X}), no Fortnite dances", system, list);
        this->dances_broken = true;
        SetDanceStatus("The dance code of this game version is not what was expected");
        return false;
    }

    // The key values of a spawn are allocated & freed by the allocator of the game
    if (!this->mem_alloc)
        this->mem_alloc = p->FindExport("tier0.dll", "g_pMemAlloc");
    if (!this->mem_alloc) {
        DanceFail("The memory allocator of the game was not found");
        return false;
    }

    if (this->dance_state == LoadState::Idle) {
        this->dance_manifest = MakeManifest(this->page + PAGE_DANCE_NAMES, 1);
        if (!this->dance_manifest) {
            DanceFail("The game did not take the load of the dance model");
            return false;
        }

        LOGF(INFO, "Emotes: the game loads the dance model");
        SetDanceStatus("Loading the dances...");
        this->dance_state = LoadState::Loading;
        this->dance_load_started = now;
        this->dance_next_check = now;
    }

    if (now < this->dance_next_check)
        return false;
    this->dance_next_check = now + LOAD_POLL;

    if (p->read<uintptr_t>(this->dance_manifest + em::manifestPending) != 0) {
        if (now - this->dance_load_started > LOAD_TIMEOUT)
            DanceFail("The game did not load the dance model in time");
        return false;
    }

    LOGF(INFO, "Emotes: the dance model is loaded ({} ms)",
        std::chrono::duration_cast<std::chrono::milliseconds>(now - this->dance_load_started).count());
    this->dance_state = LoadState::Ready;
    SetDanceStatus("");
    return true;
}

bool Emotes::UpdateDances() {
    if (!DancesUsable())
        return false;
    if (!this->mounted)
        FindPack();
    if (GetPack().empty())
        return false;

    // Again after a while: a manifest made stays, it may still finish
    if (this->dance_state == LoadState::Failed) {
        auto now = std::chrono::steady_clock::now();
        if (now < this->dance_next_check)
            return false;
        this->dance_state = this->dance_manifest ? LoadState::Loading : LoadState::Idle;
        this->dance_load_started = now;
    }
    return MountPack() && LoadDanceModel();
}

void Emotes::DanceFail(const std::string& why) {
    LOGF(WARNING, "Emotes: {}, tried again in {} s", why, std::chrono::duration_cast<std::chrono::seconds>(RETRY).count());
    SetDanceStatus(why + ", tried again soon");
    this->dance_state = LoadState::Failed;
    this->dance_next_check = std::chrono::steady_clock::now() + RETRY;
}

bool Emotes::DanceAlive(uintptr_t entity, uint32_t handle) {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();
    if (!entity || !handle || Engine::GetEntityFromHandle(handle) != entity)
        return false;
    auto identity = p->read<uintptr_t>(entity + em::entityIdentity);
    return identity && p->read<uint32_t>(identity + em::identityHandle) == handle;
}

void Emotes::BeginDance(int index, uintptr_t local) {
    if (index < 0 || index >= DANCE_COUNT || !local)
        return;

    // Our player hidden first (the hook may still have to learn how a mesh names its owner, from our player drawn in
    // third person): the copy dancing in its place comes once it is
    this->dance = index;
    this->phase = DancePhase::Hiding;
    this->pawn = local;
    this->started = std::chrono::steady_clock::now();
    this->length = FLT_MAX;
    this->playing = CLIP_COUNT + index;
    MaterialChams::SetDancer(local, 0);
    MaterialChams::Refresh();
    Freecam::SetEmoteCamera(true);

    StepDance();
}

bool Emotes::SpawnDance() {
    namespace em = offsets::emotes;
    auto p = Engine::GetProcess();
    const auto& dance = emote_dances::DANCES[this->dance];

    if (!BuildPage())
        return false;

    // Where our player stands & looks, & its agent
    auto node = p->read<uintptr_t>(this->pawn + offsets::pawn::m_pGameSceneNode);
    if (!node)
        return false;
    auto origin = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin);
    auto yaw = p->read<float>(node + offsets::grenade::m_angAbsRotation + 4);
    auto model = p->read<uintptr_t>(node + offsets::bone::m_modelState + offsets::econ::m_ModelName);
    char agent[AGENT_SIZE]{};
    if (!model || !p->read_raw(model, agent, sizeof(agent) - 1) || !agent[0] || !std::isfinite(origin.z) || !std::isfinite(yaw))
        return false;
    agent[sizeof(agent) - 1] = 0;

    // Spawned under the floor & moved up once the dance model is hidden: neither shows on our spot before
    std::vector<uint8_t> data(JOB_SIZE, 0);
    auto text = [&](Text slot, const std::string& value) { PutText(data, DANCE_TEXT + slot * TEXT_SIZE, TEXT_SIZE, value); };
    text(TEXT_CLASS, "prop_dynamic_override");      // Non-solid with solid 0, kept even with bad prop data
    text(TEXT_MODEL, emote_dances::MODEL);
    text(TEXT_ORIGIN, std::format("{:.2f} {:.2f} {:.2f}", origin.x, origin.y, origin.z - SPAWN_DROP));
    text(TEXT_ANGLES, std::format("0 {:.2f} 0", yaw));
    text(TEXT_SOLID, "0");
    text(TEXT_SEQUENCE, dance.sequence);
    PutText(data, DANCE_AGENT, AGENT_SIZE, agent);

    // The first sequence plays once then the loop after it is set, the others loop from the start
    constexpr const char* KEY_NAMES[KEY_COUNT] = { "classname", "model", "origin", "angles", "solid", "DefaultAnim" };
    for (int k = 0; k < KEY_COUNT; k++) {
        const char* name = k == KEY_ANIMATION && dance.after ? "StartingAnim" : KEY_NAMES[k];
        auto at = DANCE_TEXT + (TEXT_KEYS + k) * TEXT_SIZE;
        PutText(data, at, TEXT_SIZE, name);
        Put<uint32_t>(data, DANCE_KEYS + k * KEY_SIZE, KeyHash(name));
        Put<int32_t>(data, DANCE_KEYS + k * KEY_SIZE + 4, -1);
        Put<uint64_t>(data, DANCE_KEYS + k * KEY_SIZE + 8, this->job + at);
    }
    constexpr float IDENTITY[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    std::memcpy(&data[DANCE_MATRIX], IDENTITY, sizeof(IDENTITY));

    Code c;
    c.put({ 0x53, 0x56, 0x57 });                                            // push rbx; push rsi; push rdi
    c.put({ 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 });              // push r12; push r13; push r14; push r15
    c.put({ 0x48, 0x83, 0xEC, 0x40 });                                      // sub rsp, 0x40

    // The dance model playing it, then the copy of our agent
    auto at = [&](Text slot) { return this->job + DANCE_TEXT + slot * TEXT_SIZE; };
    std::vector<std::pair<Key, uintptr_t>> keys = { { KEY_CLASS, at(TEXT_CLASS) }, { KEY_MODEL, at(TEXT_MODEL) },
        { KEY_ORIGIN, at(TEXT_ORIGIN) }, { KEY_ANGLES, at(TEXT_ANGLES) }, { KEY_SOLID, at(TEXT_SOLID) }, { KEY_ANIMATION, at(TEXT_SEQUENCE) } };
    EmitSpawn(c, { this->mem_alloc, this->service, this->job, keys, this->job + DANCE_OUT });
    keys.pop_back();
    keys[KEY_MODEL].second = this->job + DANCE_AGENT;
    EmitSpawn(c, { this->mem_alloc, this->service, this->job, keys, this->job + DANCE_OUT + 8 });

    // FollowEntity(copy, dance model, bonemerge): its bones follow the ones of the dance. Its attachment tells it took
    std::vector<size_t> skip;
    auto client = Engine::GetClient().base;
    c.put({ 0x48, 0xB8 }); c.put64(this->job + DANCE_OUT + 8);              // mov rax, copy
    c.put({ 0x48, 0x8B, 0x08 });                                            // mov rcx, [rax]
    c.put({ 0x48, 0x85, 0xC9 });                                            // test rcx, rcx
    skip.push_back(c.jump({ 0x0F, 0x84 }));                                 // je done
    c.put({ 0x48, 0xB8 }); c.put64(this->job + DANCE_OUT);                  // mov rax, dance model
    c.put({ 0x48, 0x8B, 0x10 });                                            // mov rdx, [rax]
    c.put({ 0x48, 0x85, 0xD2 });                                            // test rdx, rdx
    skip.push_back(c.jump({ 0x0F, 0x84 }));                                 // je done
    c.put({ 0x41, 0xB8 }); c.put32(1);                                      // mov r8d, 1
    c.put({ 0x48, 0xB8 }); c.put64(client + em::fnFollowEntity);            // mov rax, FollowEntity
    c.put({ 0xFF, 0xD0 });                                                  // call rax
    c.put({ 0x48, 0xB8 }); c.put64(this->job + DANCE_OUT + 8);              // mov rax, copy
    c.put({ 0x48, 0x8B, 0x00 });                                            // mov rax, [rax]
    c.put({ 0x48, 0x8B, 0x80 }); c.put32(static_cast<int32_t>(offsets::pawn::m_pGameSceneNode)); // mov rax, [rax + scene node]
    c.put({ 0x48, 0x85, 0xC0 });                                            // test rax, rax
    skip.push_back(c.jump({ 0x0F, 0x84 }));                                 // je done
    c.put({ 0x8B, 0x80 }); c.put32(static_cast<int32_t>(offsets::econ::m_hierarchyAttachName)); // mov eax, [rax + attachment]
    c.put({ 0x48, 0xB9 }); c.put64(this->job + DANCE_OUT + 0x10);           // mov rcx, out
    c.put({ 0x89, 0x01 });                                                  // mov [rcx], eax
    for (auto j : skip)
        c.land(j);

    c.put({ 0x48, 0x83, 0xC4, 0x40 });                                      // add rsp, 0x40
    c.put({ 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C });              // pop r15; pop r14; pop r13; pop r12
    c.put({ 0x5F, 0x5E, 0x5B });                                            // pop rdi; pop rsi; pop rbx
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    if (c.size() > JOB_OUT || !CallJob(c.bytes, data))
        return false;

    this->prop = p->read<uintptr_t>(this->job + DANCE_OUT);
    this->clone = p->read<uintptr_t>(this->job + DANCE_OUT + 8);
    auto attachment = p->read<uint32_t>(this->job + DANCE_OUT + 0x10);
    auto handle_of = [&](uintptr_t entity) {
        auto identity = entity ? p->read<uintptr_t>(entity + em::entityIdentity) : 0;
        return identity ? p->read<uint32_t>(identity + em::identityHandle) : 0u;
    };
    this->prop_handle = handle_of(this->prop);
    this->clone_handle = handle_of(this->clone);

    if (!this->prop || !this->prop_handle) {
        LOGF(WARNING, "Emotes: the game did not spawn the dance model");
        return false;
    }

    // A copy that did not follow would stand still under the floor: gone, the dance model shows instead
    if (this->clone && attachment != BONEMERGE) {
        LOGF(WARNING, "Emotes: the copy of the agent did not follow the dance (0x{:X}), the dance model shows", attachment);
        RemoveProps({ { this->clone, this->clone_handle } });
        this->clone = 0;
        this->clone_handle = 0;
    }

    // The dance model hidden behind its copy from the next frame, then both up onto our spot. Their scene objects may
    // only come with their first frame: hidden once they are there
    MaterialChams::SetDancer(this->pawn, this->clone ? this->prop : 0, this->clone);
    if (this->clone) {
        for (auto until = std::chrono::steady_clock::now() + SCENE_WAIT; std::chrono::steady_clock::now() < until;) {
            if (SceneObjects(this->prop) != 0 && SceneObjects(this->clone) != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    this->clone_drawn = this->clone && SceneObjects(this->clone) > 0;
    MaterialChams::Refresh();

    data.assign(JOB_SIZE, 0);
    float position[3] = { origin.x, origin.y, origin.z };
    float angles[3] = { 0.f, yaw, 0.f };
    std::memcpy(&data[DANCE_POSITION], position, sizeof(position));
    std::memcpy(&data[DANCE_ANGLES], angles, sizeof(angles));

    c = {};
    std::vector<size_t> fails;
    c.put({ 0x53 });                                                        // push rbx
    c.put({ 0x48, 0x83, 0xEC, 0x20 });                                      // sub rsp, 0x20
    EmitAlive(c, client + offsets::entityList, this->prop, this->prop_handle, fails);
    c.put({ 0x48, 0xB8 }); c.put64(this->prop);                             // mov rax, dance model
    c.put({ 0x48, 0x8B, 0x98 }); c.put32(static_cast<int32_t>(offsets::pawn::m_pGameSceneNode)); // mov rbx, [rax + scene node]
    c.put({ 0x48, 0x85, 0xDB });                                            // test rbx, rbx
    fails.push_back(c.jump({ 0x0F, 0x84 }));                                // je fail
    c.put({ 0x49, 0xBA }); c.put64(this->prop);                             // mov r10, dance model
    c.put({ 0x4C, 0x39, 0x93 }); c.put32(static_cast<int32_t>(offsets::econ::m_pOwner)); // cmp [rbx + owner], r10
    fails.push_back(c.jump({ 0x0F, 0x85 }));                                // jne fail
    c.put({ 0x48, 0x89, 0xD9 });                                            // mov rcx, rbx
    c.put({ 0x48, 0xBA }); c.put64(this->job + DANCE_POSITION);             // mov rdx, position
    c.put({ 0x48, 0xB8 }); c.put64(client + em::fnSetLocalOrigin);          // mov rax, SetLocalOrigin
    c.put({ 0xFF, 0xD0 });                                                  // call rax
    c.put({ 0x48, 0x89, 0xD9 });                                            // mov rcx, rbx
    c.put({ 0x48, 0xBA }); c.put64(this->job + DANCE_ANGLES);               // mov rdx, angles
    c.put({ 0x48, 0xB8 }); c.put64(client + em::fnSetLocalAngles);          // mov rax, SetLocalAngles
    c.put({ 0xFF, 0xD0 });                                                  // call rax
    c.put({ 0x48, 0xB8 }); c.put64(this->job + DANCE_MOVED);                // mov rax, moved
    c.put({ 0xC7, 0x00 }); c.put32(1);                                      // mov dword ptr [rax], 1
    for (auto j : fails)
        c.land(j);
    c.put({ 0x48, 0x83, 0xC4, 0x20 });                                      // add rsp, 0x20
    c.put({ 0x5B });                                                        // pop rbx
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    // Left under the floor it is not seen, & neither is our hidden player
    auto job = this->job;
    if (!CallJob(c.bytes, data) || p->read<uint32_t>(job + DANCE_MOVED) != 1) {
        LOGF(WARNING, "Emotes: the dance could not be moved onto our spot");
        EndDance();
        return false;
    }
    this->anchor = origin;

    auto now = std::chrono::steady_clock::now();
    this->phase = DancePhase::Dancing;
    this->started = now;
    this->length = dance.length > 0.f ? dance.length : FLT_MAX;
    this->intro = dance.after != nullptr;
    this->next_alive = now + ALIVE_CHECK;

    // Its song, played by the game
    this->song = cfg::view::emote_music ? FindSong(GetPack(), dance.music) : std::filesystem::path();
    if (!this->song.empty()) {
        Sounds::PlayMusic(this->song, cfg::view::emote_volume);
        this->next_music = dance.loop_every > 0.f
            ? now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(dance.loop_every))
            : std::chrono::steady_clock::time_point::max();
    }

    LOGF(INFO, "Dance: {} (the copy {}, {})", dance.name, this->clone ? "follows" : "did not come",
        !this->song.empty() ? "with its music" : "no music");
    return true;
}

void Emotes::StepDance() {
    namespace em = offsets::emotes;
    auto now = std::chrono::steady_clock::now();

    if (this->phase == DancePhase::Hiding) {
        if (MaterialChams::CanHide()) {
            if (!SpawnDance())
                End("the dance did not spawn");
        }
        else if (now - this->started > HIDE_TIMEOUT) {
            LOGF(WARNING, "Emotes: our player could not be hidden in time, no dance");
            End("not hidden");
        }
        return;
    }
    if (this->phase != DancePhase::Dancing || this->dance < 0)
        return;

    // Removed by the game (a clean up of the map): ours is over too
    if (now >= this->next_alive) {
        this->next_alive = now + ALIVE_CHECK;
        if (!DanceAlive(this->prop, this->prop_handle)) {
            LOGF(WARNING, "Emotes: the dance was removed by the game");
            ForgetDance();
            End("removed by the game");
            return;
        }

        // The copy is all that shows of us: gone, off the dance or no longer drawn, our player is shown again
        if (this->clone) {
            auto p = Engine::GetProcess();
            auto node = p->read<uintptr_t>(this->clone + offsets::pawn::m_pGameSceneNode);
            int objects = SceneObjects(this->clone);
            if (objects > 0)
                this->clone_drawn = true;

            const char* lost = !DanceAlive(this->clone, this->clone_handle) ? "the copy of our agent was removed"
                : !node || p->read<uint32_t>(node + offsets::econ::m_hierarchyAttachName) != BONEMERGE ? "the copy of our agent left the dance"
                : this->clone_drawn && objects == 0 ? "the copy of our agent is no longer drawn"
                : nullptr;
            if (lost) {
                LOGF(WARNING, "Emotes: {}, the dance ends", lost);
                End(lost);
                return;
            }
        }

#ifdef _DEBUG
        // Once, in the check after the copy should be drawn: where each is & what of it the drawing leaves out
        auto since = now - this->started;
        if (since > COPY_SETTLE && since <= COPY_SETTLE + ALIVE_CHECK) {
            auto p = Engine::GetProcess();
            auto origin_of = [&](uintptr_t entity) {
                auto node = entity ? p->read<uintptr_t>(entity + offsets::pawn::m_pGameSceneNode) : 0;
                auto at = node ? p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) : Vec3_t{};
                return std::format("{:.0f} {:.0f} {:.0f}", at.x, at.y, at.z);
            };
            LOGF(INFO, "Dance check: spot {}, copy 0x{:X} at {} [{}], dance model 0x{:X} at {} [{}], our player [{}]",
                origin_of(this->pawn), this->clone, origin_of(this->clone), MaterialChams::Describe(this->clone),
                this->prop, origin_of(this->prop), MaterialChams::Describe(this->prop), MaterialChams::Describe(this->pawn));
        }
#endif
    }

    const auto& dance = emote_dances::DANCES[this->dance];

    // The first sequence done: the one after it loops
    float since = std::chrono::duration<float>(now - this->started).count();
    if (this->intro && dance.after && since >= (dance.switch_at > 0.f ? dance.switch_at : dance.cycle)) {
        this->intro = false;

        std::vector<uint8_t> data(JOB_SIZE, 0);
        PutText(data, DANCE_TEXT + TEXT_SEQUENCE * TEXT_SIZE, TEXT_SIZE, dance.after);

        auto client = Engine::GetClient().base;
        Code c;
        std::vector<size_t> fails;
        c.put({ 0x48, 0x83, 0xEC, 0x28 });                                  // sub rsp, 0x28
        EmitAlive(c, client + offsets::entityList, this->prop, this->prop_handle, fails);
        c.put({ 0x48, 0xB9 }); c.put64(this->prop);                         // mov rcx, dance model
        c.put({ 0x48, 0xBA }); c.put64(this->job + DANCE_TEXT + TEXT_SEQUENCE * TEXT_SIZE); // mov rdx, sequence
        c.put({ 0x41, 0xB8 }); c.put32(1);                                  // mov r8d, 1 (looping)
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnSetAnimation);        // mov rax, SetAnimation
        c.put({ 0xFF, 0xD0 });                                              // call rax
        for (auto j : fails)
            c.land(j);
        c.put({ 0x48, 0x83, 0xC4, 0x28 });                                  // add rsp, 0x28
        c.put({ 0x31, 0xC0 });                                              // xor eax, eax
        c.put({ 0xC3 });                                                    // ret

        if (!BuildPage() || !CallJob(c.bytes, data))
            LOGF(WARNING, "Emotes: the loop of {} did not start", dance.name);
    }

    // The song again from its start, it is as long as the loop
    if (!this->song.empty() && now >= this->next_music) {
        this->next_music = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(dance.loop_every));
        Sounds::PlayMusic(this->song, cfg::view::emote_volume);
    }
}

bool Emotes::RemoveProps(std::initializer_list<std::pair<uintptr_t, uint32_t>> props) {
    namespace em = offsets::emotes;
    if (!BuildPage())
        return false;

    // UTIL_Remove of each that is still ours
    auto client = Engine::GetClient().base;
    Code c;
    c.put({ 0x48, 0x83, 0xEC, 0x28 });                                      // sub rsp, 0x28
    for (auto [entity, handle] : props) {
        if (!entity || !handle)
            continue;
        std::vector<size_t> fails;
        EmitAlive(c, client + offsets::entityList, entity, handle, fails);
        c.put({ 0x48, 0xB9 }); c.put64(entity);                             // mov rcx, entity
        c.put({ 0x48, 0xB8 }); c.put64(client + em::fnRemoveEntity);        // mov rax, UTIL_Remove
        c.put({ 0xFF, 0xD0 });                                              // call rax
        for (auto j : fails)
            c.land(j);
    }
    c.put({ 0x48, 0x83, 0xC4, 0x28 });                                      // add rsp, 0x28
    c.put({ 0x31, 0xC0 });                                                  // xor eax, eax
    c.put({ 0xC3 });                                                        // ret

    return CallJob(c.bytes, std::vector<uint8_t>(JOB_SIZE, 0));
}

void Emotes::EndDance() {
    // The copy & the dance model first: our player shows again once they are gone, not next to them
    if ((this->prop || this->clone) && !RemoveProps({ { this->clone, this->clone_handle }, { this->prop, this->prop_handle } }))
        LOGF(WARNING, "Emotes: the dance could not be removed, it goes with the map");

    ForgetDance();
}

void Emotes::ForgetDance() {
    if (this->phase == DancePhase::None && !this->prop && !this->clone)
        return;

    MaterialChams::SetDancer(0, 0);
    MaterialChams::Refresh();
    if (!this->song.empty())
        Sounds::StopMusic();

    this->song.clear();
    this->intro = false;
    this->prop = this->clone = 0;
    this->clone_drawn = false;
    this->prop_handle = this->clone_handle = 0;
    this->phase = DancePhase::None;
    this->dance = -1;
}

void Emotes::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;

    std::lock_guard lock(i.update_mutex);
    auto p = Engine::GetProcess();
    if (!p)
        return;

    // The dance removed while the game still runs our calls
    i.End("shutdown");
    if (i.page)
        p->write<uint32_t>(i.page + DATA_ACTIVE, 0);
    Freecam::SetEmoteCamera(false);
    i.playing = -1;
    i.can_play = false;
    i.can_dance = false;

    // The game code as it was. The page stays, a pose may still be inside the stub
    if (i.installed) {
        auto start = Engine::GetClient().base + offsets::emotes::fnApplyPose;
        if (p->patch_code(start, ORIGINAL_START, sizeof(ORIGINAL_START)))
            i.installed = false;
        else
            LOGF(WARNING, "Emotes: failed to put back the start of ApplyAnimGraphPose ({})", GetLastError());
    }

    // The manifests let go on the main thread once no pose samples their clips anymore, the game drops them then
    if (i.service && GameThread::IsAvailable() && i.job) {
        std::this_thread::sleep_for(STUB_SETTLE);

        for (auto* manifest : { &i.manifest, &i.dance_manifest }) {
            if (!*manifest || !i.job)
                continue;

            Code c;
            c.put({ 0x48, 0x83, 0xEC, 0x28 });                              // sub rsp, 0x28
            c.put({ 0x48, 0xB9 }); c.put64(i.service);                      // mov rcx, service
            c.put({ 0x48, 0xBA }); c.put64(*manifest);                      // mov rdx, manifest
            c.put({ 0x48, 0xB8 }); c.put64(Engine::GetEngine().base + offsets::emotes::fnDestroyManifest); // mov rax, DestroyResourceManifest
            c.put({ 0xFF, 0xD0 });                                          // call rax
            c.put({ 0x48, 0x83, 0xC4, 0x28 });                              // add rsp, 0x28
            c.put({ 0x31, 0xC0 });                                          // xor eax, eax
            c.put({ 0xC3 });                                                // ret

            if (i.CallJob(c.bytes, std::vector<uint8_t>(c.size(), 0)))
                *manifest = 0;
        }
    }
}
