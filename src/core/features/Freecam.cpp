#include "Freecam.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"

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
    constexpr uintptr_t DATA_FREE_ANGLES = 0xB0;        // QAngle, where the free cam looks, moved by the mouse we keep from the game
    constexpr uintptr_t DATA_INPUT = 0x60;      // uint64, QAngle* the free cam looks along: our own (DATA_FREE_ANGLES), turned by
                                                // the mouse we keep from the game. 0 for where we look
    constexpr uintptr_t STUB = 0x100;
    constexpr uintptr_t STUB_OBSERVER = 0x340;
    constexpr uintptr_t TABLE = 0x500;
    constexpr uintptr_t OLD_TABLE = 0x400;              // Of a page from an earlier version
    constexpr size_t PAGE_SIZE = 0x1000;
    constexpr size_t MAX_ENTRIES = (PAGE_SIZE - TABLE) / sizeof(uintptr_t) - 1;
    constexpr uint64_t MAGIC = 0x4D41434545524643; // "CFREECAM"

    constexpr float FAST = 3.f;                 // Shift
    constexpr float DISTANCE_SPEED = 150.f;     // W & S following, units per second
    constexpr float MIN_DISTANCE = 40.f, MAX_DISTANCE = 300.f;

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

    // rcx = { SDL_SetEventEnabled, bool enabled }, calls it for each blocked event
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

bool Freecam::InitImpl() {
    if (!Engine::IsInsecure() || !offsets::camera::dwClientMode || !offsets::camera::overrideView)
        return false;

    if (!Install())
        return false;

    // Also gives the game its input back after a run closed while the free cam was on
    if (SetupGameInput())
        BlockGameInput(false, true);

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

    c.put({ 0x4C, 0x8D, 0x15 }); page_rel();            // lea r10, [page]
    c.put({ 0x41, 0x8B, 0x42, DATA_MODE });             // mov eax, [r10 + mode]
    c.put({ 0x83, 0xF8, 0x01 });                        // cmp eax, free
    auto to_free = c.jump({ 0x0F, 0x84 });              // je free
    c.put({ 0x83, 0xF8, 0x02 });                        // cmp eax, follow
    auto to_follow = c.jump({ 0x0F, 0x84 });            // je follow
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

    c.land(off); c.land(free_done); c.land(input_done); c.land(no_target); c.land(no_node);
    c.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
    c.put({ 0x5B });                                    // pop rbx
    c.put({ 0xC3 });                                    // ret

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

    if (STUB + c.size() > STUB_OBSERVER || STUB_OBSERVER + o.size() > TABLE) {
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
    std::copy(c.bytes.begin(), c.bytes.end(), data.begin() + STUB);
    std::copy(o.bytes.begin(), o.bytes.end(), data.begin() + STUB_OBSERVER);

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

    Code c;
    c.put({ 0x53 });                                    // push rbx
    c.put({ 0x48, 0x83, 0xEC, 0x20 });                  // sub rsp, 0x20
    c.put({ 0x48, 0x89, 0xCB });                        // mov rbx, rcx
    for (uint32_t type : BLOCKED_EVENTS) {
        c.put({ 0xB9 }); c.put32(static_cast<int32_t>(type)); // mov ecx, type
        c.put({ 0x0F, 0xB6, 0x53, 0x08 });              // movzx edx, byte [rbx + 8]
        c.put({ 0xFF, 0x13 });                          // call [rbx]
    }
    c.put({ 0x48, 0x83, 0xC4, 0x20 });                  // add rsp, 0x20
    c.put({ 0x5B });                                    // pop rbx
    c.put({ 0x31, 0xC0 });                              // xor eax, eax
    c.put({ 0xC3 });                                    // ret

    if (c.size() > INPUT_ENABLE)
        return false;

    this->input_code = p->allocate_remote(INPUT_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->input_code)
        return false;

    struct { uint64_t function; uint64_t enabled; } enable{ set_event_enabled, 1 }, disable{ set_event_enabled, 0 };
    p->write_bytes(this->input_code, c.bytes);
    p->write(this->input_code + INPUT_ENABLE, enable);
    p->write(this->input_code + INPUT_DISABLE, disable);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->input_code), INPUT_SIZE);
    return true;
}

