#include "Freecam.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/GameThread.hpp"
#include "core/engine/world/MapCollision.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"
#include "core/features/MaterialChams.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {
    // Page in the game: our data, the stub, then the vtable copy with the RTTI pointer before it
    constexpr uintptr_t PAGE_MAGIC = 0x00;      // uint64
    constexpr uintptr_t PAGE_VTABLE = 0x08;     // uint64, the real vtable, for a later run when this one did not put it back
    constexpr uintptr_t PAGE_ORIGINAL = 0x10;   // uint64, the real OverrideView
    constexpr uintptr_t DATA_MODE = 0x18;       // uint32, Freecam::Mode
    constexpr uintptr_t DATA_ORIGIN = 0x20;     // Vector: free cam where the camera is, following added to their eyes
    constexpr uintptr_t DATA_FOLLOW_ANGLES = 0x30; // QAngle, where the one followed looks, smoothed
    constexpr uintptr_t DATA_TARGET = 0x40;     // uint64, the pawn followed
    constexpr uintptr_t DATA_FOV = 0x48;        // float, field of view of our camera, 0 leaves the one of the game: it is the
                                                // one of the player watched, zoomed in while they scope
    constexpr uintptr_t DATA_VIEW = 0x50;       // QAngle, where we look, saved by the stub before the game turns it for third person
    // Dead, written into our spectator services by the stub each frame, right before the spectator camera reads them
    constexpr uintptr_t DATA_OBSERVER_PAWN = 0x70;      // uint64, our pawn, 0 for nothing. The stub asks the game for it
    constexpr uintptr_t DATA_OBSERVER_TARGET = 0x78;    // uint32, handle, 0 leaves it
    constexpr uintptr_t DATA_OBSERVER_MODE = 0x7C;      // uint8
    constexpr uintptr_t PAGE_LOCAL_PAWN = 0x88;         // uint64, GetLocalPawn of the game
    constexpr uintptr_t DATA_POSITION_ONLY = 0x80;      // uint32, the spectator camera of the game with our position
    constexpr uintptr_t PAGE_OBSERVER_VIEW = 0x90;      // uint64, the spectator camera of the game, its call goes to STUB_OBSERVER
    // Watching alive in first person: their field of view from CalcView too (zoomed while they scope), only a sane one
    constexpr uintptr_t DATA_CALC_FOV_ON = 0x98;        // uint32
    constexpr uintptr_t CONST_MIN_FOV = 0x9C;           // float
    constexpr uintptr_t CONST_MAX_FOV = 0xA0;           // float
    // Watching alive the game turns our view angles along with theirs (we are a spectator to it), sent to the server:
    // ours kept, written back by STUB_HOLD each frame after the camera of the game
    constexpr uintptr_t DATA_HOLD_ANGLES = 0xA4;        // QAngle
    constexpr uintptr_t DATA_HOLD_ON = 0xBC;            // uint32
    constexpr uintptr_t PAGE_VIEW_ANGLES = 0xF8;        // uint64, QAngle* (input, slot) of the game, 0 when not found
    constexpr uintptr_t DATA_FREE_ANGLES = 0xB0;        // QAngle, where the free cam looks, moved by the mouse we keep from the game
    constexpr uintptr_t DATA_INPUT = 0x60;      // uint64, QAngle* the free cam looks along: our own (DATA_FREE_ANGLES), turned by
                                                // the mouse we keep from the game. 0 for where we look
    // View punch: alive in first person, the kick the camera shows (where it looks minus where we aim) times our scale
    constexpr uintptr_t DATA_PUNCH_SCALE = 0xC0;        // float, 0 none of it, 1 all of it
    constexpr uintptr_t DATA_PUNCH_ON = 0xC4;           // uint32, 0 leaves the camera alone
    constexpr uintptr_t PAGE_INPUT = 0xC8;              // uint64, where the game keeps its CSGOInput pointer
    constexpr uintptr_t DATA_PUNCH_SEEN = 0xD0;         // QAngle, the kick of the last frame, for the log
    constexpr uintptr_t CONST_180 = 0xDC;               // floats for the stub
    constexpr uintptr_t CONST_MINUS_180 = 0xE0;
    constexpr uintptr_t CONST_360 = 0xE4;
    constexpr uintptr_t CONST_MAX_PUNCH = 0xE8;
    // Following in first person: the camera of the game for that player (CalcView of their camera services)
    constexpr uintptr_t DATA_CALC_ON = 0xEC;            // uint32, 0 our own position & smoothed angles
    constexpr uintptr_t PAGE_CALC_VIEW = 0xF0;          // uint64, CalcView, 0 when not found
    constexpr uintptr_t STUB_CALC = 0x1800;             // Code calling it
    constexpr uintptr_t DATA_CALC = 0x1F00;             // What it gives: Vector origin, QAngle angles (+0x10), float fov (+0x1C)
    constexpr uintptr_t DATA_CALC_ORIGIN = DATA_CALC;
    constexpr uintptr_t DATA_CALC_ANGLES = DATA_CALC + 0x10;
    constexpr uintptr_t DATA_CALC_FOV = DATA_CALC + 0x1C;
    constexpr float MAX_PUNCH = 20.f;                   // Degrees, more is no kick: dead, teleported, another camera
    constexpr uintptr_t STUB = 0x100;
    constexpr uintptr_t STUB_OBSERVER = 0x340;
    constexpr uintptr_t STUB_HOLD = 0x440;
    constexpr uintptr_t TABLE = 0x500;
    constexpr uintptr_t OLD_TABLE = 0x400;              // Of a page from an earlier version
    constexpr uintptr_t STUB_PUNCH = 0x1000;
    constexpr size_t PAGE_SIZE = 0x2000;
    constexpr size_t MAX_ENTRIES = (STUB_PUNCH - TABLE) / sizeof(uintptr_t) - 1;
    constexpr uint64_t MAGIC = 0x4D41434545524643; // "CFREECAM"

    constexpr float FAST = 3.f;                 // Shift
    constexpr float DISTANCE_SPEED = 150.f;     // W & S following, units per second
    constexpr float MIN_DISTANCE = 40.f, MAX_DISTANCE = 300.f;

    // The emote camera: around the chest (that far under the eyes), from close behind out to the distance of the
    // third person of the game in a moment, kept that far off walls
    constexpr float EMOTE_PIVOT_DROP = 18.f;
    constexpr float EMOTE_START_DISTANCE = 30.f;
    constexpr float EMOTE_PULL_OUT = 0.35f;     // Seconds
    constexpr float EMOTE_MIN_DISTANCE = 60.f, EMOTE_MAX_DISTANCE = 250.f, EMOTE_DEFAULT_DISTANCE = 120.f;
    constexpr float EMOTE_WALL_GAP = 8.f;

    // How long the angles of a tick take to reach, measured between the ones we get: a tick of 64 at the least,
    // two at most (unchanged angles are not sent, the next ones come after a pause)
    constexpr float MIN_TICK = 1.f / 128.f, MAX_TICK = 2.f / 64.f;

    // After switching to another player in first person, before the spectator camera of the game takes over
    constexpr auto SWITCH_SETTLE = std::chrono::milliseconds(600);

    // mov eax, 1; ret over the start of the spectator keys while ours are on: the game sends no spec_next & co, the
    // server would switch to a teammate of its choice
    constexpr uint8_t BINDS_ORIGINAL[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55 };
    constexpr uint8_t BINDS_PATCHED[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };

    // Or, the page in reach, jmp rel32 + nop to a filter taking only these: the player or the mode they switch.
    // The others, the loadout on F (+lookatweapon) & co, go on to the game
    constexpr uintptr_t STUB_BINDS = 0xC00;
    constexpr uintptr_t BIND_NAMES = 0xD00;
    constexpr const char* TAKEN_BINDS[] = {
        "+attack", "+attack2", "invnext", "invprev", "+jump", "lastinv",
        "slot1", "slot2", "slot3", "slot4", "slot5", "slot6", "slot7", "slot8", "slot9", "slot10", "slot11", "slot12",
    };

    // SDL3 events the game takes as moves while the free cam is on: the mouse, its buttons & wheel, keys going down.
    // Their releases still go through, nothing stays held
    constexpr uint32_t SDL_EVENT_KEY_DOWN = 0x300;
    constexpr uint32_t SDL_EVENT_MOUSE_MOTION = 0x400;
    constexpr uint32_t SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401;
    constexpr uint32_t SDL_EVENT_MOUSE_WHEEL = 0x403;
    constexpr uint32_t BLOCKED_EVENTS[] = { SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_WHEEL, SDL_EVENT_KEY_DOWN };
    constexpr size_t MOUSE_EVENTS = 3;          // The first ones

    // rcx = { SDL_SetEventEnabled, bool enabled }, calls it for each blocked event, or the mouse ones only
    constexpr uintptr_t INPUT_CODE_ALL = 0x00;
    constexpr uintptr_t INPUT_CODE_MOUSE = 0x40;
    constexpr uintptr_t INPUT_ENABLE = 0x80;    // Data enabling them
    constexpr uintptr_t INPUT_DISABLE = 0x90;   // & disabling
    constexpr size_t INPUT_SIZE = 0x100;

    // Degrees per count of the mouse, times the sensitivity of the game, like m_yaw & m_pitch
    constexpr float MOUSE_DEGREES = 0.022f;
    constexpr float MAX_PITCH = 89.f;
    constexpr float DEFAULT_FOV = 90.f;

    constexpr int32_t GAMEPHASE_MATCH_ENDED = 5;
    constexpr uint8_t TEAM_T = 2, TEAM_CT = 3;

    constexpr uint8_t JE_SHORT = 0x74;
    constexpr uint8_t JMP_SHORT = 0xEB;
    constexpr uint8_t OBSERVER_FIRST = 2;       // ObserverMode::First, through their eyes with the weapon in their hands
    constexpr uint8_t OBSERVER_THIRD = 3;       // ObserverMode::Third, no weapon in the hands drawn in front of our camera

    // Keys of Freecam::Key, each with the other keys giving the same
    constexpr DWORD KEYS[][3] = {
        { 'W' }, { 'S' }, { 'A' }, { 'D' }, { VK_SPACE },
        // The left ones only: right shift opens the menu (read with GetAsyncKeyState, which a held back key never reaches)
        { VK_LCONTROL }, { VK_LSHIFT },
    };

    // How far the cursor moves per count of the mouse, by the pointer speed of Windows (1 - 20, 10 the default):
    // the table of Windows, without "enhance pointer precision". Read now & then, it can be changed any time
    float PointerSpeed() {
        static constexpr float FACTORS[] = { 0.03125f, 0.0625f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.f,
            1.25f, 1.5f, 1.75f, 2.f, 2.25f, 2.5f, 2.75f, 3.f, 3.25f, 3.5f };
        static float factor = 1.f;
        static auto next = std::chrono::steady_clock::time_point{};

        auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            int speed = 10;
            if (SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &speed, 0) && speed >= 1 && speed <= 20)
                factor = FACTORS[speed - 1];
            next = now + std::chrono::seconds(1);
        }
        return factor;
    }

    Vec3_t Forward(const Vec3_t& angles) {
        float pitch = angles.x * std::numbers::pi_v<float> / 180.f, yaw = angles.y * std::numbers::pi_v<float> / 180.f;
        return Vec3_t(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch));
    }

    // To the right of where we look, flat
    Vec3_t Right(const Vec3_t& angles) {
        float yaw = angles.y * std::numbers::pi_v<float> / 180.f;
        return Vec3_t(std::sin(yaw), -std::cos(yaw), 0.f);
    }

    // Lets go of a key for the game, it did see it go down
    void ReleaseKey(DWORD vk) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    }

    // Small x64 assembler of what the stub needs
    struct Code {
        std::vector<uint8_t> bytes;

        void put(std::initializer_list<uint8_t> list) { bytes.insert(bytes.end(), list); }
        void put32(int32_t value) { for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); }
        size_t size() const { return bytes.size(); }

        // jcc / jmp rel32 to a label set later
        size_t jump(std::initializer_list<uint8_t> opcode) { put(opcode); put32(0); return size(); }
        void land(size_t jump_end) {
            int32_t rel = static_cast<int32_t>(size() - jump_end);
            std::memcpy(&bytes[jump_end - 4], &rel, 4);
        }
    };
}

bool Freecam::Init() {
    return GetInstance().InitImpl();
}

bool Freecam::IsAvailable() {
    return Engine::IsInsecure() && GetInstance().installed;
}

Freecam::Mode Freecam::GetMode() {
    return GetInstance().mode;
}

uintptr_t Freecam::GetTarget() {
    auto& i = GetInstance();
    return i.mode == Mode::Follow ? i.target.load() : 0;
}

bool Freecam::IsFirstPerson() {
    return GetInstance().first_person;
}

bool Freecam::IsDeadAvailable() {
    return IsAvailable() && offsets::camera::observerViewJump != 0;
}

bool Freecam::IsDeadCamera() {
    return GetInstance().dead_camera;
}

bool Freecam::Spectate(uintptr_t pawn) {
    auto& i = GetInstance();
    if (!IsAvailable() || !IsLiving(pawn))
        return false;

    i.spectate_stop = false;
    i.spectate_request = pawn;
    return true;
}

void Freecam::StopSpectate() {
    GetInstance().spectate_stop = true;
}

bool Freecam::IsSpectating() {
    return GetInstance().alive_spectate;
}

void Freecam::LendMouse(bool lend) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    GetInstance().lent_until = lend ? until.time_since_epoch().count() : 0;
}

void Freecam::SetEmoteCamera(bool on) {
    GetInstance().emote_camera = on;
}

