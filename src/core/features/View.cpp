#include "View.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"
#include "core/features/Movement.hpp"
#include "core/features/Visuals.hpp"
#include "core/features/Freecam.hpp"

#include <bit>

bool View::Init() {
    return GetInstance().InitImpl();
}

bool View::IsAvailable() {
    return Engine::IsInsecure();
}

bool View::InitImpl() {
    if (!Engine::IsInsecure())
        return false;

    std::thread(&View::Thread, this).detach();

    LOGF(INFO, "Successfully initialized view...");
    return true;
}

namespace {
    // CS2 locks everyone to this fov, so writing it back is the same as the game default
    constexpr uint32_t DEFAULT_FOV = 90;

    constexpr int THIRD_PERSON_TOGGLE = 0;
    constexpr int THIRD_PERSON_HOLD = 1;
    constexpr int THIRD_PERSON_ALWAYS = 2;

    constexpr float THIRD_PERSON_START_DISTANCE = 30.f; // Same as the game, it eases out from there

    constexpr uint8_t JNE_SHORT = 0x75;
    constexpr uint8_t JMP_SHORT = 0xEB;

    // Console variables: CreateInterface("VEngineCvar007") of tier0.dll, a list of (ConVarData*, links) entries
    constexpr char CVAR_INTERFACE[] = "VEngineCvar007";
    constexpr size_t CVAR_LIST = 0x50;          // Entries
    constexpr size_t CVAR_COUNT = 0x48;         // uint16, entries in use
    constexpr size_t CVAR_ENTRY_SIZE = 0x10;
    constexpr size_t CVAR_MAX_ENTRIES = 0x4000;

    // ConVarData: name first, then the float value & its limits
    constexpr size_t CONVAR_MIN = 0x60;
    constexpr size_t CONVAR_MAX = 0x68;

    constexpr auto VIEWMODEL_SEARCH_RETRY = std::chrono::seconds(5);

    // Wider than the game allows, its limits are widened to these while the override is on
    constexpr float VIEWMODEL_FOV_MIN = 40.f, VIEWMODEL_FOV_MAX = 120.f;
    constexpr float VIEWMODEL_OFFSET_LIMIT = 20.f;
}

bool View::IsViewmodelAvailable() {
    return Engine::IsInsecure() && GetInstance().viewmodel_found;
}

bool View::IsThirdPersonAvailable() {
    return Engine::IsInsecure() && offsets::input::dwCSGOInput != 0;
}

void View::Thread() {
    while (!this->stopping) {
        auto p = Engine::GetProcess();
        auto pawn = Engine::GetLocalPawn();

        bool alive = p && pawn && p->read<int>(pawn + offsets::pawn::m_iHealth) > 0;

        if (alive)
            UpdateFov(pawn);

        UpdateThirdPerson(alive ? pawn : 0);
        UpdateViewmodel();

        std::this_thread::sleep_for(10ms);
    }
}

void View::UpdateFov(uintptr_t pawn) {
    auto p = Engine::GetProcess();

    // A new pawn comes with a fresh camera
    if (pawn != this->last_pawn) {
        this->last_pawn = pawn;
        this->fov_applied = false;
    }

    // Scoped weapons zoom through the same fov, leave it to the game. Also while we hide the scope
    if (Visuals::IsZoomed(pawn))
        return;

    auto camera = p->read<uintptr_t>(pawn + offsets::view::m_pCameraServices);
    if (!camera)
        return;

    auto current = p->read<uint32_t>(camera + offsets::view::m_iFOV);

    if (cfg::view::fov_enabled) {
        auto fov = static_cast<uint32_t>(cfg::view::fov);

        if (current != fov)
            p->write<uint32_t>(camera + offsets::view::m_iFOV, fov);

        this->fov_applied = true;
        return;
    }

    // Turned off: back to the default the same way, the game keeps our last value otherwise.
    // Done once it holds its own value again (0 means default)
    if (!this->fov_applied)
        return;

    if (current == 0 || current == DEFAULT_FOV)
        this->fov_applied = current != 0;
    else
        p->write<uint32_t>(camera + offsets::view::m_iFOV, DEFAULT_FOV);
}

bool View::IsThirdPersonOn() {
    return GetInstance().third_person_applied;
}

