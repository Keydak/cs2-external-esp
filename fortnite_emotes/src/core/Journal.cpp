#include "core/Journal.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <mutex>

namespace emotes {
namespace {

using PrintFn = void(__cdecl*)(const char*, ...);

PrintFn Tier0(const char* name) {
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    return tier0 ? reinterpret_cast<PrintFn>(GetProcAddress(tier0, name)) : nullptr;
}

// Documents\Emotes\emotes.log, restarted each injection: what the console said, for when nobody saw it.
void ToFile(const char* level, const std::string& origin, const std::string& text) {
    static std::mutex mu;
    static FILE* f = [] {
        FILE* out = nullptr;
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
            const std::wstring dir = std::wstring(docs) + L"\\Emotes";
            CreateDirectoryW(dir.c_str(), nullptr);
            _wfopen_s(&out, (dir + L"\\emotes.log").c_str(), L"w");
        }
        CoTaskMemFree(docs);
        return out;
    }();
    if (!f) return;
    std::lock_guard lk(mu);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(f, "%02d:%02d:%02d.%03d %s %s: %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level,
                 origin.c_str(), text.c_str());
    std::fflush(f);
}

}  // namespace

void Journal::info(const std::string& origin, const std::string& text) {
    static PrintFn msg = Tier0("Msg");
    if (msg) msg("[emotes] %s: %s\n", origin.c_str(), text.c_str());
    ToFile("info", origin, text);
}

void Journal::warn(const std::string& origin, const std::string& text) {
    static PrintFn warning = Tier0("Warning");
    if (warning) warning("[emotes] %s: %s\n", origin.c_str(), text.c_str());
    ToFile("warn", origin, text);
}

Journal& journal() {
    static Journal j;
    return j;
}

}  // namespace emotes
