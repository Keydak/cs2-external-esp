#include "sdk/GameFiles.h"

#include <windows.h>

#include <mutex>

#include "sdk/PatternScan.h"

namespace emotes::sdk {
namespace {

// filesystem_stdio.dll CFileSystem_Stdio::AddSearchPath; VFileSystem017 vtable[31] must be it.
constexpr const char* kFsAddPattern =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 8B 44 24 78 41 8B F9 8B 5C 24 70 49 8B F0 "
    "89 44 24 30 48 8B EA";

using AddSearchPathFn   = void*(__fastcall*)(void* fs, const char* path, const char* pathID, int addType, int priority, int flags);
using CreateInterfaceFn = void*(*)(const char*, int*);

void*           g_fs    = nullptr;
AddSearchPathFn g_fsAdd = nullptr;

void resolve() {
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE m  = GetModuleHandleA("filesystem_stdio.dll");
        auto    ci = m ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(m, "CreateInterface")) : nullptr;
        const uintptr_t add = ci ? PatternScan("filesystem_stdio.dll", kFsAddPattern) : 0;
        if (!add) return;
        __try {
            void* fs = ci("VFileSystem017", nullptr);
            if (fs && (*reinterpret_cast<uintptr_t**>(fs))[31] == add) {
                g_fs    = fs;
                g_fsAdd = reinterpret_cast<AddSearchPathFn>(add);
            }
        } __except (1) {}
    });
}

}  // namespace

bool CanMountGamePaths() {
    resolve();
    return g_fsAdd != nullptr;
}

bool MountGamePath(const char* utf8Dir) {
    resolve();
    if (!g_fsAdd || !utf8Dir) return false;
    __try {
        g_fsAdd(g_fs, utf8Dir, "GAME", 1, 2, 0);
        return true;
    } __except (1) {
        return false;
    }
}

}  // namespace emotes::sdk