Freecam::LentMouse Freecam::TakeLentMouse() {
    auto& i = GetInstance();
    LentMouse taken;
    taken.dx = i.lent_dx.exchange(0);
    taken.dy = i.lent_dy.exchange(0);
    taken.wheel = i.lent_wheel.exchange(0);
    taken.left = i.lent_left.exchange(0);
    taken.right = i.lent_right.exchange(0);
    return taken;
}

bool Freecam::IsLiving(uintptr_t pawn) {
    if (!pawn)
        return false;

    auto snapshot = Cache::Current();
    for (const auto& player : snapshot->players)
        if (player.GetPawnAddress() == pawn)
            return player.alive && !player.localplayer;
    return false;
}

bool Freecam::InitImpl() {
    if (!Engine::IsInsecure() || !offsets::camera::dwClientMode || !offsets::camera::overrideView)
        return false;

    if (!Install())
        return false;

    // Also gives the game its input back after a run closed while the free cam was on
    if (SetupGameInput())
        BlockGameInput(Input::None, true);

    std::thread(&Freecam::Thread, this).detach();
    std::thread(&Freecam::MouseThread, this).detach();

    LOGF(INFO, "Successfully initialized free cam...");
    return true;
}

bool Freecam::Install() {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    auto pe_header = client.base + p->read<int32_t>(client.base + 0x3C);
    auto image_size = p->read<uint32_t>(pe_header + 0x50);
    auto in_client = [&](uintptr_t address) { return address >= client.base && address < client.base + image_size; };

    this->object = client.base + offsets::camera::dwClientMode;
    this->vtable = p->read<uintptr_t>(this->object);

    // Still our copy from an earlier run that was closed without putting the real one back
    if (this->vtable && !in_client(this->vtable)) {
        for (auto table : { TABLE, OLD_TABLE }) {
            auto old = this->vtable - table - sizeof(uintptr_t);
            if (p->read<uint64_t>(old + PAGE_MAGIC) == MAGIC) {
                this->vtable = p->read<uintptr_t>(old + PAGE_VTABLE);
                LOGF(INFO, "The client mode vtable was still ours from an earlier run, real one at 0x{:X}", this->vtable);
                break;
            }
        }
    }

    if (!this->vtable || !in_client(this->vtable)) {
        LOGF(WARNING, "Could not read the client mode vtable, free cam is disabled");
        return false;
    }

    // The entry of OverrideView
    auto override_view = client.base + offsets::camera::overrideView;
    std::vector<uintptr_t> entries;
    size_t index = SIZE_MAX;

    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        auto entry = p->read<uintptr_t>(this->vtable + i * sizeof(uintptr_t));
        if (!in_client(entry))
            break;

        if (entry == override_view)
            index = i;
        entries.push_back(entry);
    }

    if (index == SIZE_MAX) {
        LOGF(WARNING, "OverrideView is not in the client mode vtable ({} entries), free cam is disabled", entries.size());
        return false;
    }

    // Below client.dll, close enough for a call rel32 from it to reach (the spectator camera call)
    constexpr uintptr_t GRANULARITY = 0x10000;
    for (uintptr_t below = GRANULARITY; below < 0x70000000 && !this->page; below += GRANULARITY) {
        auto at = (client.base & ~(GRANULARITY - 1)) - below;
        this->page = reinterpret_cast<uintptr_t>(VirtualAllocEx(p->handle_, reinterpret_cast<void*>(at), PAGE_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    if (!this->page)
        this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return false;

    const int32_t ORIGIN = static_cast<int32_t>(offsets::camera::m_vecOrigin);
    const int32_t ANGLES = static_cast<int32_t>(offsets::camera::m_angView);
    const int32_t FOV = static_cast<int32_t>(offsets::camera::m_flFov);
    const int32_t SCENE = static_cast<int32_t>(offsets::pawn::m_pGameSceneNode);
    const int32_t ABS = static_cast<int32_t>(offsets::bomb::m_vecAbsOrigin);
    const int32_t VIEW_OFFSET = static_cast<int32_t>(offsets::pawn::m_vecViewOffset);
    const int32_t EYE = static_cast<int32_t>(offsets::grenade::m_angEyeAngles);
    const int32_t OBSERVER = static_cast<int32_t>(offsets::pawn::m_pObserverServices);
    const int32_t OBSERVER_MODE = static_cast<int32_t>(offsets::observerServices::m_iObserverMode);
    const int32_t OBSERVER_TARGET = static_cast<int32_t>(offsets::observerServices::m_hObserverTarget);
    const int32_t VIEW_ANGLES = static_cast<int32_t>(offsets::input::m_angViewAngles);
    const int32_t THIRD_PERSON = static_cast<int32_t>(offsets::input::m_bInThirdPerson);

    // rcx the client mode, rdx the view
    Code c;
    auto page_rel = [&]() { c.put32(-static_cast<int32_t>(STUB + c.size() + 4)); };   // rip relative to the page

    c.put({ 0x53 });                                    // push rbx
    c.put({ 0x48, 0x83, 0xEC, 0x20 });                  // sub rsp, 0x20
    c.put({ 0x48, 0x8B, 0xDA });                        // mov rbx, rdx
    c.put({ 0x4C, 0x8D, 0x15 }); page_rel();            // lea r10, [page]

    // Dead: who we spectate, into our spectator services. Only when the game says our pawn is still that one
    c.put({ 0x49, 0x8B, 0x42, DATA_OBSERVER_PAWN });    // mov rax, [r10 + observer pawn]
    c.put({ 0x48, 0x85, 0xC0 });                        // test rax, rax
    auto no_observer = c.jump({ 0x0F, 0x84 });          // jz
    c.put({ 0x51, 0x52 });                              // push rcx, push rdx
    c.put({ 0x48, 0x83, 0xEC, 0x20 });                  // sub rsp, 0x20
    c.put({ 0x33, 0xC9 });                              // xor ecx, ecx (slot 0)
    c.put({ 0x49, 0x8B, 0x82 }); c.put32(PAGE_LOCAL_PAWN); // mov rax, [r10 + GetLocalPawn]
    c.put({ 0xFF, 0xD0 });                              // call rax
    c.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
    c.put({ 0x5A, 0x59 });                              // pop rdx, pop rcx
    c.put({ 0x4C, 0x8D, 0x15 }); page_rel();            // lea r10, [page]
    c.put({ 0x49, 0x3B, 0x42, DATA_OBSERVER_PAWN });    // cmp rax, [r10 + observer pawn]
    auto other_pawn = c.jump({ 0x0F, 0x85 });           // jne
    c.put({ 0x4C, 0x8B, 0x98 }); c.put32(OBSERVER);     // mov r11, [rax + observer services]
    c.put({ 0x4D, 0x85, 0xDB });                        // test r11, r11
    auto no_services = c.jump({ 0x0F, 0x84 });          // jz
    c.put({ 0x41, 0x8A, 0x42, DATA_OBSERVER_MODE });    // mov al, [r10 + mode]
    c.put({ 0x41, 0x88, 0x83 }); c.put32(OBSERVER_MODE);    // mov [r11 + observer mode], al
    c.put({ 0x41, 0x8B, 0x42, DATA_OBSERVER_TARGET });  // mov eax, [r10 + target]
    c.put({ 0x85, 0xC0 });                              // test eax, eax
    auto no_handle = c.jump({ 0x0F, 0x84 });            // jz
    c.put({ 0x41, 0x89, 0x83 }); c.put32(OBSERVER_TARGET);  // mov [r11 + observer target], eax
    c.land(no_observer); c.land(other_pawn); c.land(no_services); c.land(no_handle);

    // Where we look, before third person turns it toward the camera
    c.put({ 0xF2, 0x0F, 0x10, 0x83 }); c.put32(ANGLES);             // movsd xmm0, [rbx + angles]
    c.put({ 0xF2, 0x41, 0x0F, 0x11, 0x42, DATA_VIEW });             // movsd [r10 + view], xmm0
    c.put({ 0x8B, 0x83 }); c.put32(ANGLES + 8);                     // mov eax, [rbx + angles + 8]
    c.put({ 0x41, 0x89, 0x42, DATA_VIEW + 8 });                     // mov [r10 + view + 8], eax

    c.put({ 0x41, 0xFF, 0x52, PAGE_ORIGINAL });         // call [r10 + original], rcx & rdx untouched
    c.put({ 0xE8 }); c.put32(static_cast<int32_t>(STUB_HOLD - (STUB + c.size() + 4)));    // call hold

    c.put({ 0x4C, 0x8D, 0x15 }); page_rel();            // lea r10, [page]
    c.put({ 0x41, 0x8B, 0x42, DATA_MODE });             // mov eax, [r10 + mode]
    c.put({ 0x83, 0xF8, 0x01 });                        // cmp eax, free
    auto to_free = c.jump({ 0x0F, 0x84 });              // je free
    c.put({ 0x83, 0xF8, 0x02 });                        // cmp eax, follow
    auto to_follow = c.jump({ 0x0F, 0x84 });            // je follow

    // Off: the camera of the game, its kick scaled (STUB_PUNCH, rbx still the view)
    c.put({ 0xE8 }); c.put32(static_cast<int32_t>(STUB_PUNCH - (STUB + c.size() + 4)));   // call punch
    auto off = c.jump({ 0xE9 });                        // jmp done

    // Our field of view, not the zoom of the one watched before
    auto put_fov = [&]() {
        c.put({ 0x41, 0x8B, 0x42, DATA_FOV });                      // mov eax, [r10 + fov]
        c.put({ 0x85, 0xC0 });                                      // test eax, eax
        c.put({ 0x74, 0x06 });                                      // jz +6
        c.put({ 0x89, 0x83 }); c.put32(FOV);                        // mov [rbx + fov], eax
    };

    // Free: our position, looking where we look
    c.land(to_free);
    put_fov();
    c.put({ 0xF2, 0x41, 0x0F, 0x10, 0x42, DATA_ORIGIN });           // movsd xmm0, [r10 + origin]
    c.put({ 0xF2, 0x0F, 0x11, 0x83 }); c.put32(ORIGIN);             // movsd [rbx + origin], xmm0
    c.put({ 0x41, 0x8B, 0x42, DATA_ORIGIN + 8 });                   // mov eax, [r10 + origin + 8]
    c.put({ 0x89, 0x83 }); c.put32(ORIGIN + 8);                     // mov [rbx + origin + 8], eax
    c.put({ 0x4D, 0x8B, 0x5A, DATA_INPUT });                        // mov r11, [r10 + input angles]
    c.put({ 0x4D, 0x85, 0xDB });                                    // test r11, r11
    auto no_input = c.jump({ 0x0F, 0x84 });                         // jz view
    c.put({ 0xF2, 0x41, 0x0F, 0x10, 0x03 });                        // movsd xmm0, [r11]
    c.put({ 0xF2, 0x0F, 0x11, 0x83 }); c.put32(ANGLES);             // movsd [rbx + angles], xmm0
    c.put({ 0x41, 0x8B, 0x43, 0x08 });                              // mov eax, [r11 + 8]
    c.put({ 0x89, 0x83 }); c.put32(ANGLES + 8);                     // mov [rbx + angles + 8], eax
    auto input_done = c.jump({ 0xE9 });                             // jmp done
    c.land(no_input);
    c.put({ 0xF2, 0x41, 0x0F, 0x10, 0x42, DATA_VIEW });             // movsd xmm0, [r10 + view]
    c.put({ 0xF2, 0x0F, 0x11, 0x83 }); c.put32(ANGLES);             // movsd [rbx + angles], xmm0
    c.put({ 0x41, 0x8B, 0x42, DATA_VIEW + 8 });                     // mov eax, [r10 + view + 8]
    c.put({ 0x89, 0x83 }); c.put32(ANGLES + 8);                     // mov [rbx + angles + 8], eax
    auto free_done = c.jump({ 0xE9 });                              // jmp done

    // Follow: their eyes this frame plus our offset, looking where they look
    c.land(to_follow);
    put_fov();
    c.put({ 0x4D, 0x8B, 0x5A, DATA_TARGET });           // mov r11, [r10 + target]
    c.put({ 0x4D, 0x85, 0xDB });                        // test r11, r11
    auto no_target = c.jump({ 0x0F, 0x84 });            // jz done
    c.put({ 0x4D, 0x8B, 0x8B }); c.put32(SCENE);        // mov r9, [r11 + scene node]
    c.put({ 0x4D, 0x85, 0xC9 });                        // test r9, r9
    auto no_node = c.jump({ 0x0F, 0x84 });              // jz done

    for (int32_t k = 0; k < 3; k++) {
        c.put({ 0xF3, 0x41, 0x0F, 0x10, 0x81 }); c.put32(ABS + k * 4);          // movss xmm0, [r9 + abs origin]
        c.put({ 0xF3, 0x41, 0x0F, 0x58, 0x83 }); c.put32(VIEW_OFFSET + k * 4);  // addss xmm0, [r11 + view offset]
        c.put({ 0xF3, 0x41, 0x0F, 0x58, 0x42, static_cast<uint8_t>(DATA_ORIGIN + k * 4) }); // addss xmm0, [r10 + offset]
        c.put({ 0xF3, 0x0F, 0x11, 0x83 }); c.put32(ORIGIN + k * 4);             // movss [rbx + origin], xmm0
    }
    // Where they look, smoothed between the ticks by our thread (see UpdateFollow)
    c.put({ 0xF2, 0x41, 0x0F, 0x10, 0x42, DATA_FOLLOW_ANGLES });   // movsd xmm0, [r10 + follow angles]
    c.put({ 0xF2, 0x0F, 0x11, 0x83 }); c.put32(ANGLES);             // movsd [rbx + angles], xmm0
    c.put({ 0x41, 0x8B, 0x42, DATA_FOLLOW_ANGLES + 8 });            // mov eax, [r10 + follow angles + 8]
    c.put({ 0x89, 0x83 }); c.put32(ANGLES + 8);                     // mov [rbx + angles + 8], eax

    // First person: over that, their eyes & where they look the way the game shows them (STUB_CALC, r11 the target)
    c.put({ 0xE8 }); c.put32(static_cast<int32_t>(STUB_CALC - (STUB + c.size() + 4)));   // call calc

    c.land(off); c.land(free_done); c.land(input_done); c.land(no_target); c.land(no_node);
    c.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
    c.put({ 0x5B });                                    // pop rbx
    c.put({ 0xC3 });                                    // ret

    // View punch, called with rbx the view: the kick is what the camera turned past where we aim, after the game built
    // the view of this frame, so nothing races with the game. Where we aim plus the kick times our scale
    Code punch;
    auto page32 = [&](std::initializer_list<uint8_t> opcode, uintptr_t at) { punch.put(opcode); punch.put32(static_cast<int32_t>(at)); };
    std::vector<size_t> no_punch;
    punch.put({ 0x4C, 0x8D, 0x15 }); punch.put32(-static_cast<int32_t>(STUB_PUNCH + punch.size() + 4));  // lea r10, [page]
    page32({ 0x41, 0x8B, 0x82 }, DATA_PUNCH_ON);        // mov eax, [r10 + punch on]
    punch.put({ 0x85, 0xC0 });                              // test eax, eax
    no_punch.push_back(punch.jump({ 0x0F, 0x84 }));         // jz done
    page32({ 0x4D, 0x8B, 0x9A }, PAGE_INPUT);           // mov r11, [r10 + input]
    punch.put({ 0x4D, 0x85, 0xDB });                        // test r11, r11
    no_punch.push_back(punch.jump({ 0x0F, 0x84 }));         // jz done
    punch.put({ 0x4D, 0x8B, 0x1B });                        // mov r11, [r11]
    punch.put({ 0x4D, 0x85, 0xDB });                        // test r11, r11
    no_punch.push_back(punch.jump({ 0x0F, 0x84 }));         // jz done
    punch.put({ 0x41, 0x80, 0xBB }); punch.put32(THIRD_PERSON); punch.put({ 0x00 });  // cmp byte [r11 + third person], 0
    no_punch.push_back(punch.jump({ 0x0F, 0x85 }));         // jne done: the third person camera looks at us, not along

    // Each angle: the kick, -180 - 180, none when too large
    for (int32_t a = 0; a < 3; a++) {
        punch.put({ 0xF3, 0x0F, 0x10, 0x83 }); punch.put32(ANGLES + a * 4);             // movss xmm0, [rbx + angles]
        punch.put({ 0xF3, 0x41, 0x0F, 0x10, 0x8B }); punch.put32(VIEW_ANGLES + a * 4);  // movss xmm1, [r11 + view angles]
        punch.put({ 0xF3, 0x0F, 0x5C, 0xC1 });                                      // subss xmm0, xmm1
        page32({ 0x41, 0x0F, 0x2F, 0x82 }, CONST_180);                          // comiss xmm0, [180]
        auto not_above = punch.jump({ 0x0F, 0x86 });                                // jbe
        page32({ 0xF3, 0x41, 0x0F, 0x5C, 0x82 }, CONST_360);                    // subss xmm0, [360]
        punch.land(not_above);
        page32({ 0x41, 0x0F, 0x2F, 0x82 }, CONST_MINUS_180);                    // comiss xmm0, [-180]
        auto not_below = punch.jump({ 0x0F, 0x83 });                                // jae
        page32({ 0xF3, 0x41, 0x0F, 0x58, 0x82 }, CONST_360);                    // addss xmm0, [360]
        punch.land(not_below);
        page32({ 0xF3, 0x41, 0x0F, 0x11, 0x82 }, DATA_PUNCH_SEEN + a * 4);      // movss [r10 + seen], xmm0
        punch.put({ 0x66, 0x0F, 0x7E, 0xC0 });                                      // movd eax, xmm0
        punch.put({ 0x25, 0xFF, 0xFF, 0xFF, 0x7F });                                // and eax, 0x7FFFFFFF
        punch.put({ 0x66, 0x0F, 0x6E, 0xC0 });                                      // movd xmm0, eax
        page32({ 0x41, 0x0F, 0x2F, 0x82 }, CONST_MAX_PUNCH);                    // comiss xmm0, [max]
        no_punch.push_back(punch.jump({ 0x0F, 0x87 }));                             // ja done
        no_punch.push_back(punch.jump({ 0x0F, 0x8A }));                             // jp done (NaN)
    }

    for (int32_t a = 0; a < 3; a++) {
        page32({ 0xF3, 0x41, 0x0F, 0x10, 0x82 }, DATA_PUNCH_SEEN + a * 4);      // movss xmm0, [r10 + seen]
        page32({ 0xF3, 0x41, 0x0F, 0x59, 0x82 }, DATA_PUNCH_SCALE);             // mulss xmm0, [r10 + scale]
        punch.put({ 0xF3, 0x41, 0x0F, 0x58, 0x83 }); punch.put32(VIEW_ANGLES + a * 4);  // addss xmm0, [r11 + view angles]
        punch.put({ 0xF3, 0x0F, 0x11, 0x83 }); punch.put32(ANGLES + a * 4);             // movss [rbx + angles], xmm0
    }
    for (auto jump : no_punch)
        punch.land(jump);
    punch.put({ 0xC3 });                                    // ret

    // The spectator camera of the game, then our position over its own: rcx its services, rdx the origin, r8 the angles
    Code o;
    auto page_rel_o = [&]() { o.put32(-static_cast<int32_t>(STUB_OBSERVER + o.size() + 4)); };
    o.put({ 0x53 });                                    // push rbx
    o.put({ 0x48, 0x83, 0xEC, 0x20 });                  // sub rsp, 0x20
    o.put({ 0x48, 0x8B, 0xDA });                        // mov rbx, rdx
    o.put({ 0x4C, 0x8D, 0x15 }); page_rel_o();          // lea r10, [page]
    o.put({ 0x41, 0xFF, 0x92 }); o.put32(PAGE_OBSERVER_VIEW);   // call [r10 + spectator camera]
    o.put({ 0x4C, 0x8D, 0x15 }); page_rel_o();          // lea r10, [page]
    o.put({ 0x41, 0x8B, 0x82 }); o.put32(DATA_POSITION_ONLY);   // mov eax, [r10 + position only]
    o.put({ 0x85, 0xC0 });                              // test eax, eax
    auto o_off = o.jump({ 0x0F, 0x84 });                // jz done
    o.put({ 0x4D, 0x8B, 0x5A, DATA_TARGET });           // mov r11, [r10 + target]
    o.put({ 0x4D, 0x85, 0xDB });                        // test r11, r11
    auto o_no_target = o.jump({ 0x0F, 0x84 });          // jz done
    o.put({ 0x4D, 0x8B, 0x8B }); o.put32(SCENE);        // mov r9, [r11 + scene node]
    o.put({ 0x4D, 0x85, 0xC9 });                        // test r9, r9
    auto o_no_node = o.jump({ 0x0F, 0x84 });            // jz done
    for (int32_t k = 0; k < 3; k++) {
        o.put({ 0xF3, 0x41, 0x0F, 0x10, 0x81 }); o.put32(ABS + k * 4);          // movss xmm0, [r9 + abs origin]
        o.put({ 0xF3, 0x41, 0x0F, 0x58, 0x83 }); o.put32(VIEW_OFFSET + k * 4);  // addss xmm0, [r11 + view offset]
        o.put({ 0xF3, 0x0F, 0x11, 0x43, static_cast<uint8_t>(k * 4) });       // movss [rbx + k], xmm0
    }
    o.land(o_off); o.land(o_no_target); o.land(o_no_node);
    o.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
    o.put({ 0x5B });                                    // pop rbx
    o.put({ 0xC3 });                                    // ret

    // Watching alive: our view angles back in the input, the camera of the game has turned them along with theirs.
    // Through the function of the game when found (the last of its list), else its own field
    Code h;
    {
        auto page_rel_h = [&]() { h.put32(-static_cast<int32_t>(STUB_HOLD + h.size() + 4)); };
        std::vector<size_t> hold_off;
        h.put({ 0x4C, 0x8D, 0x15 }); page_rel_h();                  // lea r10, [page]
        h.put({ 0x41, 0x8B, 0x82 }); h.put32(DATA_HOLD_ON);         // mov eax, [r10 + hold on]
        h.put({ 0x85, 0xC0 });                                      // test eax, eax
        hold_off.push_back(h.jump({ 0x0F, 0x84 }));                 // jz done
        h.put({ 0x49, 0x8B, 0x8A }); h.put32(PAGE_INPUT);           // mov rcx, [r10 + input]
        h.put({ 0x48, 0x85, 0xC9 });                                // test rcx, rcx
        hold_off.push_back(h.jump({ 0x0F, 0x84 }));                 // jz done
        h.put({ 0x48, 0x8B, 0x09 });                                // mov rcx, [rcx]
        h.put({ 0x48, 0x85, 0xC9 });                                // test rcx, rcx
        hold_off.push_back(h.jump({ 0x0F, 0x84 }));                 // jz done
        h.put({ 0x48, 0x8D, 0x81 }); h.put32(VIEW_ANGLES);          // lea rax, [rcx + view angles]
        h.put({ 0x49, 0x8B, 0x92 }); h.put32(PAGE_VIEW_ANGLES);     // mov rdx, [r10 + its function]
        h.put({ 0x48, 0x85, 0xD2 });                                // test rdx, rdx
        auto to_write = h.jump({ 0x0F, 0x84 });                     // jz write
        h.put({ 0x49, 0x89, 0xD3 });                                // mov r11, rdx
        h.put({ 0x31, 0xD2 });                                      // xor edx, edx (slot 0)
        h.put({ 0x41, 0xFF, 0xD3 });                                // call r11, no stack of its own
        h.put({ 0x4C, 0x8D, 0x15 }); page_rel_h();                  // lea r10, [page]
        h.land(to_write);
        h.put({ 0xF2, 0x41, 0x0F, 0x10, 0x82 }); h.put32(DATA_HOLD_ANGLES);   // movsd xmm0, [r10 + hold angles]
        h.put({ 0xF2, 0x0F, 0x11, 0x00 });                          // movsd [rax], xmm0
        h.put({ 0x41, 0x8B, 0x8A }); h.put32(DATA_HOLD_ANGLES + 8); // mov ecx, [r10 + hold angles + 8]
        h.put({ 0x89, 0x48, 0x08 });                                // mov [rax + 8], ecx
        for (auto jump : hold_off)
            h.land(jump);
        h.put({ 0xC3 });                                            // ret
    }

    // Following in first person, called with rbx the view & r11 the target: their eyes & where they look the way the
    // game shows them, moved between the ticks by the game in this very frame. What the view has is what it starts from
    Code k;
    {
        const int32_t CAMERA = static_cast<int32_t>(offsets::view::m_pCameraServices);
        auto page_rel_k = [&]() { k.put32(-static_cast<int32_t>(STUB_CALC + k.size() + 4)); };
        std::vector<size_t> calc_off;
        k.put({ 0x48, 0x83, 0xEC, 0x28 });                          // sub rsp, 0x28
        k.put({ 0x4C, 0x8D, 0x15 }); page_rel_k();                  // lea r10, [page]
        k.put({ 0x41, 0x8B, 0x82 }); k.put32(DATA_CALC_ON);         // mov eax, [r10 + calc on]
        k.put({ 0x85, 0xC0 });                                      // test eax, eax
        calc_off.push_back(k.jump({ 0x0F, 0x84 }));                 // jz done
        k.put({ 0x49, 0x8B, 0x82 }); k.put32(PAGE_CALC_VIEW);       // mov rax, [r10 + CalcView]
        k.put({ 0x48, 0x85, 0xC0 });                                // test rax, rax
        calc_off.push_back(k.jump({ 0x0F, 0x84 }));                 // jz done
        k.put({ 0x49, 0x8B, 0x8B }); k.put32(CAMERA);               // mov rcx, [r11 + camera services]
        k.put({ 0x48, 0x85, 0xC9 });                                // test rcx, rcx
        calc_off.push_back(k.jump({ 0x0F, 0x84 }));                 // jz done

        k.put({ 0xF2, 0x0F, 0x10, 0x83 }); k.put32(ORIGIN);         // movsd xmm0, [rbx + origin]
        k.put({ 0xF2, 0x41, 0x0F, 0x11, 0x82 }); k.put32(DATA_CALC_ORIGIN); // movsd [r10 + calc origin], xmm0
        k.put({ 0x8B, 0x93 }); k.put32(ORIGIN + 8);                 // mov edx, [rbx + origin + 8]
        k.put({ 0x41, 0x89, 0x92 }); k.put32(DATA_CALC_ORIGIN + 8); // mov [r10 + calc origin + 8], edx
        k.put({ 0xF2, 0x0F, 0x10, 0x83 }); k.put32(ANGLES);         // movsd xmm0, [rbx + angles]
        k.put({ 0xF2, 0x41, 0x0F, 0x11, 0x82 }); k.put32(DATA_CALC_ANGLES); // movsd [r10 + calc angles], xmm0
        k.put({ 0x8B, 0x93 }); k.put32(ANGLES + 8);                 // mov edx, [rbx + angles + 8]
        k.put({ 0x41, 0x89, 0x92 }); k.put32(DATA_CALC_ANGLES + 8); // mov [r10 + calc angles + 8], edx
        k.put({ 0x8B, 0x93 }); k.put32(FOV);                        // mov edx, [rbx + fov]
        k.put({ 0x41, 0x89, 0x92 }); k.put32(DATA_CALC_FOV);        // mov [r10 + calc fov], edx

        // CalcView(services, &origin, &angles, &fov)
        k.put({ 0x49, 0x8D, 0x92 }); k.put32(DATA_CALC_ORIGIN);     // lea rdx, [r10 + calc origin]
        k.put({ 0x4D, 0x8D, 0x82 }); k.put32(DATA_CALC_ANGLES);     // lea r8, [r10 + calc angles]
        k.put({ 0x4D, 0x8D, 0x8A }); k.put32(DATA_CALC_FOV);        // lea r9, [r10 + calc fov]
        k.put({ 0xFF, 0xD0 });                                      // call rax
        k.put({ 0x4C, 0x8D, 0x15 }); page_rel_k();                  // lea r10, [page]

        for (int32_t a = 0; a < 3; a++) {
            k.put({ 0xF3, 0x41, 0x0F, 0x10, 0x82 }); k.put32(DATA_CALC_ORIGIN + a * 4);  // movss xmm0, [r10 + calc origin]
            k.put({ 0xF3, 0x41, 0x0F, 0x58, 0x42, static_cast<uint8_t>(DATA_ORIGIN + a * 4) }); // addss xmm0, [r10 + offset]
            k.put({ 0xF3, 0x0F, 0x11, 0x83 }); k.put32(ORIGIN + a * 4);                  // movss [rbx + origin], xmm0
        }
        k.put({ 0xF2, 0x41, 0x0F, 0x10, 0x82 }); k.put32(DATA_CALC_ANGLES); // movsd xmm0, [r10 + calc angles]
        k.put({ 0xF2, 0x0F, 0x11, 0x83 }); k.put32(ANGLES);         // movsd [rbx + angles], xmm0
        k.put({ 0x41, 0x8B, 0x82 }); k.put32(DATA_CALC_ANGLES + 8); // mov eax, [r10 + calc angles + 8]
        k.put({ 0x89, 0x83 }); k.put32(ANGLES + 8);                 // mov [rbx + angles + 8], eax

        // Watching alive: their field of view, zoomed while they scope. Nothing odd (NaN fails the first compare)
        k.put({ 0x41, 0x8B, 0x82 }); k.put32(DATA_CALC_FOV_ON);     // mov eax, [r10 + calc fov on]
        k.put({ 0x85, 0xC0 });                                      // test eax, eax
        calc_off.push_back(k.jump({ 0x0F, 0x84 }));                 // jz done
        k.put({ 0xF3, 0x41, 0x0F, 0x10, 0x82 }); k.put32(DATA_CALC_FOV);   // movss xmm0, [r10 + calc fov]
        k.put({ 0x41, 0x0F, 0x2F, 0x82 }); k.put32(CONST_MIN_FOV);  // comiss xmm0, [r10 + min]
        calc_off.push_back(k.jump({ 0x0F, 0x82 }));                 // jb done
        k.put({ 0x41, 0x0F, 0x2F, 0x82 }); k.put32(CONST_MAX_FOV);  // comiss xmm0, [r10 + max]
        calc_off.push_back(k.jump({ 0x0F, 0x87 }));                 // ja done
        k.put({ 0xF3, 0x0F, 0x11, 0x83 }); k.put32(FOV);            // movss [rbx + fov], xmm0

        for (auto jump : calc_off)
            k.land(jump);
        k.put({ 0x48, 0x83, 0xC4, 0x28 });                          // add rsp, 0x28
        k.put({ 0xC3 });                                            // ret
    }

    if (STUB + c.size() > STUB_OBSERVER || STUB_OBSERVER + o.size() > STUB_HOLD || STUB_HOLD + h.size() > TABLE
        || STUB_PUNCH + punch.size() > STUB_CALC
        || STUB_CALC + k.size() > DATA_CALC) {
        LOGF(WARNING, "Free cam stub too large");
        p->free_remote(this->page);
        this->page = 0;
        return false;
    }

    std::vector<uint8_t> data(PAGE_SIZE, 0xCC);
    auto put64 = [&](uintptr_t at, uint64_t value) { std::memcpy(&data[at], &value, sizeof(value)); };

    std::fill(data.begin(), data.begin() + STUB, 0);
    put64(PAGE_MAGIC, MAGIC);
    put64(PAGE_VTABLE, this->vtable);
    put64(PAGE_ORIGINAL, override_view);
    put64(PAGE_LOCAL_PAWN, offsets::camera::getLocalPawn ? client.base + offsets::camera::getLocalPawn : 0);
    put64(PAGE_INPUT, offsets::input::dwCSGOInput ? client.base + offsets::input::dwCSGOInput : 0);
    put64(PAGE_CALC_VIEW, offsets::camera::calcView ? client.base + offsets::camera::calcView : 0);
    put64(PAGE_VIEW_ANGLES, offsets::input::viewAnglesPtr ? client.base + offsets::input::viewAnglesPtr : 0);
    auto put_float = [&](uintptr_t at, float value) { std::memcpy(&data[at], &value, sizeof(value)); };
    put_float(DATA_PUNCH_SCALE, 1.f);
    put_float(CONST_180, 180.f);
    put_float(CONST_MINUS_180, -180.f);
    put_float(CONST_360, 360.f);
    put_float(CONST_MAX_PUNCH, MAX_PUNCH);
    put_float(CONST_MIN_FOV, 10.f);
    put_float(CONST_MAX_FOV, 170.f);
    std::copy(c.bytes.begin(), c.bytes.end(), data.begin() + STUB);
    std::copy(o.bytes.begin(), o.bytes.end(), data.begin() + STUB_OBSERVER);
    std::copy(h.bytes.begin(), h.bytes.end(), data.begin() + STUB_HOLD);
    std::copy(punch.bytes.begin(), punch.bytes.end(), data.begin() + STUB_PUNCH);
    std::copy(k.bytes.begin(), k.bytes.end(), data.begin() + STUB_CALC);

    // The spectator camera: what the call goes to now, or what our page from an earlier run it still goes to kept
    if (offsets::camera::observerViewCall) {
        auto call = client.base + offsets::camera::observerViewCall;
        auto target = call + 5 + p->read<int32_t>(call + 1);
        if (!in_client(target)) {
            auto old = target - STUB_OBSERVER;
            target = p->read<uint64_t>(old + PAGE_MAGIC) == MAGIC ? p->read<uintptr_t>(old + PAGE_OBSERVER_VIEW) : 0;
            if (target && PatchCall(call, target))
                LOGF(INFO, "Put back the spectator camera call an earlier run left on its page");
        }

        // Out of reach of a call rel32, the page went somewhere else
        auto distance = static_cast<int64_t>(this->page + STUB_OBSERVER) - static_cast<int64_t>(call + 5);
        if (target && in_client(target) && distance >= INT32_MIN && distance <= INT32_MAX) {
            this->observer_view = target;
            put64(PAGE_OBSERVER_VIEW, target);
        }
        else {
            LOGF(WARNING, "The spectator camera can't go through our page, first person while dead uses our camera only");
        }
    }

    // The spectator keys through a filter: the ones switching the player or the mode are taken, the rest (the
    // loadout on F & co) goes on to the game. jmp rel32 over its start, the page must be in reach
    this->binds_filter = false;
    if (offsets::camera::spectatorBinds && TABLE + sizeof(uintptr_t) * (entries.size() + 1) <= STUB_BINDS) {
        auto binds = client.base + offsets::camera::spectatorBinds;
        auto distance = static_cast<int64_t>(this->page + STUB_BINDS) - static_cast<int64_t>(binds + 5);
        if (distance >= INT32_MIN && distance <= INT32_MAX) {
            Code f;
            auto here = [&]() { return STUB_BINDS + f.size(); };
            auto back = [&](std::initializer_list<uint8_t> opcode, size_t label) {  // jcc / jmp rel32 to a label before
                f.put(opcode);
                f.put32(static_cast<int32_t>(static_cast<int64_t>(label) - static_cast<int64_t>(f.size() + 4)));
            };

            // rdx the command bound to the key
            f.put({ 0x48, 0x85, 0xD2 });                                // test rdx, rdx
            auto no_command = f.jump({ 0x0F, 0x84 });                   // jz pass
            f.put({ 0x56, 0x57 });                                      // push rsi; push rdi
            f.put({ 0x48, 0x8D, 0x35 });                                // lea rsi, [rip + names]
            f.put32(static_cast<int32_t>(BIND_NAMES - (here() + 4)));
            size_t next = f.size();
            f.put({ 0x80, 0x3E, 0x00 });                                // cmp byte [rsi], 0
            auto none = f.jump({ 0x0F, 0x84 });                         // je pass_pop: the list ended
            f.put({ 0x48, 0x89, 0xD7 });                                // mov rdi, rdx
            size_t compare = f.size();
            f.put({ 0x8A, 0x06 });                                      // mov al, [rsi]
            f.put({ 0x84, 0xC0 });                                      // test al, al
            auto name_end = f.jump({ 0x0F, 0x84 });                     // je name_end
            f.put({ 0x3A, 0x07 });                                      // cmp al, [rdi]
            auto differs = f.jump({ 0x0F, 0x85 });                      // jne skip
            f.put({ 0x48, 0xFF, 0xC6 });                                // inc rsi
            f.put({ 0x48, 0xFF, 0xC7 });                                // inc rdi
            back({ 0xE9 }, compare);                                    // jmp compare
            f.land(name_end);
            f.put({ 0x48, 0xFF, 0xC6 });                                // inc rsi, past its 0
            f.put({ 0x80, 0x3F, 0x00 });                                // cmp byte [rdi], 0
            auto taken = f.jump({ 0x0F, 0x84 });                        // je block: the whole command
            back({ 0xE9 }, next);                                       // jmp next
            f.land(differs);
            size_t skip = f.size();                                     // skip: to the next name
            f.put({ 0x8A, 0x06 });                                      // mov al, [rsi]
            f.put({ 0x48, 0xFF, 0xC6 });                                // inc rsi
            f.put({ 0x84, 0xC0 });                                      // test al, al
            back({ 0x0F, 0x85 }, skip);                                 // jnz skip
            back({ 0xE9 }, next);                                       // jmp next
            f.land(taken);
            f.put({ 0x5F, 0x5E });                                      // pop rdi; pop rsi
            f.put({ 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 });              // mov eax, 1; ret: not handled, sends nothing
            f.land(none);
            f.put({ 0x5F, 0x5E });                                      // pop rdi; pop rsi
            f.land(no_command);
            f.bytes.insert(f.bytes.end(), std::begin(BINDS_ORIGINAL), std::end(BINDS_ORIGINAL));  // what the jmp covers
            f.put({ 0xE9 });                                            // jmp back after it
            f.put32(static_cast<int32_t>(static_cast<int64_t>(binds + sizeof(BINDS_ORIGINAL)) - static_cast<int64_t>(this->page + here() + 4)));

            if (STUB_BINDS + f.size() <= BIND_NAMES) {
                std::copy(f.bytes.begin(), f.bytes.end(), data.begin() + STUB_BINDS);

                // Names one after the other, an empty one ends them
                size_t at = BIND_NAMES;
                for (const char* name : TAKEN_BINDS) {
                    size_t length = std::strlen(name) + 1;
                    std::memcpy(&data[at], name, length);
                    at += length;
                }
                data[at] = 0;
                this->binds_filter = true;
            }
        }
    }

    // The copy, with the RTTI pointer right before it like the real one, OverrideView going through the stub
    put64(TABLE,p->read<uintptr_t>(this->vtable - sizeof(uintptr_t)));
    for (size_t i = 0; i < entries.size(); i++)
        put64(TABLE + sizeof(uintptr_t) * (i + 1), i == index ? this->page + STUB : entries[i]);

    p->write_bytes(this->page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), data.size());

    this->copy = this->page + TABLE + sizeof(uintptr_t);
    p->write<uintptr_t>(this->object, this->copy);
    this->installed = true;

    LOGF(INFO, "The camera goes through our stub (OverrideView entry {} of {})", index, entries.size());
    return true;
}

bool Freecam::SetupGameInput() {
    auto p = Engine::GetProcess();

    auto set_event_enabled = p->FindExport("SDL3.dll", "SDL_SetEventEnabled");
    if (!set_event_enabled) {
        LOGF(WARNING, "SDL_SetEventEnabled was not found, the free cam keeps the mouse from the game through the hook only");
        return false;
    }

    auto events = [](size_t count) {
        Code c;
        c.put({ 0x53 });                                    // push rbx
        c.put({ 0x48, 0x83, 0xEC, 0x20 });                  // sub rsp, 0x20
        c.put({ 0x48, 0x89, 0xCB });                        // mov rbx, rcx
        for (size_t k = 0; k < count; k++) {
            c.put({ 0xB9 }); c.put32(static_cast<int32_t>(BLOCKED_EVENTS[k])); // mov ecx, type
            c.put({ 0x0F, 0xB6, 0x53, 0x08 });              // movzx edx, byte [rbx + 8]
            c.put({ 0xFF, 0x13 });                          // call [rbx]
        }
        c.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
        c.put({ 0x5B });                                    // pop rbx
        c.put({ 0x31, 0xC0 });                              // xor eax, eax
        c.put({ 0xC3 });                                    // ret
        return c;
    };
    auto all = events(std::size(BLOCKED_EVENTS)), mouse = events(MOUSE_EVENTS);

    if (INPUT_CODE_ALL + all.size() > INPUT_CODE_MOUSE || INPUT_CODE_MOUSE + mouse.size() > INPUT_ENABLE)
        return false;

    this->input_code = p->allocate_remote(INPUT_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->input_code)
        return false;

    struct { uint64_t function; uint64_t enabled; } enable{ set_event_enabled, 1 }, disable{ set_event_enabled, 0 };
    p->write_bytes(this->input_code + INPUT_CODE_ALL, all.bytes);
    p->write_bytes(this->input_code + INPUT_CODE_MOUSE, mouse.bytes);
    p->write(this->input_code + INPUT_ENABLE, enable);
    p->write(this->input_code + INPUT_DISABLE, disable);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->input_code), INPUT_SIZE);
    return true;
}

