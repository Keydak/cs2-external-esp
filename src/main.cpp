/*
    This project is licensed under CC BY-NC 4.0
    https://creativecommons.org/licenses/by-nc/4.0*

    You are free to:
    • Share: Copy and redistribute the material in any medium or format.
    • Adapt: Remix, transform, and build upon the material*

    Under the following terms:
    • Attribution: You must give appropriate credit, provide a link to the original source repository, and indicate if changes were made.
    • Non-Commercial: You may not use the material for commercial purposes*

    You are not allowed to:
    • Sell: This license forbids selling original or modified material for commercial purposes.
    • Sublicense: This license forbids sublicensing original or modified material.

    © Copyright by IMXNOOBX (https://github.com/IMXNOOBX) and contributors.
    See https://github.com/IMXNOOBX/cs2-external-esp/-/blob/main/LICENSE for full details.
*/

#include <iostream>

#include "updater/Updater.hpp"
#include "gui/loader/Loader.hpp"
#include "core/engine/Engine.hpp"
#include "gui/renderer/Renderer.hpp"
#include "core/features/View.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Skins.hpp"
#include "core/features/CustomModels.hpp"
#include "core/features/ModBrowser.hpp"
#include "core/features/Movement.hpp"
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
#include "core/engine/GameThread.hpp"

#include <external/exception.hpp>

int main()
{
    c_exception_handler::setup();

    LogHelper::Init();

    LOGF(INFO, "Compiled {}, welcome to CS2-EXTERNAL!", __TIMESTAMP__);

    // Normal priority: above it our threads take the CPU from the game, which lost a lot of its frame rate

    // The loader: the version against GitHub, then on Start the game & the items. Closed, or the game could not be
    // used (it says why): nothing was started, straight out
    if (!Loader::Run()) {
        LOGF(INFO, "The loader was closed");
        LogHelper::Destroy();
        ExitProcess(0);
    }

    // Everything checked: the features from here on
    Engine::Start();

    if (!Renderer::Init()) {
        LOGF(FATAL, "Renderer failed to initialize, cannot continue execution");
        goto exit;
    }

    LOGF(INFO, "Everything setup and ready, just... make sure you are not in \"Full Screen\"!");

    // The model list of GameBanana & its check, behind everything else
    ModBrowser::Start();

    // Locking
    Renderer::Thread();

    // The game is gone, nothing in it to put back: straight out. Our threads still run, ExitProcess stops them
    // before anything they use is torn down
    if (Renderer::IsGameClosed()) {
#ifdef _DEBUG
        // Debug keeps the console open, its log is what is read after a crash of the game
        LOGF(INFO, "The game closed, press any key to exit...");
        Logger::FlushQueue();
        std::cin.get();
#endif
        LogHelper::Destroy();
        ExitProcess(0);
    }

    Freecam::Shutdown();
    View::Shutdown();
    Skins::Shutdown();
    CustomModels::Shutdown();
    ModBrowser::Shutdown();
    Movement::Shutdown();
    AutoAccept::Shutdown();
    ClanTag::Shutdown();
    VoteEvents::Shutdown();
    HitEffects::Shutdown();
    Sounds::Shutdown();
    Subtick::Shutdown();
    Visuals::Shutdown();
    MaterialChams::Shutdown();
    AgentPreview::Shutdown();
    KillEffect::Shutdown();
    GameThread::Shutdown();

    // Everything is put back, now say why it stopped. Debug tells what was wrong, release only that it needs an update
    if (Engine::IsOutdated()) {
#ifdef _DEBUG
        LogHelper::Show();
        LOGF(FATAL, "Stopped, outdated for the running CS2: {}. Everything changed in the game was put back, "
            "run update-project/check.bat to see the rest", Engine::GetOutdated());
#else
        // Release has no console
        MessageBoxA(nullptr, "CS2 was updated, this program has to be updated too before it can be used again.",
            "Cs2 External", MB_ICONWARNING);
#endif
    }

exit:
    LOGF(INFO, "Thats it, im done, hope you had a great time!");
    LogHelper::Destroy();
    std::cin.get();
}