void View::UpdateThirdPerson(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    if (!p || !offsets::input::dwCSGOInput)
        return;

    auto input = p->read<uintptr_t>(Engine::GetClient().base + offsets::input::dwCSGOInput);
    if (!input)
        return;

    // Key, only while playing
    bool key_down = Movement::IsKeyUsable(cfg::view::third_person_key) && (GetAsyncKeyState(cfg::view::third_person_key) & 0x8000);

    if (key_down && !this->third_person_key_was_down)
        this->third_person_toggled = !this->third_person_toggled;
    this->third_person_key_was_down = key_down;

    bool wanted = false;
    switch (cfg::view::third_person_mode) {
    case THIRD_PERSON_TOGGLE:   wanted = this->third_person_toggled;    break;
    case THIRD_PERSON_HOLD:     wanted = key_down;                      break;
    case THIRD_PERSON_ALWAYS:   wanted = true;                          break;
    }

    wanted = wanted && cfg::view::third_person && pawn;

    if (wanted && cfg::view::third_person_scoped_off && Visuals::IsZoomed(pawn))
        wanted = false;

    // The camera away from us shows our player, without the weapon in our hands
    if (pawn && Freecam::GetMode() != Freecam::Mode::Off)
        wanted = true;

    if (!wanted) {
        // Back to first person, like the "firstperson" command does
        if (this->third_person_applied) {
            this->third_person_applied = false;
            p->write<bool>(input + offsets::input::m_bInThirdPerson, false);
        }

        PatchCheatsCheck(false);
        return;
    }

    // Without sv_cheats the camera code turns third person off every frame
    PatchCheatsCheck(true);

    // Same as the "thirdperson" command: the camera starts close behind us,
    // the game then moves it every frame, writing it ourselves would fight with that
    if (!p->read<bool>(input + offsets::input::m_bInThirdPerson)) {
        auto angles = p->read<Vec3_t>(input + offsets::input::m_angViewAngles);

        p->write<Vec3_t>(input + offsets::input::m_vecCameraOffset, Vec3_t(angles.x, angles.y, THIRD_PERSON_START_DISTANCE));
        p->write<bool>(input + offsets::input::m_bInThirdPerson, true);
    }
    this->third_person_applied = true;
}

void View::PatchCheatsCheck(bool patched) {
    if (!offsets::input::cheatsCheckJump || this->cheats_check_patched == patched)
        return;

    auto p = Engine::GetProcess();
    auto address = Engine::GetClient().base + offsets::input::cheatsCheckJump;

    // jne -> jmp, same jump offset, so the check never turns third person off
    uint8_t opcode = patched ? JMP_SHORT : JNE_SHORT;

    if (p->patch_code(address, &opcode, sizeof(opcode)))
        this->cheats_check_patched = patched;
    else
        LOGF(WARNING, "Failed to {} the third person sv_cheats check ({})", patched ? "patch" : "restore", GetLastError());
}

void View::Shutdown() {
    auto& view = GetInstance();
    auto p = Engine::GetProcess();

    // Stop the thread first, it would apply everything again
    view.stopping = true;
    std::this_thread::sleep_for(50ms);

    // Leave the game the way we found it
    if (view.third_person_applied && p && offsets::input::dwCSGOInput) {
        if (auto input = p->read<uintptr_t>(Engine::GetClient().base + offsets::input::dwCSGOInput))
            p->write<bool>(input + offsets::input::m_bInThirdPerson, false);
    }

    view.third_person_applied = false;
    view.PatchCheatsCheck(false);
    view.RestoreViewmodel();
}

uintptr_t View::FindConVarList(uintptr_t& count) {
    auto p = Engine::GetProcess();

    auto cvar = p->FindInterface("tier0.dll", CVAR_INTERFACE);
    if (!cvar)
        return 0;

    count = std::min<uintptr_t>(p->read<uint16_t>(cvar + CVAR_COUNT), CVAR_MAX_ENTRIES);
    return p->read<uintptr_t>(cvar + CVAR_LIST);
}

