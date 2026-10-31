#include "ui/Fonts.h"
#include <windows.h>
#include <shlobj.h>
#include <imgui.h>
#include <string>
#include <vector>

namespace emotes::ui {
namespace {

ImFont* g_heavy = nullptr;
ImFont* g_body = nullptr;

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// The first file in dirs whose name holds every one of parts (case-insensitive), e.g. {"burbank", "black"}.
std::filesystem::path FindFont(const std::vector<std::filesystem::path>& dirs, std::initializer_list<const wchar_t*> parts) {
    std::error_code ec;
    for (const auto& dir : dirs) {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring name = it->path().filename().wstring(), ext = it->path().extension().wstring();
            for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
            for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext != L".ttf" && ext != L".otf") continue;
            bool all = true;
            for (const wchar_t* part : parts) all = all && name.find(part) != std::wstring::npos;
            if (all) return it->path();
        }
    }
    return {};
}

ImFont* Add(const std::filesystem::path& file, float size) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) return nullptr;  // ImGui asserts on a missing file
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    const std::u8string utf8 = file.u8string();
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()), size, &config);
}

}  // namespace

void LoadFonts(const std::filesystem::path& packDir) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();  // the fallback, and ImGui's default font
    const std::vector<std::filesystem::path> dirs = {packDir / "fonts", KnownFolder(FOLDERID_Fonts),
                                                     KnownFolder(FOLDERID_LocalAppData) / "Microsoft" / "Windows" / "Fonts"};
    g_heavy = Add(FindFont(dirs, {L"burbank", L"black"}), 64.0f);
    g_body = Add(KnownFolder(FOLDERID_Fonts) / "segoeuib.ttf", 20.0f);
}

ImFont* HeavyFont() { return g_heavy ? g_heavy : ImGui::GetFont(); }
ImFont* BodyFont() { return g_body ? g_body : ImGui::GetFont(); }

}  // namespace emotes::ui
