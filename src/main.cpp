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
#include "core/engine/Engine.hpp"
#include "gui/renderer/Renderer.hpp"
#include "core/features/View.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Skins.hpp"
#include "core/features/Movement.hpp"
#include "core/features/AutoAccept.hpp"
#include "core/features/ClanTag.hpp"
#include "core/features/Subtick.hpp"
#include "core/features/Visuals.hpp"
#include "core/engine/GameThread.hpp"

#include <external/exception.hpp>

int main()
{
    c_exception_handler::setup();

    LogHelper::Init();
    LogHelper::Banner();

    LOGF(INFO, "Compiled {}, welcome to CS2-EXTERNAL!", __TIMESTAMP__);

    // Needs to be ran as ADMINISTRATOR
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        LOGF(WARNING, "Could not set application process priority to HIGH");

    if (!Updater::Init() || !Updater::Process()) {
        LOGF(FATAL, "Updater failed to run, the application has not verified its status, execution its not recommended");
        LOGF(INFO, "Press any key to ignore and continue execution...");
        std::cin.get();
    }

    if (!Engine::Init()) {
        LOGF(FATAL, "Engine failed to initialize, cannot continue execution");
        goto exit;
    }

    if (!Renderer::Init()) {
        LOGF(FATAL, "Renderer failed to initialize, cannot continue execution");
        goto exit;
    }

    LOGF(INFO, "Everything setup and ready, just... make sure you are not in \"Full Screen\"!");

    // Locking
    Renderer::Thread();

    Freecam::Shutdown();
    View::Shutdown();
    Skins::Shutdown();
    Movement::Shutdown();
    AutoAccept::Shutdown();
    ClanTag::Shutdown();
    Subtick::Shutdown();
    Visuals::Shutdown();
    GameThread::Shutdown();

exit:
    LOGF(INFO, "Thats it, im done, hope you had a great time!");
    LogHelper::Destroy();
    std::cin.get();
}