uintptr_t View::FindConVar(std::string_view name) {
    auto p = Engine::GetProcess();

    uintptr_t count = 0;
    auto list = GetInstance().FindConVarList(count);
    if (!p || !list || !count || name.size() >= 64)
        return 0;

    std::vector<uint8_t> entries(count * CVAR_ENTRY_SIZE);
    if (!p->read_raw(list, entries.data(), entries.size()))
        return 0;

    for (uintptr_t i = 0; i < count; i++) {
        auto data = *reinterpret_cast<uintptr_t*>(&entries[i * CVAR_ENTRY_SIZE]);
        if (!data)
            continue;

        char buffer[64]{};
        p->read_raw(p->read<uintptr_t>(data), buffer, name.size() + 1);
        if (std::string_view(buffer) == name)
            return data;
    }
    return 0;
}

bool View::FindViewmodelVars() {
    auto p = Engine::GetProcess();

    uintptr_t count = 0;
    auto list = FindConVarList(count);
    if (!list || !count)
        return false;

    // The whole list in one read, the names one by one
    std::vector<uint8_t> entries(count * CVAR_ENTRY_SIZE);
    if (!p->read_raw(list, entries.data(), entries.size()))
        return false;

    int found = 0;
    for (uintptr_t i = 0; i < count && found < 4; i++) {
        auto data = *reinterpret_cast<uintptr_t*>(&entries[i * CVAR_ENTRY_SIZE]);
        if (!data)
            continue;

        char name[24]{};
        p->read_raw(p->read<uintptr_t>(data), name, sizeof(name) - 1);
        if (!std::string_view(name).starts_with("viewmodel_"))
            continue;

        for (auto& var : this->viewmodel_vars) {
            if (var.data || std::string_view(name) != var.name)
                continue;

            var.data = data;
            var.min = p->read<float>(data + CONVAR_MIN);
            var.max = p->read<float>(data + CONVAR_MAX);
            found++;
        }
    }

    if (found != 4) {
        for (auto& var : this->viewmodel_vars)
            var.data = 0;
        return false;
    }

    LOGF(INFO, "Found the viewmodel console variables");
    return true;
}

bool View::FindViewmodelClamps() {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();
    if (!client.base || !client.size)
        return false;

    // Float limits as they are in the code
    constexpr uint32_t FOV_MAX = 0x42880000;    // 68
    constexpr uint32_t FOV_MIN = 0x42700000;    // 60
    constexpr uint32_t OFFSET_X_MAX = 0x40200000; // 2.5
    constexpr uint32_t OFFSET_MAX = 0x40000000; // 2
    constexpr uint32_t OFFSET_MIN = 0xC0000000; // -2
    constexpr size_t LOOK_BACK = 0xA0;          // The offsets are clamped right before the fov

    auto as_bits = [](float value) { return std::bit_cast<uint32_t>(value); };

    // An imm32 of "mov dword ptr [rsp + x], imm" (C7 44 24 x) or "[rbp + x]" (C7 45 x)
    auto is_mov_imm = [](const uint8_t* at) { return at[-4] == 0xC7 || at[-3] == 0xC7; };

    // Both functions that place the weapon: "mov [stack], 68.0; mov rcx, ..; mov [stack], 60.0"
    constexpr size_t CHUNK = 0x10000, OVERLAP = 0x100;
    std::vector<uint8_t> buffer(CHUNK + OVERLAP);
    std::vector<ClampPatch> found;

    for (uintptr_t offset = 0x1000; offset < client.size; offset += CHUNK) {
        size_t size = std::min<size_t>(CHUNK + OVERLAP, client.size - offset);
        if (!p->read_raw(client.base + offset, buffer.data(), size))
            continue;

        for (size_t i = LOOK_BACK + 4; i + 16 <= size && i < CHUNK + LOOK_BACK + 4; i++) {
            if (*reinterpret_cast<uint32_t*>(&buffer[i]) != FOV_MAX || !is_mov_imm(&buffer[i]))
                continue;

            // The min a few bytes later, in a mov to the stack as well
            size_t min_at = 0;
            for (size_t j = i + 4; j < i + 16 && j + 4 <= size && !min_at; j++)
                if (*reinterpret_cast<uint32_t*>(&buffer[j]) == FOV_MIN && is_mov_imm(&buffer[j]))
                    min_at = j;

            if (!min_at)
                continue;

            std::vector<ClampPatch> block = {
                { client.base + offset + i, FOV_MAX, as_bits(VIEWMODEL_FOV_MAX) },
                { client.base + offset + min_at, FOV_MIN, as_bits(VIEWMODEL_FOV_MIN) },
            };

            // The offset limits before it, each an imm32 of a mov to the stack
            for (size_t k = i - LOOK_BACK; k < i; k++) {
                auto value = *reinterpret_cast<uint32_t*>(&buffer[k]);
                if (!is_mov_imm(&buffer[k]))
                    continue;

                if (value == OFFSET_X_MAX || value == OFFSET_MAX)
                    block.push_back({ client.base + offset + k, value, as_bits(VIEWMODEL_OFFSET_LIMIT) });
                else if (value == OFFSET_MIN)
                    block.push_back({ client.base + offset + k, value, as_bits(-VIEWMODEL_OFFSET_LIMIT) });
            }

            // x, y & z: a max & a min each
            if (block.size() == 8)
                found.insert(found.end(), block.begin(), block.end());
        }
    }

    if (found.empty()) {
        LOGF(WARNING, "Could not find the viewmodel limits of the game, the viewmodel stays within them");
        return false;
    }

    this->clamp_patches = std::move(found);
    LOGF(INFO, "Found {} viewmodel limits in the game code", this->clamp_patches.size());
    return true;
}