void Freecam::BlockGameInput(Input level, bool force) {
    if (!this->input_code || (this->input_blocked == level && !force))
        return;

    // All of it back first unless all of it goes, then the mouse alone
    auto p = Engine::GetProcess();
    bool done = level == Input::All
        ? p->call_remote(this->input_code + INPUT_CODE_ALL, this->input_code + INPUT_DISABLE)
        : p->call_remote(this->input_code + INPUT_CODE_ALL, this->input_code + INPUT_ENABLE);
    if (done && level == Input::Mouse)
        done = p->call_remote(this->input_code + INPUT_CODE_MOUSE, this->input_code + INPUT_DISABLE);

    if (done)
        this->input_blocked = level;
    else
        LOGF(WARNING, "Failed to set the input of the game ({})", GetLastError());
}

bool Freecam::OnKey(DWORD vk, bool down) {
    auto& i = GetInstance();

    for (int key = 0; key < KEY_COUNT; key++) {
        for (DWORD same : KEYS[key]) {
            if (same && same == vk) {
                bool pressed = down && !i.held[key];
                i.held[key] = down;

                // Dead & watching a player only space (the mode) & W S (closer, further) are ours: crouch & the
                // rest go to the game
                if (i.dead_camera && i.mode != Mode::Free)
                    return i.blocking && (key == KEY_UP || key == KEY_FORWARD || key == KEY_BACK);

                // Watching alive the keys that move us are ours while the game hides the cursor: shown, the chat or a
                // menu of the game takes them. A press goes to the game whole or not at all
                if (i.alive_spectate) {
                    if (pressed)
                        i.key_taken[key] = i.blocking && Movement::IsPlaying();
                    return i.key_taken[key];
                }
                return i.blocking;
            }
        }
    }
    return false;
}

