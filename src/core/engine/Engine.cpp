#include "Engine.hpp"

#include "core/offsets/Dumper.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"
#include "core/features/Skins.hpp"
#include "core/features/GameRadar.hpp"
#include "core/features/AutoAccept.hpp"
#include "core/features/ClanTag.hpp"
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
    return GetInstance().insecure;
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

    if (!Dumper::Init()) {
        LOGF(FATAL, "Failed to dump game offsets");
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
    Subtick::Init();
    Visuals::Init();
    Freecam::Init();

    LOGF(INFO, "Successfully initialized engine...");
    return true;
}

void Engine::Thread() {
    // TODO: Check build number 
    // uintptr_t number = process->read<uintptr_t>(base_engine.base + offsets::buildNumber);

    while (true) {
        auto start = steady_clock::now();

        Cache::Refresh();

        if (cfg::settings::free_cpu)
            std::this_thread::sleep_until(start + 1ms);
    }
}

bool Engine::AwaitProcess() {
    if (!process || process->handle_) // Process not initialized, or already attached
        return false;

    do {
        if (process->AttachProcess("cs2.exe"))
            break;

        if (process->pid_ && !process->handle_) {
            LOGF(FATAL, "Insufficient permissions to open a handle to the process. Try running as Administrator.");
            return false;
        }

        static int attempts = 0;

        if (!attempts)
            LOGF(INFO, "Waiting 50s for the game to open...");

        if (attempts > 10)
            return false;
        attempts++;

        std::this_thread::sleep_for(5s);
    } while (true);

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
