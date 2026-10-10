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
#include "core/features/MaterialChams.hpp"
#include "core/features/AgentPreview.hpp"
#include "core/features/KillEffect.hpp"
#include "core/features/Freecam.hpp"
#include "core/engine/GameThread.hpp"

#include <algorithm>
#include <cwctype>

bool Engine::Attach() {
    return GetInstance().AttachImpl();
}

void Engine::Start() {
    GetInstance().StartImpl();
}

std::string Engine::GetProgress() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.progress_mtx);
    return i.progress;
}

bool Engine::IsWaitingForGame() {
    return GetInstance().waiting_for_game;
}

void Engine::SetProgress(const std::string& text) {
    std::lock_guard<std::mutex> lock(this->progress_mtx);
    this->progress = text;
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

bool Engine::AttachImpl() {
    // Closed again while it loaded: waited for once more
    while (true) {
        process = std::make_shared<pProcess>();

        if (!this->AwaitProcess()) {
            LOGF(FATAL, "Could not find process, please make sure the game is open");
            return false;
        }

        if (this->AwaitModules())
            break;

        DWORD code = 0;
        if (GetExitCodeProcess(process->handle_, &code) && code != STILL_ACTIVE) {
            LOGF(WARNING, "CS2 closed while it loaded, waiting for it again");
            continue;
        }

        SetProgress("The game took too long to load, open this again once it is loaded");
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

    SetProgress("Reading the game");
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
        SetProgress("CS2 was updated, this program has to be updated too");
        return false;
    }

    SetProgress("Loading the config");
    if (!Config::Read())
        LOGF(WARNING, "Failed to parse config, using default values");

    // The configs marked as default in the menu, over the last settings
    Config::LoadDefaultPresets();

#ifdef _DEBUG
    if (!cfg::dev::console)
        LogHelper::Free();
#endif

    SetProgress("Found");
    LOGF(INFO, "Found the game...");
    return true;
}

void Engine::StartImpl() {
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
    MaterialChams::Init();
    AgentPreview::Init();
    KillEffect::Init();
    Freecam::Init();

    LOGF(INFO, "Successfully initialized engine...");
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

    LogHelper::Status("Checking whether CS2 is open...");
    SetProgress("Looking for CS2");

    // As long as it takes, the loader can be closed meanwhile
    this->just_opened = false;
    for (int waited = 0; ; waited++) {
        if (process->AttachProcess("cs2.exe")) {
            this->waiting_for_game = false;
            this->just_opened = waited > 0;
            break;
        }
        this->waiting_for_game = true;

        if (process->pid_ && !process->handle_) {
            this->waiting_for_game = false;
            LogHelper::Status("");
            SetProgress("No access to CS2, run this program as administrator");
            LOGF(FATAL, "Insufficient permissions to open a handle to the process. Try running as Administrator.");
            return false;
        }

        LogHelper::Status(std::format("CS2 is not open yet, waiting for it... {}s", waited));
        SetProgress("Waiting for CS2 to be opened");
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
    SetProgress("Waiting for the game to load");

    // Every module the offsets are read from: a game opened a moment ago loads them one after the other. Up to 3
    // minutes, a slow disk takes a while
    constexpr const char* NEEDED[] = { "soundsystem.dll", "resourcesystem.dll", "scenesystem.dll", "materialsystem2.dll", "panorama.dll" };
    auto deadline = std::chrono::steady_clock::now() + 3min;
    while (true) {
        this->client = process->GetModule("client.dll");
        this->engine = process->GetModule("engine2.dll");

        bool all = this->client.base && this->engine.base;
        for (auto name : NEEDED)
            all = all && process->GetModule(name).base;
        if (all)
            break;

        DWORD code = 0;
        if (std::chrono::steady_clock::now() > deadline || (GetExitCodeProcess(process->handle_, &code) && code != STILL_ACTIVE))
            return false;

        std::this_thread::sleep_for(1s);
    }

    // Just opened: its code is there, what it sets up in it a moment later
    if (this->just_opened) {
        SetProgress("CS2 opened, letting it finish loading");
        std::this_thread::sleep_for(8s);
    }

    return true;
}