void Freecam::Thread() {
    auto last = std::chrono::steady_clock::now();

    while (!this->stopping) {
        // Movement asked for 1ms timer resolution
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto now = std::chrono::steady_clock::now();
        float seconds = std::min(std::chrono::duration<float>(now - last).count(), 0.05f);
        last = now;

        Update(seconds);

        for (int key = 0; key < KEY_COUNT; key++)
            this->was_held[key] = this->held[key];
    }
}

void Freecam::SetMode(Mode wanted) {
    auto p = Engine::GetProcess();

    // Watching alive is following, anything else ends it
    if (wanted != Mode::Follow)
        this->alive_spectate = false;

    if (wanted == this->mode)
        return;

    // Third person starts behind them again
    this->orbiting = false;

    // The game saw the keys we now take go down, they would keep it moving
    if (this->mode == Mode::Off) {
        for (int key = 0; key < KEY_COUNT; key++)
            if (this->held[key])
                ReleaseKey(KEYS[key][0]);
    }

    this->mode = wanted;

    // Dead, UpdateDead decides: first person is the spectator camera of the game
    if (!this->dead_camera)
        p->write<uint32_t>(this->page + DATA_MODE, static_cast<uint32_t>(wanted));
}

void Freecam::Update(float seconds) {
    auto p = Engine::GetProcess();
    if (!p || !this->installed)
        return;

    // The game put its own vtable back
    if (p->read<uintptr_t>(this->object) != this->copy) {
        if (p->read<uintptr_t>(this->object) == this->vtable)
            p->write<uintptr_t>(this->object, this->copy);
        else
            return;
    }

    // The lobby keeps the pawn of the last match: none there, like Globals (the lobby still counts one client)
    auto globals = p->read<uintptr_t>(Engine::GetClient().base + offsets::globalVars);
    bool in_match = globals && p->read<int>(globals + offsets::global::maxClients) > 1;

    // A match going on: the rules exist & it did not end. Ended, the mouse goes to the scoreboard & its buttons
    auto rules = in_match ? p->read<uintptr_t>(Engine::GetClient().base + offsets::rules::dwGameRules) : 0;
    in_match = rules && p->read<int32_t>(rules + offsets::rules::m_gamePhase) != GAMEPHASE_MATCH_ENDED;

    // Not IsPlaying: the cursor the game shows after a while of nothing pressed would hand the mouse & keys back
    bool playing = Movement::HasFocus();
    auto pawn = in_match ? Engine::GetLocalPawn() : 0;
    bool alive = pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) > 0;

    // Dying, a new life or another match starts over with the camera of the game
    if (pawn != this->local_pawn || alive != this->local_alive) {
        this->local_pawn = pawn;
        this->local_alive = alive;
        this->spectate_request = 0;     // Asked in the life before
        EndDead();
        SetMode(Mode::Off);
    }

    // An emote playing: the mouse goes around our player, only alive with no camera of ours on
    UpdateEmoteCamera(alive && this->mode == Mode::Off ? pawn : 0, seconds);

    // Watching alive through their eyes: their body & what they wear not drawn, the head would be in front of the camera
    uintptr_t hide = this->alive_spectate && this->mode == Mode::Follow && this->first_person ? this->target.load() : 0;
    if (hide != this->hidden_body) {
        this->hidden_body = hide;
        MaterialChams::SetHiddenPlayer(hide);
    }
    UpdateAliveView(pawn, alive);
    if (this->alive_spectate != this->holding) {
        this->holding = this->alive_spectate;
        p->write<uint32_t>(this->page + DATA_HOLD_ON, this->holding ? 1 : 0);
    }

    // Dead: like casual on its own, no key needed. Only in a team: before choosing one (deathmatch, practice,
    // joining) or as a spectator the game shows its team menu & its own spectator camera
    uint8_t team = pawn ? p->read<uint8_t>(pawn + offsets::pawn::m_iTeamNum) : 0;
    bool in_team = team == TEAM_T || team == TEAM_CT;
    bool dead = pawn && !alive && in_team && cfg::view::dead_spectate && IsDeadAvailable();
    if (dead && !this->dead_camera)
        StartDead(pawn);
    else if (!dead && this->dead_camera)
        EndDead();

    if (this->dead_camera) {
        this->lent_mouse = false;

        // Its clicks only while the game hides the cursor: shown, a menu of the game takes them (teams, buying,
        // pause), they go to it
        this->blocking = this->mode != Mode::Off && playing && Movement::IsPlaying();
        // Dead there is no player to move: the keys of the game (use, radio, ...) all stay, ours are taken by
        // the hook alone
        BlockGameInput(Input::None);
        UpdateDead(pawn, playing, seconds);
        WriteFov();
        WritePunch(false);
        return;
    }

    // Watching someone alive, from the profile page
    UpdateSpectate(alive && in_team ? pawn : 0);

    // The key turns it on & off
    bool freecam_key = cfg::view::freecam && Movement::IsKeyUsable(cfg::view::freecam_key) && (GetAsyncKeyState(cfg::view::freecam_key) & 0x8000);
    bool freecam_pressed = freecam_key && !this->freecam_key_was_down;
    this->freecam_key_was_down = freecam_key;

    if (alive && freecam_pressed) {
        if (this->mode == Mode::Free) {
            SetMode(Mode::Off);
        }
        else {
            // From our eyes, looking where we look
            auto node = p->read<uintptr_t>(pawn + offsets::pawn::m_pGameSceneNode);
            StartFree(p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) + p->read<Vec3_t>(pawn + offsets::pawn::m_vecViewOffset),
                p->read<Vec3_t>(this->page + DATA_VIEW));
        }
    }

    // Turned off in the menu (watching alive is no free cam)
    if (this->mode != Mode::Off && !cfg::view::freecam && !this->alive_spectate)
        SetMode(Mode::Off);

    // Free cam: the game gets none of the mouse or the keys. Watching alive our player stands still: the mouse while
    // the game hides the cursor (shown, a menu of the game takes it), the keys that move us by the hook (OnKey)
    this->blocking = this->mode != Mode::Off && playing;
    this->alive_mouse = this->blocking && this->alive_spectate && Movement::IsPlaying();
    // The emote wheel open: the mouse picks on it, the view stays
    bool lent = std::chrono::steady_clock::now().time_since_epoch().count() < this->lent_until;
    this->lent_mouse = lent && alive && this->mode == Mode::Off && Movement::IsPlaying();
    BlockGameInput(this->blocking && this->mode == Mode::Free ? Input::All
        : this->alive_mouse || this->lent_mouse || this->emote_mouse ? Input::Mouse : Input::None);

    switch (this->mode) {
    case Mode::Free:    UpdateFree(seconds);    break;
    case Mode::Follow:  UpdateFollow(seconds);  break;
    default:                                    break;
    }

    WriteFov();
    WritePunch(alive && this->mode == Mode::Off);
}

