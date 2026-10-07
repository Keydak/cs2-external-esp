#include "Engine.hpp"

#include "core/offsets/Dumper.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"
#include "core/features/Skins.hpp"
#include "core/features/GameRadar.hpp"
#include "core/features/AutoAccept.hpp"
#include "core/features/ClanTag.hpp"
#include "core/features/VoteEvents.hpp"
#include "core/features/HitEffects.hpp"
#include "core/features/Sounds.hpp"
#include "core/features/Subtick.hpp"
#include "core/features/Visuals.hpp"
#include "core/features/Freecam.hpp"
#include "core/engine/GameThread.hpp"

#include <algorithm>
#include <cwctype>

bool Engine::Init() {
    return GetInstance().InitImpl();
}

ProcessModule Engine::GetClient() {
    return GetInstance().client;
}

ProcessModule Engine::GetEngine() {
    return GetInstance().engine;
}

std::shared_ptr<pProcess> Engine::GetProcess() {
    return GetInstance().process;
}

bool Engine::IsInsecure() {
    auto& i = GetInstance();
    return i.insecure && !i.outdated;
}

void Engine::ReportOutdated(const std::string& what) {
    auto& i = GetInstance();
    {
        std::lock_guard<std::mutex> lock(i.outdated_mtx);
        if (i.outdated)
            return;
        i.outdated_reason = what;
    }
    i.outdated = true;
    LOGF(VERBOSE, "Outdated for this CS2 build ({}), stopping everything", what);
}

bool Engine::IsOutdated() {
    return GetInstance().outdated;
}

std::string Engine::GetOutdated() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.outdated_mtx);
    return i.outdated_reason;
}

void Engine::CheckSanity() {
    // Reading wrong is as outdated as writing wrong: checked with or without -insecure
    if (this->outdated || !this->process)
        return;

    auto p = this->process;

    // Only in a match, with our controller there
    auto controller = p->read<uintptr_t>(this->client.base + offsets::localPlayerController);
    if (!controller) {
        this->failed_checks = 0;
        return;
    }

    std::string problem;

    // Our pawn, when alive: health & team in their ranges
    if (auto pawn = GetEntityFromHandle(p->read<uint32_t>(controller + offsets::controller::m_hPawn))) {
        auto health = p->read<int32_t>(pawn + offsets::pawn::m_iHealth);
        auto team = p->read<uint8_t>(pawn + offsets::pawn::m_iTeamNum);
        if (health < 0 || health > 1000)
            problem = std::format("health reads {}", health);
        else if (team > 3)
            problem = std::format("team reads {}", team);
    }

    // The game rules, by the class name in their RTTI
    if (problem.empty()) {
        auto rules = p->read<uintptr_t>(this->client.base + offsets::rules::dwGameRules);
        auto vtable = rules ? p->read<uintptr_t>(rules) : 0;
        auto locator = vtable ? p->read<uintptr_t>(vtable - 8) : 0;

        char name[64]{};
        if (locator) {
            auto module = locator - p->read<uint32_t>(locator + 0x14);
            p->read_raw(module + p->read<uint32_t>(locator + 0xC) + 0x10, name, sizeof(name) - 1);
        }
        if (!strstr(name, "GameRules"))
            problem = "the game rules are not where they should be";
    }

    if (problem.empty()) {
        this->failed_checks = 0;
        return;
    }

    // Loading screens can read odd for a moment, wrong for 5 seconds in a row is an update
    if (++this->failed_checks == 1)
        LOGF(VERBOSE, "Offset check failed: {}", problem);
    if (this->failed_checks >= 5)
        ReportOutdated(problem);
}

uintptr_t Engine::GetLocalPawn() {
    auto p = GetProcess();
    auto client = GetClient();

    if (!p)
        return 0;

    auto controller = p->read<uintptr_t>(client.base + offsets::localPlayerController);
    if (!controller)
        return 0;

    return GetEntityFromHandle(p->read<uint32_t>(controller + offsets::controller::m_hPawn));
}

uintptr_t Engine::GetEntityFromHandle(uint32_t handle) {
    auto p = GetProcess();
    auto client = GetClient();

    if (!p || !handle || handle == 0xFFFFFFFF)
        return 0;

    // Similar to Player::GetPawn()
    auto entity_list = p->read<uintptr_t>(client.base + offsets::entityList);
    if (!entity_list)
        return 0;

    auto list_entry = p->read<uintptr_t>(entity_list + 0x10 + 0x8 * ((handle & 0x7FFF) >> 9));
    if (!list_entry)
        return 0;

    return p->read<uintptr_t>(list_entry + 0x70 * (handle & 0x1FF));
}