void View::PatchViewmodelClamps(bool patched) {
    if (this->clamps_patched == patched)
        return;

    auto p = Engine::GetProcess();
    if (!p)
        return;

    for (const auto& patch : this->clamp_patches) {
        uint32_t value = patched ? patch.widened : patch.original;
        if (!p->patch_code(patch.address, &value, sizeof(value)))
            LOGF(WARNING, "Failed to {} a viewmodel limit ({})", patched ? "widen" : "restore", GetLastError());
    }

    this->clamps_patched = patched;
}

void View::UpdateViewmodel() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    if (!this->viewmodel_found) {
        if (!cfg::view::viewmodel_enabled || std::chrono::steady_clock::now() < this->viewmodel_next_search)
            return;

        this->viewmodel_found = FindViewmodelVars();
        this->viewmodel_next_search = std::chrono::steady_clock::now() + VIEWMODEL_SEARCH_RETRY;
        if (!this->viewmodel_found)
            return;
    }

    if (!cfg::view::viewmodel_enabled) {
        RestoreViewmodel();
        return;
    }

    if (!this->clamps_searched) {
        this->clamps_searched = true;
        FindViewmodelClamps();
    }
    PatchViewmodelClamps(true);

    const float wanted[4] = { cfg::view::viewmodel_fov, cfg::view::viewmodel_x, cfg::view::viewmodel_y, cfg::view::viewmodel_z };

    for (int i = 0; i < 4; i++) {
        auto& var = this->viewmodel_vars[i];
        float current = p->read<float>(var.data + CONVAR_VALUE);

        // What the player had, before the first change
        if (!this->viewmodel_applied)
            var.original = current;

        // Past the limits of the game: they are widened, the game would clamp it back otherwise
        float low = i == 0 ? VIEWMODEL_FOV_MIN : -VIEWMODEL_OFFSET_LIMIT;
        float high = i == 0 ? VIEWMODEL_FOV_MAX : VIEWMODEL_OFFSET_LIMIT;
        float value = std::clamp(wanted[i], low, high);

        if (p->read<float>(var.data + CONVAR_MIN) != std::min(low, var.min))
            p->write<float>(var.data + CONVAR_MIN, std::min(low, var.min));
        if (p->read<float>(var.data + CONVAR_MAX) != std::max(high, var.max))
            p->write<float>(var.data + CONVAR_MAX, std::max(high, var.max));

        if (current != value)
            p->write<float>(var.data + CONVAR_VALUE, value);
    }

    this->viewmodel_applied = true;
}

void View::RestoreViewmodel() {
    PatchViewmodelClamps(false);

    if (!this->viewmodel_applied)
        return;

    // Value & limits of the game, it saves them to the config of the player
    if (auto p = Engine::GetProcess()) {
        for (const auto& var : this->viewmodel_vars) {
            if (!var.data)
                continue;

            p->write<float>(var.data + CONVAR_VALUE, var.original);
            p->write<float>(var.data + CONVAR_MIN, var.min);
            p->write<float>(var.data + CONVAR_MAX, var.max);
        }
    }

    this->viewmodel_applied = false;
}