void Freecam::WritePunch(bool alive) {
    auto p = Engine::GetProcess();

    // 100% is the game itself
    bool on = alive && cfg::view::punch_enabled && cfg::view::punch_scale < 100.f && offsets::input::dwCSGOInput;
    float scale = std::clamp(cfg::view::punch_scale, 0.f, 100.f) / 100.f;

    if (on != this->punch_on || scale != this->punch_scale) {
        p->write<float>(this->page + DATA_PUNCH_SCALE, scale);
        p->write<uint32_t>(this->page + DATA_PUNCH_ON, on ? 1 : 0);
        this->punch_on = on;
        this->punch_scale = scale;
    }

    // What the camera kicked, now & then while it does: tells whether the kick is in the view the stub gets
    auto now = std::chrono::steady_clock::now();
    if (on && now >= this->punch_logged) {
        auto seen = p->read<Vec3_t>(this->page + DATA_PUNCH_SEEN);
        if (std::abs(seen.x) > 0.05f || std::abs(seen.y) > 0.05f) {
            LOGF(VERBOSE, "View punch: kick ({:.2f}, {:.2f}, {:.2f}) shown at {:.0f}%", seen.x, seen.y, seen.z, scale * 100.f);
            this->punch_logged = now + std::chrono::seconds(1);
        }
    }
}

void Freecam::WriteFov() {
    // The one of the FOV changer, else the default of the game
    float fov = this->mode == Mode::Off ? 0.f : cfg::view::fov_enabled ? static_cast<float>(cfg::view::fov) : DEFAULT_FOV;
    if (fov != this->fov_written) {
        Engine::GetProcess()->write<float>(this->page + DATA_FOV, fov);
        this->fov_written = fov;
    }
}

void Freecam::UpdateFree(float seconds) {
    auto p = Engine::GetProcess();
    if (!this->blocking)
        return;

    if (TurnByMouse(this->free_angles, cfg::view::freecam_sensitivity))
        p->write<Vec3_t>(this->page + DATA_FREE_ANGLES, this->free_angles);

    auto forward = Forward(this->free_angles), right = Right(this->free_angles);

    auto axis = [&](Key plus, Key minus) { return (Held(plus) ? 1.f : 0.f) - (Held(minus) ? 1.f : 0.f); };
    float f = axis(KEY_FORWARD, KEY_BACK), r = axis(KEY_RIGHT, KEY_LEFT), u = axis(KEY_UP, KEY_DOWN);

    Vec3_t move(forward.x * f + right.x * r, forward.y * f + right.y * r, forward.z * f + right.z * r + u);
    float length = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
    if (length < 0.001f)
        return;

    float step = cfg::view::freecam_speed * (Held(KEY_FAST) ? FAST : 1.f) * seconds / length;
    this->position = Vec3_t(this->position.x + move.x * step, this->position.y + move.y * step, this->position.z + move.z * step);
    p->write<Vec3_t>(this->page + DATA_ORIGIN, this->position);
}

bool Freecam::TurnByMouse(Vec3_t& angles, float multiplier) {
    auto p = Engine::GetProcess();

    // The mouse, kept from the game by the hook: like the game turns the view with it
    int dx = this->mouse_dx.exchange(0), dy = this->mouse_dy.exchange(0);
    if (!dx && !dy)
        return false;

    if (!this->sensitivity_convar)
        this->sensitivity_convar = View::FindConVar("sensitivity");
    float sensitivity = this->sensitivity_convar ? p->read<float>(this->sensitivity_convar + View::CONVAR_VALUE) : 1.f;
    if (!(sensitivity > 0.f && sensitivity < 100.f))
        sensitivity = 1.f;

    // The hook sees the cursor, moved by the pointer speed of Windows: back to the counts of the mouse the game
    // turns by, then our own multiplier
    sensitivity *= multiplier / PointerSpeed();

    angles.y = std::remainder(angles.y - dx * sensitivity * MOUSE_DEGREES, 360.f);
    angles.x = std::clamp(angles.x + dy * sensitivity * MOUSE_DEGREES, -MAX_PITCH, MAX_PITCH);
    return true;
}

bool Freecam::PickTarget(int step) {
    auto p = Engine::GetProcess();
    auto local = Engine::GetLocalPawn();

    // The living players we may follow, in the order of the scoreboard
    std::vector<uintptr_t> targets;
    auto snapshot = Cache::CopySnapshot();
    std::sort(snapshot.players.begin(), snapshot.players.end(), [](const Player& a, const Player& b) { return a.index < b.index; });

    for (const auto& player : snapshot.players) {
        auto pawn = player.GetPawnAddress();
        if (!pawn || pawn == local || !player.alive || player.localplayer || p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0)
            continue;
        targets.push_back(pawn);
    }

    if (targets.empty())
        return false;

    auto it = std::find(targets.begin(), targets.end(), this->target.load());
    size_t at = 0;
    if (it != targets.end())
        at = (static_cast<size_t>(it - targets.begin()) + targets.size() + step) % targets.size();

    this->target = targets[at];
    p->write<uintptr_t>(this->page + DATA_TARGET, this->target);
    return true;
}