bool Engine::ForceFullUpdate() {
    auto p = GetProcess();
    auto engine = GetEngine();

    if (!p)
        return false;

    auto network_client = p->read<uintptr_t>(engine.base + offsets::network::dwNetworkGameClient);
    if (!network_client) {
        LOGF(WARNING, "Could not find the network client to request a full update");
        return false;
    }

    p->write<int>(network_client + offsets::network::deltaTick, -1);
    LOGF(VERBOSE, "Requested a full update");
    return true;
}

bool Engine::InitImpl() {
    process = std::make_shared<pProcess>();

    if (!this->AwaitProcess()) {
        LOGF(FATAL, "Could not find process, please make sure the game is open");
        return false;
    }

    if (!this->AwaitModules()) {
        LOGF(FATAL, "Game took too long to load, please open me again once its fully loaded");
        return false;
    }

    auto command_line = process->ReadCommandLine();
    std::transform(command_line.begin(), command_line.end(), command_line.begin(), std::towlower);
    this->insecure = command_line.find(L"-insecure") != std::wstring::npos;

    if (this->insecure)
        LOGF(INFO, "Game launched with -insecure, memory writing features are available");
    else
        LOGF(WARNING, "Game was not launched with -insecure, memory writing features are disabled");

    if (!Dumper::FetchRemote())
        LOGF(WARNING, "Could not fetch latest offsets, using built-in offsets (they might be outdated)");

    // Everything has to be found in this build: nothing runs on offsets of the one before
    if (!Dumper::Init() || !Dumper::Unverified().empty()) {
#ifdef _DEBUG
        std::string list;
        for (const auto& what : Dumper::Unverified())
            list += "\n    - " + what;
        LOGF(FATAL, "Outdated for the running CS2, nothing was started. Not found in this build:{}", list);
#else
        LOGF(FATAL, "CS2 was updated, this program has to be updated too before it can be used again");
#endif
        return false;
    }

    if (!Config::Read())
        LOGF(WARNING, "Failed to parse config, using default values");

    // The configs marked as default in the menu, over the last settings
    Config::LoadDefaultPresets();

#ifdef _DEBUG
    if (!cfg::dev::console)
        LogHelper::Free();
#endif

    std::thread(&Engine::Thread, this).detach();

    Movement::Init();
    View::Init();
    GameThread::Init();
    Skins::Init();
    GameRadar::Init();
    AutoAccept::Init();
    ClanTag::Init();
    VoteEvents::Init();
    Sounds::Init();
    HitEffects::Init();
    Subtick::Init();
    Visuals::Init();
    Freecam::Init();

    LOGF(INFO, "Successfully initialized engine...");
    return true;
}

void Engine::Thread() {
    // TODO: Check build number 
    // uintptr_t number = process->read<uintptr_t>(base_engine.base + offsets::buildNumber);

    auto next_check = steady_clock::now();

    while (true) {
        auto start = steady_clock::now();

        Cache::Refresh();

        if (start >= next_check) {
            next_check = start + 1s;
            CheckSanity();
        }

        if (cfg::settings::free_cpu)
            std::this_thread::sleep_until(start + 1ms);
    }
}

bool Engine::AwaitProcess() {
    if (!process || process->handle_) // Process not initialized, or already attached
        return false;

    constexpr int WAIT_SECONDS = 50;
    LogHelper::Status("Checking whether CS2 is open...");

    for (int waited = 0; ; waited++) {
        if (process->AttachProcess("cs2.exe"))
            break;

        if (process->pid_ && !process->handle_) {
            LogHelper::Status("");
            LOGF(FATAL, "Insufficient permissions to open a handle to the process. Try running as Administrator.");
            return false;
        }

        // Not opened in time: nothing to do, closes by itself
        if (waited >= WAIT_SECONDS) {
            LogHelper::Status(std::format("CS2 was not opened within {} seconds, closing...", WAIT_SECONDS));
            std::this_thread::sleep_for(3s);
            LogHelper::Destroy();
            ExitProcess(0);
        }

        LogHelper::Status(std::format("CS2 is not open yet, waiting for it... {}s", WAIT_SECONDS - waited));
        std::this_thread::sleep_for(1s);
    }

    // Found: the console starts over with the name & the credits
    LogHelper::Clear();
    LogHelper::Banner();
    return true;
}

bool Engine::AwaitModules() {
    if (!process || !process->handle_) // Process not initialized, or not attached
        return false;

    LOGF(INFO, "Waiting for the game to open...");

    do {
        this->client = process->GetModule("client.dll");
        this->engine = process->GetModule("engine2.dll");

        if (this->client.base && this->engine.base)
            break;

        static int attempts = 0;
        if (attempts > 10)
            return false;
        attempts++;

        std::this_thread::sleep_for(5s);
    } while (true);

    return true;
}
