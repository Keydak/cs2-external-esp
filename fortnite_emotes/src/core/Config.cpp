#include "core/Config.h"
#include "core/Journal.h"
#include "features/EmoteWheel.h"
#include "features/FortniteDances.h"
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace emotes {
namespace {

Config g_cfg;
std::filesystem::path g_dataDir;

int ReadInt(const wchar_t* ini, const wchar_t* key, int def) {
    wchar_t buf[32];
    GetPrivateProfileStringW(L"emotes", key, L"", buf, 32, ini);
    return buf[0] ? static_cast<int>(std::wcstol(buf, nullptr, 0)) : def;  // 0x42 or 66
}

void WriteInt(const wchar_t* ini, const wchar_t* key, int value, bool hex = false) {
    wchar_t buf[32];
    std::swprintf(buf, 32, hex ? L"0x%02X" : L"%d", value);
    WritePrivateProfileStringW(L"emotes", key, buf, ini);
}

// The signed-in user's Documents folder (wherever Windows has it, OneDrive included), else next to the DLL.
std::filesystem::path Documents(const std::filesystem::path& fallback) {
    PWSTR p = nullptr;
    std::filesystem::path out = fallback;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

constexpr const char* kModelFile = "csgo_addons/characters/kolka/fortnite_dance.vmdl_c";

// The pack inside root: root itself, or the folder that holds csgo_addons a few levels down. The site's download
// unzips as preset-files-main\emotes\..., and people extract it into Emotes as it is, or one level deeper.
std::filesystem::path FindPack(const std::filesystem::path& root) {
    std::error_code ec;
    if (std::filesystem::exists(root / kModelFile, ec)) return root;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 3) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec) && std::filesystem::exists(it->path() / kModelFile, ec)) return it->path();
    }
    return {};
}

}  // namespace

void LoadConfig(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    const std::filesystem::path root = Documents(dir) / "Emotes";
    g_dataDir = FindPack(root);
    if (g_dataDir.empty()) {
        g_dataDir = root;  // FortniteDances keeps checking every 2 s, so a pack added later still loads
        journal().warn("config", "Emote Pack not found: extract it into " +
                                     std::string(reinterpret_cast<const char*>(root.u8string().c_str())));
    }
    // Next to the pack, not the DLL: an injector that maps the DLL by hand leaves it no file name to find.
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const std::wstring ini = (root / "emotes.ini").wstring();

    if (!std::filesystem::exists(ini, ec)) {  // first run: write the defaults so they can be edited
        WriteInt(ini.c_str(), L"wheel_key", g_cfg.wheelKey, true);
        WriteInt(ini.c_str(), L"unload_key", g_cfg.unloadKey, true);
        WriteInt(ini.c_str(), L"music", g_cfg.music);
        WriteInt(ini.c_str(), L"volume", static_cast<int>(g_cfg.volume * 100.0f));
        for (int i = 0; i < 6; ++i) WriteInt(ini.c_str(), (L"slot" + std::to_wstring(i + 1)).c_str(), g_cfg.slots[i]);
    }
    g_cfg.wheelKey = std::clamp(ReadInt(ini.c_str(), L"wheel_key", g_cfg.wheelKey), 1, 0xFE);
    g_cfg.unloadKey = std::clamp(ReadInt(ini.c_str(), L"unload_key", g_cfg.unloadKey), 0, 0xFE);
    g_cfg.music = ReadInt(ini.c_str(), L"music", g_cfg.music) != 0;
    g_cfg.volume = std::clamp(ReadInt(ini.c_str(), L"volume", 70), 0, 100) / 100.0f;
    for (int i = 0; i < 6; ++i)
        g_cfg.slots[i] = ReadInt(ini.c_str(), (L"slot" + std::to_wstring(i + 1)).c_str(), g_cfg.slots[i]);

    auto& w = GetEmoteWheelConfig();
    w.emoteEnabled = true;
    w.emoteKey = g_cfg.wheelKey;
    for (int i = 0; i < 6; ++i) w.emoteSlots[i] = g_cfg.slots[i];

    auto& fd = GetFortniteDancesConfig();
    fd.enabled = true;
    fd.music = g_cfg.music;
    fd.volume = g_cfg.volume;
    FortniteDancesSetDir(g_dataDir);

    char key[16];
    std::snprintf(key, sizeof(key), "0x%02X", g_cfg.wheelKey);
    journal().info("config", std::string("wheel key ") + key + ", pack folder " +
                                 reinterpret_cast<const char*>(g_dataDir.u8string().c_str()));
}

const Config& GetConfig() { return g_cfg; }

std::filesystem::path DataDir() { return g_dataDir; }

}  // namespace emotes