void Freecam::UpdateFollow(float seconds) {
    auto p = Engine::GetProcess();

    // Alive & watching the one picked: back to our own eyes when they die
    if (this->alive_spectate && !IsLiving(this->target)) {
        SetMode(Mode::Off);
        return;
    }

    // Dead: the next one, none left ends it (dead ourselves, the free cam from there)
    if (!this->target || p->read<int>(this->target + offsets::pawn::m_iHealth) <= 0) {
        if (!PickTarget(1)) {
            if (this->dead_camera)
                StartFree(CameraPosition(), CameraAngles());
            else
                SetMode(Mode::Off);
            return;
        }
    }

    if (this->blocking) {
        float zoom = (Held(KEY_BACK) ? 1.f : 0.f) - (Held(KEY_FORWARD) ? 1.f : 0.f);
        this->distance = std::clamp(this->distance + zoom * DISTANCE_SPEED * seconds, MIN_DISTANCE, MAX_DISTANCE);
    }

    auto angles = SmoothAngles(p->read<Vec3_t>(this->target + offsets::grenade::m_angEyeAngles));

    // Dead in third person the mouse turns the camera around them, like the chase camera of the game. It starts
    // behind where they look, another player keeps it
    bool orbit = this->dead_camera && !this->first_person;
    if (orbit && !this->orbiting) {
        this->orbit_angles = angles;
        this->mouse_dx = this->mouse_dy = 0;
    }
    this->orbiting = orbit;
    if (orbit) {
        if (this->blocking)
            TurnByMouse(this->orbit_angles, 1.f);
        angles = this->orbit_angles;
    }
    p->write<Vec3_t>(this->page + DATA_FOLLOW_ANGLES, angles);

    // Behind them where the camera looks, from their eyes
    Vec3_t offset{};
    if (!this->first_person) {
        auto forward = Forward(angles);
        offset = Vec3_t(-forward.x * this->distance, -forward.y * this->distance, -forward.z * this->distance);
    }
    p->write<Vec3_t>(this->page + DATA_ORIGIN, offset);
    // First person: the eyes & the turning of the game (CalcView), the ones above stay for the rest
    p->write<uint32_t>(this->page + DATA_CALC_ON, this->first_person ? 1 : 0);
    // Alive their field of view too, like the spectator camera of the game shows their zoom when dead
    p->write<uint32_t>(this->page + DATA_CALC_FOV_ON, this->alive_spectate && this->first_person ? 1 : 0);
}

void Freecam::UpdateEmoteCamera(uintptr_t pawn, float seconds) {
    auto p = Engine::GetProcess();
    if (!this->emote_camera || !pawn) {
        EndEmoteCamera();
        return;
    }

    auto node = p->read<uintptr_t>(pawn + offsets::pawn::m_pGameSceneNode);
    if (!node) {
        EndEmoteCamera();
        return;
    }

    // Starts behind where we look, close like the third person of the game
    bool starting = !this->emote_orbit;
    if (starting) {
        auto view = p->read<Vec3_t>(this->page + DATA_VIEW);
        this->emote_angles = Vec3_t(std::clamp(view.x, -MAX_PITCH, MAX_PITCH), view.y, 0.f);
        this->emote_distance = EMOTE_START_DISTANCE;
        this->mouse_dx = this->mouse_dy = 0;
    }

    // The mouse while the game hides the cursor, a menu of the game showing it gets it
    this->emote_mouse = Movement::IsPlaying();
    if (this->emote_mouse)
        TurnByMouse(this->emote_angles, 1.f);

    // Out to the distance of the third person of the game
    if (!this->ideal_distance_convar)
        this->ideal_distance_convar = View::FindConVar("cam_idealdist");
    float ideal = this->ideal_distance_convar ? p->read<float>(this->ideal_distance_convar + View::CONVAR_VALUE) : EMOTE_DEFAULT_DISTANCE;
    ideal = std::isfinite(ideal) ? std::clamp(ideal, EMOTE_MIN_DISTANCE, EMOTE_MAX_DISTANCE) : EMOTE_DEFAULT_DISTANCE;
    this->emote_distance = std::min(ideal, this->emote_distance + (ideal - EMOTE_START_DISTANCE) * seconds / EMOTE_PULL_OUT);

    // Behind the chest where the camera looks, short of a wall in between
    auto forward = Forward(this->emote_angles);
    Vec3_t drop(0.f, 0.f, -EMOTE_PIVOT_DROP);
    auto pivot = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) + p->read<Vec3_t>(pawn + offsets::pawn::m_vecViewOffset) + drop;
    float distance = this->emote_distance;
    MapCollision::Hit hit;
    if (MapCollision::IsLoaded() && MapCollision::Trace(pivot, pivot - forward * (distance + EMOTE_WALL_GAP), hit))
        distance = std::max(EMOTE_WALL_GAP, (distance + EMOTE_WALL_GAP) * hit.fraction - EMOTE_WALL_GAP);

    p->write<Vec3_t>(this->page + DATA_FOLLOW_ANGLES, this->emote_angles);
    p->write<Vec3_t>(this->page + DATA_ORIGIN, drop - forward * distance);

    // The stub follows our pawn (its follow path, the C++ mode stays Off), everything written before
    if (starting) {
        p->write<uintptr_t>(this->page + DATA_TARGET, pawn);
        p->write<uint32_t>(this->page + DATA_CALC_ON, 0);
        p->write<uint32_t>(this->page + DATA_CALC_FOV_ON, 0);
        p->write<uint32_t>(this->page + DATA_MODE, static_cast<uint32_t>(Mode::Follow));
        this->emote_orbit = true;
    }
}

void Freecam::EndEmoteCamera() {
    this->emote_mouse = false;
    if (!this->emote_orbit)
        return;

    // The camera of the game again, unless another of ours took over meanwhile
    this->emote_orbit = false;
    if (this->mode == Mode::Off && !this->dead_camera)
        Engine::GetProcess()->write<uint32_t>(this->page + DATA_MODE, static_cast<uint32_t>(Mode::Off));
}

void Freecam::UpdateSpectate(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    int health = pawn ? p->read<int>(pawn + offsets::pawn::m_iHealth) : 0;

    // Picked in the menu: through their eyes
    if (auto wanted = this->spectate_request.exchange(0); wanted && pawn && IsLiving(wanted)) {
        this->target = wanted;
        p->write<uintptr_t>(this->page + DATA_TARGET, wanted);
        this->first_person = true;
        this->spectate_health = health;
        // Where we look now (the view the stub saw last), kept while watching. Switching to another player keeps
        // it: the view is the one of the player watched then
        if (!this->alive_spectate) {
            auto view = p->read<Vec3_t>(this->page + DATA_VIEW);
            p->write<Vec3_t>(this->page + DATA_HOLD_ANGLES, Vec3_t(view.x, view.y, 0.f));
        }
        this->spectate_stop = false;
        SetMode(Mode::Follow);
        this->alive_spectate = true;
        LOGF(VERBOSE, "Watching 0x{:X} while alive", wanted);
    }

    bool stop = this->spectate_stop.exchange(false);
    if (!this->alive_spectate)
        return;

    // Unspectate in the menu, a hit on us (our player stands there blind) or a death ends it, never another player.
    // Clicks only stay from the game (no shot). Healing is no hit
    bool hit = health < this->spectate_health;
    this->spectate_health = health;
    if (stop || !pawn || hit || !IsLiving(this->target)) {
        LOGF(VERBOSE, "Stopped watching while alive ({})", stop ? "asked" : !pawn ? "we died" : hit ? "we were hit" : "they are gone");
        SetMode(Mode::Off);
    }
}

void Freecam::UpdateAliveView(uintptr_t pawn, bool alive) {
    auto p = Engine::GetProcess();

    uint32_t handle = this->alive_spectate && this->mode == Mode::Follow && this->first_person && alive && this->target
        && offsets::camera::getLocalPawn ? HandleOf(this->target) : 0;

    // Nothing to watch any more or another pawn: what the game had back
    if (this->alive_view_services && (!handle || pawn != this->alive_view_pawn))
        EndAliveView(pawn, alive);

    if (!this->alive_spectate)
        this->alive_view_told = false;
    if (!handle)
        return;

    if (!this->alive_view_services) {
        // Alive our pawn has none, the game gives them to spectators: ours in for the time
        auto services = p->read<uintptr_t>(pawn + offsets::pawn::m_pObserverServices);
        if (!services && (services = MakeServices(pawn))) {
            p->write<uintptr_t>(pawn + offsets::pawn::m_pObserverServices, services);
            this->own_services = services;
        }
        if (!services) {
            if (!this->alive_view_told) {
                this->alive_view_told = true;
                LOGF(VERBOSE, "The game can't spectate with us alive: no spectator services (vtable 0x{:X})", offsets::camera::observerServicesVtable);
            }
            return;
        }

        // What the game had, to put back
        this->alive_view_services = services;
        this->alive_view_pawn = pawn;
        this->alive_view_mode = p->read<uint8_t>(services + offsets::observerServices::m_iObserverMode);
        this->alive_view_target = p->read<uint32_t>(services + offsets::observerServices::m_hObserverTarget);
        this->alive_view_since = std::chrono::steady_clock::now();
        // A spectator now, its keys would send spec_next & co to the server
        PatchBinds(true);
        LOGF(VERBOSE, "The game spectates 0x{:X} with us alive ({} spectator services 0x{:X})", this->target.load(),
            this->own_services ? "our" : "its", services);
    }
    auto services = this->alive_view_services;

    // Each frame by the stub like dead, the pawn last: it reads the pawn first
    p->write<uint8_t>(this->page + DATA_OBSERVER_MODE, OBSERVER_FIRST);
    p->write<uint32_t>(this->page + DATA_OBSERVER_TARGET, handle);
    p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, pawn);

    // Also right away, for what reads them outside of the view (the HUD, the radar)
    if (p->read<uint8_t>(services + offsets::observerServices::m_iObserverMode) != OBSERVER_FIRST)
        p->write<uint8_t>(services + offsets::observerServices::m_iObserverMode, OBSERVER_FIRST);
    if (p->read<uint32_t>(services + offsets::observerServices::m_hObserverTarget) != handle)
        p->write<uint32_t>(services + offsets::observerServices::m_hObserverTarget, handle);

    // The player the game shows is ours while we live, whatever the services say: past that check now. With no
    // spectating in them it still gives our own pawn (the one of our controller)
    PatchViewPawn(true);

    // Like casual spectating dead: the spectator camera of the game turns the view (their aim, their zoom, the weapon
    // in their hands & the scope), only where it is comes from us (STUB_OBSERVER). Right after starting it still
    // settles: ours alone until then, the spectator camera skipped
    bool position_only = this->observer_view && this->own_services && offsets::camera::observerViewCheck
        && std::chrono::steady_clock::now() >= this->alive_view_since + SWITCH_SETTLE;
    PatchObserverCall(position_only);
    p->write<uint32_t>(this->page + DATA_POSITION_ONLY, position_only ? 1 : 0);
    p->write<uint32_t>(this->page + DATA_MODE, static_cast<uint32_t>(position_only ? Mode::Off : this->mode.load()));
    PatchObserverView(!position_only);

#ifdef _DEBUG
    LogAliveView(pawn);
#endif
}