void Freecam::BlockGameInput(bool blocked, bool force) {
    if (!this->input_code || (this->input_blocked == blocked && !force))
        return;

    auto p = Engine::GetProcess();
    if (p->call_remote(this->input_code, this->input_code + (blocked ? INPUT_DISABLE : INPUT_ENABLE)))
        this->input_blocked = blocked;
    else
        LOGF(WARNING, "Failed to {} the input of the game ({})", blocked ? "block" : "give back", GetLastError());
}

bool Freecam::OnKey(DWORD vk, bool down) {
    auto& i = GetInstance();

    for (int key = 0; key < KEY_COUNT; key++) {
        for (DWORD same : KEYS[key]) {
            if (same && same == vk) {
                i.held[key] = down;

                // Dead & watching a player only space (the mode) & W S (closer, further) are ours: crouch & the
                // rest go to the game
                if (i.dead_camera && i.mode != Mode::Free)
                    return i.blocking && (key == KEY_UP || key == KEY_FORWARD || key == KEY_BACK);
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
    if (wanted == this->mode)
        return;

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
        EndDead();
        SetMode(Mode::Off);
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
        // Its clicks only while the game hides the cursor: shown, a menu of the game takes them (teams, buying,
        // pause), they go to it
        this->blocking = this->mode != Mode::Off && playing && Movement::IsPlaying();
        // Dead there is no player to move: the keys of the game (use, radio, ...) all stay, ours are taken by
        // the hook alone
        BlockGameInput(false);
        UpdateDead(pawn, playing, seconds);
        WriteFov();
        return;
    }

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

    // Turned off in the menu
    if (this->mode != Mode::Off && !cfg::view::freecam)
        SetMode(Mode::Off);

    this->blocking = this->mode != Mode::Off && playing;
    BlockGameInput(this->blocking && this->mode == Mode::Free);

    switch (this->mode) {
    case Mode::Free:    UpdateFree(seconds);    break;
    case Mode::Follow:  UpdateFollow(seconds);  break;
    default:                                    break;
    }

    WriteFov();
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

    // The mouse, kept from the game by the hook: like the game turns the view with it
    int dx = this->mouse_dx.exchange(0), dy = this->mouse_dy.exchange(0);
    if (dx || dy) {
        if (!this->sensitivity_convar)
            this->sensitivity_convar = View::FindConVar("sensitivity");
        float sensitivity = this->sensitivity_convar ? p->read<float>(this->sensitivity_convar + View::CONVAR_VALUE) : 1.f;
        if (!(sensitivity > 0.f && sensitivity < 100.f))
            sensitivity = 1.f;

        // The hook sees the cursor, moved by the pointer speed of Windows: back to the counts of the mouse the
        // game turns by, then our own multiplier
        sensitivity *= cfg::view::freecam_sensitivity / PointerSpeed();

        this->free_angles.y = std::remainder(this->free_angles.y - dx * sensitivity * MOUSE_DEGREES, 360.f);
        this->free_angles.x = std::clamp(this->free_angles.x + dy * sensitivity * MOUSE_DEGREES, -MAX_PITCH, MAX_PITCH);
        p->write<Vec3_t>(this->page + DATA_FREE_ANGLES, this->free_angles);
    }

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
    p->write<Vec3_t>(this->page + DATA_FOLLOW_ANGLES, angles);

    // Behind them where they look, from their eyes
    Vec3_t offset{};
    if (!this->first_person) {
        auto forward = Forward(angles);
        offset = Vec3_t(-forward.x * this->distance, -forward.y * this->distance, -forward.z * this->distance);
    }
    p->write<Vec3_t>(this->page + DATA_ORIGIN, offset);
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
    p->write<uint32_t>(i.page + DATA_MODE, static_cast<uint32_t>(Mode::Off));
    i.mode = Mode::Off;
    i.blocking = false;
    i.BlockGameInput(false);

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
        return this->angles_shown;

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
        bool taken = i.dead_camera && i.blocking;

        switch (wparam) {
        case WM_MOUSEMOVE:
            // Where it would go from where it is: the cursor stays, the game never sees it
            if (free) {
                POINT cursor{};
                GetCursorPos(&cursor);
                i.mouse_dx += info->pt.x - cursor.x;
                i.mouse_dy += info->pt.y - cursor.y;
                return 1;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // Bound to jumping or the weapons, dead it switches the player on the server
            if (free || taken)
                return 1;
            break;
        case WM_LBUTTONDOWN:
            if (taken || free) {
                if (taken)
                    i.left_clicks++;
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
            if (taken || free) {
                if (taken)
                    i.right_clicks++;
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
