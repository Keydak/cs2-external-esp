#include <windows.h>
#include "hooks/Hooks.h"

static HMODULE g_module = nullptr;

static DWORD WINAPI InitThread(LPVOID) {
    emotes::hooks::Initialize(g_module);

    while (!emotes::hooks::ShouldUnload()) Sleep(100);

    emotes::hooks::Shutdown();
    Sleep(300);
    if (emotes::hooks::CanUnload()) FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_module = hModule;
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