uintptr_t Freecam::MakeServices(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();
    auto weapons = p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices);
    if (!offsets::camera::observerServicesVtable || !weapons)
        return 0;

    // Page: the destructor that does nothing, yes for the spectator camera, the vtable copy (its object locator
    // before it), the object
    constexpr uintptr_t DTOR = 0x000, YES = 0x010, VTABLE = 0x108, OBJECT = 0x800;
    // A spectator's are larger (m_obsInterpState at 0x68, their keys write it): zeros, nothing to interpolate
    constexpr size_t MAX_ENTRIES = 0x60, DTOR_INDEX = 7, OBJECT_SIZE = 0x100;
    constexpr size_t COMPONENT_SIZE = 0x48;     // CPlayerPawnComponent: the network chain & the pawn
    constexpr size_t CHAIN_LIST = 0x10, CHAIN_LIST_SIZE = 0x18;   // A CUtlVector of the chain, the destructor frees it

    if (!this->services_page) {
        auto page = p->allocate_remote(0x1000, PAGE_EXECUTE_READWRITE);
        if (!page)
            return 0;

        std::vector<uint8_t> data(0x1000, 0);
        constexpr uint8_t RETURN_THIS[] = { 0x48, 0x8B, 0xC1, 0xC3 };  // mov rax, rcx; ret
        constexpr uint8_t RETURN_TRUE[] = { 0xB0, 0x01, 0xC3 };         // mov al, 1; ret
        std::copy(std::begin(RETURN_THIS), std::end(RETURN_THIS), data.begin() + DTOR);
        std::copy(std::begin(RETURN_TRUE), std::end(RETURN_TRUE), data.begin() + YES);

        // The spectator camera asks the services first: the ones of a player pawn say no, a spectator's yes. It turns
        // the view like for the dead, with the weapon in their hands
        auto check = offsets::camera::observerViewCheck;
        size_t check_index = check > 0 && check % sizeof(uintptr_t) == 0 ? static_cast<size_t>(check) / sizeof(uintptr_t) : SIZE_MAX;

        // A vtable ends where its entries stop pointing at code: the RTTI pointer of the next one is still in the image
        std::vector<std::pair<uintptr_t, uintptr_t>> code;
        auto pe = client.base + p->read<int32_t>(client.base + 0x3C);
        auto section = pe + 0x18 + p->read<uint16_t>(pe + 0x14);
        for (uint16_t s = p->read<uint16_t>(pe + 6); s > 0; s--, section += sizeof(IMAGE_SECTION_HEADER)) {
            IMAGE_SECTION_HEADER header{};
            if (p->read_raw(section, &header, sizeof(header)) && (header.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                code.emplace_back(client.base + header.VirtualAddress, client.base + header.VirtualAddress + header.Misc.VirtualSize);
        }
        auto entries = [&](std::ptrdiff_t at) {
            std::vector<uintptr_t> out;
            for (size_t i = 0; at && i < MAX_ENTRIES; i++) {
                auto entry = p->read<uintptr_t>(client.base + at + i * sizeof(uintptr_t));
                if (std::none_of(code.begin(), code.end(), [&](const auto& range) { return entry >= range.first && entry < range.second; }))
                    break;
                out.push_back(entry);
            }
            return out;
        };
        auto base = entries(offsets::camera::observerServicesVtable);
        auto spectator = entries(offsets::camera::spectatorServicesVtable);
        if (base.empty()) {
            p->free_remote(page);
            return 0;
        }

        // A spectator's own where they count: their camera in first person (the camera services of the one watched,
        // their aim punch & zoom; the base one turns ours along) & keys (F their loadout, through our filter), with
        // what only theirs have past the end of the base one (the keys ask it). The rest stays the base one, ours
        // is that small
        auto in_eye = offsets::camera::observerInEye ? client.base + offsets::camera::observerInEye : 0;
        auto keys = offsets::camera::spectatorBinds ? client.base + offsets::camera::spectatorBinds : 0;
        bool camera = false, binds = false;
        auto put64 = [&](uintptr_t at, uint64_t value) { std::memcpy(&data[at], &value, sizeof(value)); };
        put64(VTABLE - sizeof(uintptr_t), p->read<uintptr_t>(client.base + offsets::camera::observerServicesVtable - sizeof(uintptr_t)));
        for (size_t i = 0; i < std::max(base.size(), spectator.size()); i++) {
            uintptr_t entry = i < base.size() ? base[i] : spectator[i];
            if (i < spectator.size() && spectator[i] && (spectator[i] == in_eye || spectator[i] == keys)) {
                entry = spectator[i];
                (entry == in_eye ? camera : binds) = true;
            }
            put64(VTABLE + i * sizeof(uintptr_t), i == DTOR_INDEX ? page + DTOR : i == check_index ? page + YES : entry);
        }
        p->write_bytes(page, data);
        this->services_page = page;
        LOGF(VERBOSE, "Our spectator services: {} entries, {} past the base ones, with {} camera & {} keys", std::max(base.size(), spectator.size()),
            spectator.size() > base.size() ? spectator.size() - base.size() : 0, camera ? "a spectator's" : "the base", binds ? "a spectator's" : "the base");
    }

    // The part every service of the pawn has from its weapon services, the list of the chain empty (never shared).
    // Then watching nobody yet
    std::vector<uint8_t> object(OBJECT_SIZE, 0);
    p->read_raw(weapons, object.data(), COMPONENT_SIZE);
    std::fill_n(object.begin() + CHAIN_LIST, CHAIN_LIST_SIZE, 0);
    uint64_t vtable = this->services_page + VTABLE;
    std::memcpy(object.data(), &vtable, sizeof(vtable));
    uint32_t nobody = 0xFFFFFFFF;
    std::memcpy(object.data() + offsets::observerServices::m_hObserverTarget, &nobody, sizeof(nobody));
    p->write_bytes(this->services_page + OBJECT, object);
    return this->services_page + OBJECT;
}

void Freecam::LogAliveView(uintptr_t pawn) {
    auto now = std::chrono::steady_clock::now();
    if (now < this->shown_next)
        return;
    this->shown_next = now + std::chrono::seconds(1);

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient().base;

    // The function of the alive check, its start 0x3C before the patched byte: push rbx; sub rsp, 0x20; xor ecx, ecx; call
    constexpr uint8_t START[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC9, 0xE8 };
    uintptr_t function = offsets::camera::viewPawnAlive ? client + offsets::camera::viewPawnAlive - 0x3C : 0;
    uint8_t start[sizeof(START)]{};
    if (function && (!p->read_raw(function, start, sizeof(start)) || !std::equal(std::begin(START), std::end(START), start)))
        function = 0;

    // sub rsp, 0x28; mov rax, function; call rax; mov [rip + slot], rax; add rsp, 0x28; ret; slot
    constexpr uintptr_t SLOT = 0x30;
    if (function && !this->shown_code) {
        if ((this->shown_code = p->allocate_remote(0x40, PAGE_EXECUTE_READWRITE))) {
            Code c;
            c.put({ 0x48, 0x83, 0xEC, 0x28 });
            c.put({ 0x48, 0xB8 });
            for (int i = 0; i < 8; i++)
                c.bytes.push_back(static_cast<uint8_t>(function >> (i * 8)));
            c.put({ 0xFF, 0xD0 });
            c.put({ 0x48, 0x89, 0x05 }); c.put32(static_cast<int32_t>(SLOT - (c.size() + 4)));
            c.put({ 0x48, 0x83, 0xC4, 0x28 });
            c.put({ 0xC3 });
            p->write_bytes(this->shown_code, c.bytes);
        }
    }

    uintptr_t shown = 0;
    if (function && this->shown_code) {
        p->write<uintptr_t>(this->shown_code + SLOT, 1);
        if (GameThread::Call(this->shown_code, 500))
            shown = p->read<uintptr_t>(this->shown_code + SLOT);
    }

    auto services = this->alive_view_services;
    auto arms = [&](uintptr_t of) { return of ? Engine::GetEntityFromHandle(p->read<uint32_t>(of + offsets::econ::m_hHudModelArms)) : 0; };
    LOGF(VERBOSE, "Alive view: services mode {} target 0x{:X}, check byte 0x{:02X}, the game shows 0x{:X} (them 0x{:X}, us 0x{:X}), arms ours 0x{:X} theirs 0x{:X}",
        p->read<uint8_t>(services + offsets::observerServices::m_iObserverMode),
        p->read<uint32_t>(services + offsets::observerServices::m_hObserverTarget),
        offsets::camera::viewPawnAlive ? p->read<uint8_t>(client + offsets::camera::viewPawnAlive) : 0,
        shown, this->target.load(), pawn, arms(pawn), arms(this->target));

    // Our eyes must stay where they were kept, whatever theirs do
    auto ours = p->read<Vec3_t>(pawn + offsets::grenade::m_angEyeAngles);
    auto theirs = p->read<Vec3_t>(this->target + offsets::grenade::m_angEyeAngles);
    auto kept = p->read<Vec3_t>(this->page + DATA_HOLD_ANGLES);
    LOGF(VERBOSE, "Alive view angles: ours {:.1f} {:.1f}, kept {:.1f} {:.1f}, theirs {:.1f} {:.1f}", ours.x, ours.y, kept.x, kept.y, theirs.x, theirs.y);
}

void Freecam::PatchViewPawn(bool patched) {
    if (this->view_pawn_patched == patched)
        return;

    // jo never jumps after a test: past the "a player pawn of ours is it" of the HUD one & the "only a spectator pawn"
    // of the first person one, whose "is it" jumps over its return instead (jmp). Only while our spectator services
    // are in, the first person one reads them unchecked. The radar takes the camera (jmp), the eyes we see through,
    // not our pawn
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient().base;
    bool done = true;
    auto patch = [&](std::ptrdiff_t at, uint8_t original, uint8_t ours) {
        uint8_t opcode = patched ? ours : original;
        if (!p->patch_code(client + at, &opcode, sizeof(opcode)))
            done = false;
    };
    if (offsets::camera::viewPawnAlive)
        patch(offsets::camera::viewPawnAlive, 0x85, 0x80);
    if (offsets::camera::viewPawnFirstPerson) {
        patch(offsets::camera::viewPawnFirstPerson + 51, JE_SHORT, JMP_SHORT);
        patch(offsets::camera::viewPawnFirstPerson + 76, JE_SHORT, 0x70);
    }
    if (offsets::camera::radarCenter && offsets::camera::radarLevel) {
        patch(offsets::camera::radarCenter, JE_SHORT, JMP_SHORT);
        patch(offsets::camera::radarLevel, JE_SHORT, JMP_SHORT);
    }

    this->view_pawn_patched = patched;
    if (!done)
        LOGF(WARNING, "Failed to {} the player the game shows ({})", patched ? "patch" : "restore", GetLastError());
}

void Freecam::EndAliveView(uintptr_t pawn, bool alive) {
    auto p = Engine::GetProcess();

    // Our camera back first, then the services
    PatchObserverCall(false);
    p->write<uint32_t>(this->page + DATA_POSITION_ONLY, 0);
    p->write<uint32_t>(this->page + DATA_MODE, static_cast<uint32_t>(this->mode.load()));
    PatchObserverView(false);

    PatchViewPawn(false);
    p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, 0);
    p->write<uint8_t>(this->page + DATA_OBSERVER_MODE, 0);
    p->write<uint32_t>(this->page + DATA_OBSERVER_TARGET, 0);
    // A frame of the game may still be in the stub, writing them once more
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto services = this->alive_view_services;
    auto field = this->alive_view_pawn + offsets::pawn::m_pObserverServices;
    if (this->own_services) {
        // Ours out again, only from a pawn still in the game: a gone one is freed memory. Ours stays allocated, the
        // game may still hold it this frame
        if (HandleOf(this->alive_view_pawn) && p->read<uintptr_t>(field) == this->own_services)
            p->write<uintptr_t>(field, 0);
        this->own_services = 0;
    }
    else if (alive && pawn == this->alive_view_pawn && p->read<uintptr_t>(field) == services) {
        // Its own: what they had back, the server sends it only when it changes. Kept, the game takes our keys as a
        // spectator's up to the next death. Dead, the server already set its own
        p->write<uint8_t>(services + offsets::observerServices::m_iObserverMode, this->alive_view_mode);
        p->write<uint32_t>(services + offsets::observerServices::m_hObserverTarget, this->alive_view_target);
    }

    PatchBinds(false);
    this->alive_view_services = 0;
    this->alive_view_pawn = 0;
    LOGF(VERBOSE, "The game no longer spectates with us alive");
}

Vec3_t Freecam::SmoothAngles(const Vec3_t& raw) {
    auto now = std::chrono::steady_clock::now();

    // Another player starts over
    if (this->target != this->smooth_target) {
        this->smooth_target = this->target;
        this->angles_from = this->angles_to = raw;
        this->time_from = this->time_to = now;
    }

    // A new tick from the server: from where we show now toward it
    if (raw != this->angles_to) {
        auto shown = this->angles_shown;
        float since = std::chrono::duration<float>(now - this->time_to).count();
        this->tick = std::clamp(since, MIN_TICK, MAX_TICK);
        this->angles_from = shown;
        this->angles_to = raw;
        this->time_from = now;
        this->time_to = now;
    }

    // Through the length of a tick, like the game moves players between the ones it gets
    float t = std::clamp(std::chrono::duration<float>(now - this->time_from).count() / this->tick, 0.f, 1.f);
    float yaw = std::remainder(this->angles_to.y - this->angles_from.y, 360.f);   // The short way around
    this->angles_shown = Vec3_t(
        this->angles_from.x + (this->angles_to.x - this->angles_from.x) * t,
        std::remainder(this->angles_from.y + yaw * t, 360.f),
        0.f);
    return this->angles_shown;
}

void Freecam::Shutdown() {
    auto& i = GetInstance();
    auto p = Engine::GetProcess();

    i.stopping = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!i.installed || !p)
        return;

    i.EndDead();
    i.alive_spectate = false;
    p->write<uint32_t>(i.page + DATA_HOLD_ON, 0);
    if (i.alive_view_services) {
        auto pawn = Engine::GetLocalPawn();
        i.EndAliveView(pawn, pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) > 0);
    }
    if (i.hidden_body) {
        i.hidden_body = 0;
        MaterialChams::SetHiddenPlayer(0);
    }
    p->write<uint32_t>(i.page + DATA_MODE, static_cast<uint32_t>(Mode::Off));
    p->write<uint32_t>(i.page + DATA_PUNCH_ON, 0);
    i.mode = Mode::Off;
    i.blocking = false;
    i.BlockGameInput(Input::None);

    if (p->read<uintptr_t>(i.object) == i.copy)
        p->write<uintptr_t>(i.object, i.vtable);

    // The page stays, the game might still be inside the stub
    i.installed = false;
}

Vec3_t Freecam::CameraAngles() {
    auto p = Engine::GetProcess();

    if (this->mode == Mode::Free)
        return this->free_angles;
    if (this->mode == Mode::Follow)
        return this->orbiting ? this->orbit_angles : this->angles_shown;

    // Dead the view of the input, alive what the game showed last
    auto input = offsets::input::dwCSGOInput ? p->read<uintptr_t>(Engine::GetClient().base + offsets::input::dwCSGOInput) : 0;
    return input ? p->read<Vec3_t>(input + offsets::input::m_angViewAngles) : p->read<Vec3_t>(this->page + DATA_VIEW);
}

Vec3_t Freecam::CameraPosition() {
    auto p = Engine::GetProcess();

    if (this->mode == Mode::Free)
        return this->position;

    // Following: their eyes plus our offset, like the stub
    if (this->mode == Mode::Follow && this->target) {
        auto node = p->read<uintptr_t>(this->target + offsets::pawn::m_pGameSceneNode);
        if (node)
            return p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) + p->read<Vec3_t>(this->target + offsets::pawn::m_vecViewOffset)
                + p->read<Vec3_t>(this->page + DATA_ORIGIN);
    }

    // Our own pawn, while dead the spectator one
    auto node = p->read<uintptr_t>(this->local_pawn + offsets::pawn::m_pGameSceneNode);
    return node ? p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) + p->read<Vec3_t>(this->local_pawn + offsets::pawn::m_vecViewOffset) : Vec3_t{};
}

void Freecam::StartFree(const Vec3_t& from, const Vec3_t& look) {
    auto p = Engine::GetProcess();
    this->position = from;
    this->free_angles = Vec3_t(std::clamp(look.x, -MAX_PITCH, MAX_PITCH), look.y, 0.f);
    this->mouse_dx = this->mouse_dy = 0;

    p->write<Vec3_t>(this->page + DATA_ORIGIN, this->position);
    p->write<Vec3_t>(this->page + DATA_FREE_ANGLES, this->free_angles);
    p->write<uintptr_t>(this->page + DATA_INPUT, this->page + DATA_FREE_ANGLES);
    SetMode(Mode::Free);
}

void Freecam::PatchObserverView(bool patched) {
    if (!offsets::camera::observerViewJump || this->observer_patched == patched)
        return;

    auto p = Engine::GetProcess();
    auto address = Engine::GetClient().base + offsets::camera::observerViewJump;

    // je -> jmp, same jump offset, the spectator camera never moves the view
    uint8_t opcode = patched ? JMP_SHORT : JE_SHORT;

    if (p->patch_code(address, &opcode, sizeof(opcode)))
        this->observer_patched = patched;
    else
        LOGF(WARNING, "Failed to {} the spectator camera call ({})", patched ? "patch" : "restore", GetLastError());
}

uint32_t Freecam::HandleOf(uintptr_t entity) {
    auto p = Engine::GetProcess();

    // CEntityIdentity, its handle right after the instance & the class
    auto identity = p->read<uintptr_t>(entity + 0x10);
    auto handle = identity ? p->read<uint32_t>(identity + 0x10) : 0;
    return handle && Engine::GetEntityFromHandle(handle) == entity ? handle : 0;
}

void Freecam::StartDead(uintptr_t pawn) {
    auto p = Engine::GetProcess();

    this->dead_camera = true;
    this->first_person = true;      // Like the game starts, through their eyes
    this->distance = cfg::view::spectate_distance;
    this->observer_services = 0;
    this->left_handled = this->left_clicks;     // Clicks from before do not switch
    this->right_handled = this->right_clicks;

    // Straight to the next player, no flight across the map. Written like typing it in the console, put back after
    if (!this->interp_convar)
        this->interp_convar = View::FindConVar("cl_obs_interp_enable");
    if (this->interp_convar) {
        this->interp_value = p->read<uint8_t>(this->interp_convar + View::CONVAR_VALUE);
        p->write<uint8_t>(this->interp_convar + View::CONVAR_VALUE, 0);
        this->interp_changed = true;
    }

    // The game sends no spectator command of its own while we choose
    PatchBinds(true);

    // The one the game shows, else the first one
    this->target = 0;
    if (auto services = p->read<uintptr_t>(pawn + offsets::pawn::m_pObserverServices)) {
        auto shown = Engine::GetEntityFromHandle(p->read<uint32_t>(services + offsets::observerServices::m_hObserverTarget));
        if (shown && p->read<int>(shown + offsets::pawn::m_iHealth) > 0) {
            this->target = shown;
            p->write<uintptr_t>(this->page + DATA_TARGET, shown);
        }
    }

    if (this->target || PickTarget(0))
        SetMode(Mode::Follow);
    else
        StartFree(CameraPosition(), CameraAngles());
}

void Freecam::EndDead() {
    auto p = Engine::GetProcess();
    if (!this->dead_camera)
        return;

    // The stub first, it writes into the services we put back
    p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, 0);
    p->write<uint32_t>(this->page + DATA_POSITION_ONLY, 0);
    PatchObserverCall(false);
    this->dead_camera = false;
    PatchObserverView(false);
    PatchBinds(false);

    if (this->interp_changed) {
        p->write<uint8_t>(this->interp_convar + View::CONVAR_VALUE, this->interp_value);
        this->interp_changed = false;
    }
    p->write<uintptr_t>(this->page + DATA_INPUT, 0);

    // What the game had, the server sends it again only when it changes. Only while still dead with these services
    // (turned off, the match ended): alive again the server already set its own (no spectating), the old mode
    // written over it made the game take our keys as a spectator's, up to the next death
    auto pawn = Engine::GetLocalPawn();
    bool still_dead = pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0
        && p->read<uintptr_t>(pawn + offsets::pawn::m_pObserverServices) == this->observer_services;
    if (this->observer_services && still_dead) {
        p->write<uint8_t>(this->observer_services + offsets::observerServices::m_iObserverMode, this->observer_mode);
        p->write<uint32_t>(this->observer_services + offsets::observerServices::m_hObserverTarget, this->observer_target);
    }
    else if (this->observer_services && pawn && p->read<uintptr_t>(pawn + offsets::pawn::m_pObserverServices) == this->observer_services) {
        // Alive in the same pawn: our stub may have written once more after the server said no spectating
        p->write<uint8_t>(this->observer_services + offsets::observerServices::m_iObserverMode, 0);
    }
    this->observer_services = 0;

    SetMode(Mode::Off);
}

void Freecam::UpdateDead(uintptr_t pawn, bool playing, float seconds) {
    auto p = Engine::GetProcess();

    // Clicks switch the player like the game, space goes first person, third person, free cam
    uint32_t left_clicks = this->left_clicks, right_clicks = this->right_clicks;
    bool left_pressed = left_clicks != this->left_handled, right_pressed = right_clicks != this->right_handled;
    this->left_handled = left_clicks;
    this->right_handled = right_clicks;

    if (playing) {
        if (left_pressed || right_pressed) {
            if (PickTarget(left_pressed ? 1 : -1))
                SetMode(Mode::Follow);
        }

        if (this->blocking && Pressed(KEY_UP)) {
            if (this->mode == Mode::Follow && this->first_person)
                this->first_person = false;
            else if (this->mode == Mode::Follow)
                StartFree(CameraPosition(), CameraAngles());
            else if (PickTarget(0)) {
                this->first_person = true;
                SetMode(Mode::Follow);
            }
        }
    }

    switch (this->mode) {
    case Mode::Free:    UpdateFree(seconds);    break;
    case Mode::Follow:  UpdateFollow(seconds);  break;
    default:                                    break;
    }

    // First person: our player written into our spectator services like the server does for a teammate, so the game
    // draws the weapon in their hands & their HUD. Third person & free cam: the game following in third person so it
    // draws no weapon in front of our camera.
    // Where the camera is stays ours in all of them: the spectator camera of the game smooths toward where the server
    // keeps our spectator pawn, at the teammate it makes us watch while one lives, & jumped back & forth
    uint32_t handle = this->mode == Mode::Follow && this->first_person && this->target ? HandleOf(this->target) : 0;
    bool game_view = handle != 0;

    // First person: the spectator camera of the game turns the view, with their aim punch & the weapon in their
    // hands moving with it, only where it is comes from us (STUB_OBSERVER). Without that call, all of it is ours
    // Right after a switch the spectator camera of the game still settles on the new player & shook: ours alone
    // until then
    auto now = std::chrono::steady_clock::now();
    if (this->target != this->settled_target) {
        this->settled_target = this->target;
        this->settle_until = now + SWITCH_SETTLE;
    }

    bool position_only = game_view && this->observer_view && now >= this->settle_until;
    PatchObserverCall(position_only);
    p->write<uint32_t>(this->page + DATA_POSITION_ONLY, position_only ? 1 : 0);
    p->write<uint32_t>(this->page + DATA_MODE, position_only ? static_cast<uint32_t>(Mode::Off) : static_cast<uint32_t>(this->mode.load()));
    PatchObserverView(!position_only && this->mode != Mode::Off);

    auto services = p->read<uintptr_t>(pawn + offsets::pawn::m_pObserverServices);
    if (!services) {
        p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, 0);
        return;
    }

    auto mode = p->read<uint8_t>(services + offsets::observerServices::m_iObserverMode);
    auto shown = p->read<uint32_t>(services + offsets::observerServices::m_hObserverTarget);

    // What the game had, to put back
    if (services != this->observer_services) {
        this->observer_services = services;
        this->observer_mode = mode;
        this->observer_target = shown;
    }

    // Not spectating at all (the round is over, the server shows a fixed camera): leave it
    if (mode == 0 && p->read<uint8_t>(this->page + DATA_OBSERVER_MODE) == 0) {
        p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, 0);
        return;
    }

    // Each frame by the stub, right before the spectator camera reads it: whatever the server sends in between
    // never shows. Its target last, the stub reads the pawn first
    uint8_t wanted_mode = game_view ? OBSERVER_FIRST : OBSERVER_THIRD;
    p->write<uint8_t>(this->page + DATA_OBSERVER_MODE, wanted_mode);
    p->write<uint32_t>(this->page + DATA_OBSERVER_TARGET, game_view ? handle : 0);
    p->write<uintptr_t>(this->page + DATA_OBSERVER_PAWN, offsets::camera::getLocalPawn ? pawn : 0);

    // Also right away, for what reads them outside of the view
    if (mode != wanted_mode)
        p->write<uint8_t>(services + offsets::observerServices::m_iObserverMode, wanted_mode);
    if (game_view && shown != handle)
        p->write<uint32_t>(services + offsets::observerServices::m_hObserverTarget, handle);
}

LRESULT CALLBACK Freecam::MouseHook(int code, WPARAM wparam, LPARAM lparam) {
    auto& i = GetInstance();

    auto info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    if (code == HC_ACTION && !(info->flags & LLMHF_INJECTED)) {
        bool free = i.mode == Mode::Free && i.blocking;
        bool taken = (i.dead_camera && i.blocking) || i.alive_mouse;
        bool orbit = i.orbiting && i.blocking;     // Dead in third person, around the one followed
        bool lent = i.lent_mouse;                  // The emote wheel
        bool emote = i.emote_mouse;                // Around us while an emote plays, its clicks still seen (they end it)

        switch (wparam) {
        case WM_MOUSEMOVE:
            // Where it would go from where it is: the cursor stays, the game never sees it
            if (free || orbit || lent || emote) {
                POINT cursor{};
                GetCursorPos(&cursor);
                (lent ? i.lent_dx : i.mouse_dx) += info->pt.x - cursor.x;
                (lent ? i.lent_dy : i.mouse_dy) += info->pt.y - cursor.y;
                return 1;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (lent && wparam == WM_MOUSEWHEEL)
                i.lent_wheel += static_cast<short>(HIWORD(info->mouseData)) / WHEEL_DELTA;
            // Bound to jumping or the weapons, dead it switches the player on the server
            if (free || taken || lent)
                return 1;
            break;
        case WM_LBUTTONDOWN:
            if (taken || free || lent) {
                if (taken)
                    i.left_clicks++;
                if (lent)
                    i.lent_left++;
                i.left_held_back = true;
                return 1;
            }
            break;
        case WM_LBUTTONUP:
            if (i.left_held_back) {
                i.left_held_back = false;
                return 1;
            }
            break;
        case WM_RBUTTONDOWN:
            if (taken || free || lent) {
                if (taken)
                    i.right_clicks++;
                if (lent)
                    i.lent_right++;
                i.right_held_back = true;
                return 1;
            }
            break;
        case WM_RBUTTONUP:
            if (i.right_held_back) {
                i.right_held_back = false;
                return 1;
            }
            break;
        }
    }

    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void Freecam::MouseThread() {
    // Low level hook, called through the message loop of this thread
    if (!SetWindowsHookExA(WH_MOUSE_LL, &Freecam::MouseHook, GetModuleHandleA(nullptr), 0)) {
        LOGF(WARNING, "Failed to install the mouse hook ({}), clicks while dead go to the game too", GetLastError());
        return;
    }

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

bool Freecam::PatchCall(uintptr_t call, uintptr_t target) {
    auto p = Engine::GetProcess();
    auto rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(call + 5));
    return p->patch_code(call + 1, &rel, sizeof(rel));
}

void Freecam::PatchObserverCall(bool patched) {
    if (!this->observer_view || this->observer_call_patched == patched)
        return;

    auto call = Engine::GetClient().base + offsets::camera::observerViewCall;
    if (PatchCall(call, patched ? this->page + STUB_OBSERVER : this->observer_view))
        this->observer_call_patched = patched;
    else
        LOGF(WARNING, "Failed to {} the spectator camera call ({})", patched ? "patch" : "restore", GetLastError());
}

void Freecam::PatchBinds(bool patched) {
    if (!offsets::camera::spectatorBinds || this->binds_patched == patched)
        return;

    auto p = Engine::GetProcess();
    auto address = Engine::GetClient().base + offsets::camera::spectatorBinds;

    uint8_t bytes[sizeof(BINDS_ORIGINAL)];
    std::copy(std::begin(BINDS_ORIGINAL), std::end(BINDS_ORIGINAL), bytes);
    if (patched && this->binds_filter) {
        auto rel = static_cast<int32_t>(static_cast<int64_t>(this->page + STUB_BINDS) - static_cast<int64_t>(address + 5));
        bytes[0] = 0xE9;
        std::memcpy(&bytes[1], &rel, sizeof(rel));
        bytes[5] = 0x90;
    }
    else if (patched) {
        std::copy(std::begin(BINDS_PATCHED), std::end(BINDS_PATCHED), bytes);
    }

    if (p->patch_code(address, bytes, sizeof(bytes)))
        this->binds_patched = patched;
    else
        LOGF(WARNING, "Failed to {} the spectator keys ({})", patched ? "patch" : "restore", GetLastError());
}
