#include "ModBrowser.hpp"

#include "CustomModels.hpp"
#include "updater/http/HttpHelper.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>

namespace {
    constexpr auto API = "https://gamebanana.com/apiv11";
    // Counter-Strike 2 > Skins. Player models are in its Server-Side Players, many also right in Skins without a
    // subcategory (the rest of those are weapons & props)
    constexpr int SKINS = 22484;
    constexpr auto PLAYER_MODELS = "/25415";
    constexpr int PER_PAGE = 50;
    constexpr int MAX_PAGES = 10;
    constexpr uint64_t MAX_ARCHIVE = 512ull << 20;
    constexpr DWORD UNPACK_TIMEOUT_MS = 5 * 60 * 1000;

    const std::filesystem::path cache_dir = "cache/models";

    constexpr int CHECK_VERSION = 3;                    // Results of another version of the check are made again
    constexpr int CHECK_WORKERS = 3;

    // Every zip uploaded before this (2026-02-01) that was checked had models for the old animations of CS2: a rar or
    // 7z of then is taken for one too, a newer one is checked once downloaded
    constexpr int64_t NEW_ANIMATIONS_SINCE = 1769904000;
    constexpr uint32_t MAX_MODEL_FILE = 64u << 20;
    constexpr size_t MAX_MODELS_CHECKED = 16;           // Of one file

    // Where game files start in an archive, when it has no csgo/ folder
    constexpr const char* GAME_ROOTS[] = { "characters", "agents", "models", "materials", "particles", "sounds", "soundevents", "animation" };
    constexpr const char* ARCHIVES[] = { ".zip", ".rar", ".7z" };

    std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string Utf8(const std::filesystem::path& path) {
        auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::filesystem::path FromUtf8(const std::string& text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // With / between the folders
    std::string Generic(const std::filesystem::path& path) {
        auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    bool Flag(const json& object, const char* key, bool fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    // GameBanana gives some numbers as strings
    int64_t Number(const json& object, const char* key) {
        auto it = object.find(key);
        if (it == object.end())
            return 0;
        if (it->is_number())
            return it->get<int64_t>();
        if (it->is_string()) {
            try { return std::stoll(it->get<std::string>()); }
            catch (...) {}
        }
        return 0;
    }

    std::string Text(const json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // Compiled resources of Source 2: ".vmdl_c", ".vtex_c", ... Nothing that runs
    bool IsResource(const std::filesystem::path& file) {
        auto extension = Lower(Utf8(file.extension()));
        if (extension.size() < 4 || !extension.starts_with(".v") || !extension.ends_with("_c"))
            return false;
        return std::all_of(extension.begin() + 1, extension.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    }

    // "Some Mod/csgo/characters/models/x.vmdl_c" or "characters/models/x.vmdl_c" -> "characters/models/x.vmdl_c"
    std::optional<std::filesystem::path> GamePath(const std::filesystem::path& relative) {
        std::vector<std::filesystem::path> parts(relative.begin(), relative.end());

        size_t start = parts.size();
        for (size_t k = 0; k < parts.size(); k++)
            if (Lower(Utf8(parts[k])) == "csgo")
                start = k + 1;

        if (start >= parts.size()) {
            for (size_t k = 0; k + 1 < parts.size() && start >= parts.size(); k++)
                if (std::any_of(std::begin(GAME_ROOTS), std::end(GAME_ROOTS), [&](const char* root) { return Lower(Utf8(parts[k])) == root; }))
                    start = k;
        }

        if (start >= parts.size())
            return std::nullopt;

        std::filesystem::path out;
        for (size_t k = start; k < parts.size(); k++) {
            auto part = Utf8(parts[k]);
            if (part == ".." || part == ".")
                return std::nullopt;
            out /= parts[k];
        }
        return out;
    }

    // "characters\\Models\\X.vmdl_c" -> "characters/models/x.vmdl"
    std::string ResourceName(const std::filesystem::path& path) {
        auto name = Lower(Generic(path));
        if (name.ends_with("_c"))
            name.resize(name.size() - 2);
        return name;
    }

    std::string DownloadUrl(int file_id) {
        return std::format("https://gamebanana.com/dl/{}", file_id);
    }
}

// zlib, linked already for curl: the raw deflate of zip entries
extern "C" {
    struct ZipStream {
        const uint8_t* next_in;
        unsigned int avail_in;
        unsigned long total_in;
        uint8_t* next_out;
        unsigned int avail_out;
        unsigned long total_out;
        const char* msg;
        void* state;
        void* zalloc;
        void* zfree;
        void* opaque;
        int data_type;
        unsigned long adler;
        unsigned long reserved;
    };

    int inflateInit2_(ZipStream* stream, int window_bits, const char* version, int stream_size);
    int inflate(ZipStream* stream, int flush);
    int inflateEnd(ZipStream* stream);
}

namespace {
    bool Inflate(const uint8_t* source, size_t size, size_t out_size, std::vector<uint8_t>& out) {
        constexpr int Z_FINISH = 4, Z_STREAM_END = 1;

        out.resize(out_size);
        ZipStream stream{};
        stream.next_in = source;
        stream.avail_in = static_cast<unsigned int>(size);
        stream.next_out = out.data();
        stream.avail_out = static_cast<unsigned int>(out_size);

        if (inflateInit2_(&stream, -15, "1.2.13", sizeof(stream)) != 0)
            return false;

        int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        return result == Z_STREAM_END && stream.total_out == out_size;
    }

    // Bytes first to last of a file of the given size. Servers that send the whole file are served too
    enum class Fetch { OK, BAD, NETWORK };
    Fetch Range(const std::string& url, uint64_t first, uint64_t last, uint64_t total, std::string& out) {
        int status = HttpHelper::GetRange(url, first, last, out);
        if (status == 206)
            return out.size() == last - first + 1 ? Fetch::OK : Fetch::NETWORK;
        if (status == 200 && out.size() == total) {
            out = out.substr(static_cast<size_t>(first), static_cast<size_t>(last - first + 1));
            return Fetch::OK;
        }
        return status == 404 || status == 416 ? Fetch::BAD : Fetch::NETWORK;
    }

    template<class T>
    T Get(const std::string& data, size_t offset) {
        T value{};
        if (offset + sizeof(T) <= data.size())
            memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }

    struct ZipEntry {
        std::string name;
        uint16_t method = 0;
        uint32_t compressed = 0, size = 0, offset = 0;
    };

    // The central directory at the end of the zip, from a range of its end
    Fetch ReadZipDirectory(const std::string& url, uint64_t total, std::vector<ZipEntry>& entries) {
        if (total < 22)
            return Fetch::BAD;

        uint64_t tail_size = std::min<uint64_t>(total, 66000);
        uint64_t tail_start = total - tail_size;
        std::string tail;
        if (auto result = Range(url, tail_start, total - 1, total, tail); result != Fetch::OK)
            return result;

        // End of central directory, the last one
        size_t end = std::string::npos;
        for (size_t k = tail.size() - 22 + 1; k-- > 0;) {
            if (Get<uint32_t>(tail, k) == 0x06054B50) {
                end = k;
                break;
            }
        }
        if (end == std::string::npos)
            return Fetch::BAD;

        uint32_t directory_size = Get<uint32_t>(tail, end + 12);
        uint32_t directory_offset = Get<uint32_t>(tail, end + 16);
        if (directory_offset == 0xFFFFFFFF || directory_size > (16u << 20) || static_cast<uint64_t>(directory_offset) + directory_size > total)
            return Fetch::BAD;      // Zip64 or broken

        std::string directory;
        if (directory_offset >= tail_start)
            directory = tail.substr(static_cast<size_t>(directory_offset - tail_start), directory_size);
        else if (auto result = Range(url, directory_offset, static_cast<uint64_t>(directory_offset) + directory_size - 1, total, directory); result != Fetch::OK)
            return result;

        for (size_t p = 0; p + 46 <= directory.size() && Get<uint32_t>(directory, p) == 0x02014B50 && entries.size() < 20000;) {
            ZipEntry entry;
            entry.method = Get<uint16_t>(directory, p + 10);
            entry.compressed = Get<uint32_t>(directory, p + 20);
            entry.size = Get<uint32_t>(directory, p + 24);
            uint16_t name_length = Get<uint16_t>(directory, p + 28);
            uint16_t extra_length = Get<uint16_t>(directory, p + 30);
            uint16_t comment_length = Get<uint16_t>(directory, p + 32);
            entry.offset = Get<uint32_t>(directory, p + 42);

            if (p + 46 + name_length > directory.size())
                break;
            entry.name = directory.substr(p + 46, name_length);
            std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
            entries.push_back(std::move(entry));

            p += 46 + name_length + extra_length + comment_length;
        }

        return entries.empty() ? Fetch::BAD : Fetch::OK;
    }

    // One file of the zip: its local header, then its data, stored or deflated
    Fetch ReadZipEntry(const std::string& url, uint64_t total, const ZipEntry& entry, std::vector<uint8_t>& out) {
        if ((entry.method != 0 && entry.method != 8) || entry.size > MAX_MODEL_FILE)
            return Fetch::BAD;

        uint64_t want = 30ull + entry.name.size() + 1024 + entry.compressed;
        uint64_t last = std::min<uint64_t>(total - 1, entry.offset + want - 1);
        std::string block;
        if (auto result = Range(url, entry.offset, last, total, block); result != Fetch::OK)
            return result;

        if (Get<uint32_t>(block, 0) != 0x04034B50)
            return Fetch::BAD;

        size_t start = 30ull + Get<uint16_t>(block, 26) + Get<uint16_t>(block, 28);
        if (start + entry.compressed > block.size()) {
            // A long extra field: the data again, now that its start is known
            if (auto result = Range(url, entry.offset + start, entry.offset + start + entry.compressed - 1, total, block); result != Fetch::OK)
                return result;
            start = 0;
        }

        auto data = reinterpret_cast<const uint8_t*>(block.data()) + start;
        if (entry.method == 0) {
            if (entry.compressed != entry.size)
                return Fetch::BAD;
            out.assign(data, data + entry.size);
            return Fetch::OK;
        }

        return Inflate(data, entry.compressed, entry.size, out) ? Fetch::OK : Fetch::BAD;
    }
}

void ModBrowser::Start() {
    List();
}

void ModBrowser::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;
    i.cancel = true;
}

std::vector<ModBrowser::Mod> ModBrowser::List() {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);

    if (i.list_state == ListState::IDLE) {
        // What the last run had, shown while GameBanana is asked again
        i.LoadList();
        i.list_state = i.mods.empty() ? ListState::LOADING : ListState::READY;
        std::thread(&ModBrowser::LoadThread, &i).detach();
    }

    auto out = i.mods;
    for (auto& mod : out) {
        if (auto it = i.verdicts.find(mod.id); it != i.verdicts.end()) {
            mod.verdict = it->second.verdict;
            mod.note = it->second.note;
            mod.own_hands = it->second.own_hands;
        }
    }
    return out;
}

void ModBrowser::CheckProgress(int& checked, int& total, int& usable) {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);

    checked = usable = 0;
    total = static_cast<int>(i.check_queue.size());
    for (auto id : i.check_queue) {
        auto it = i.verdicts.find(id);
        if (it == i.verdicts.end() || it->second.verdict == Mod::Verdict::PENDING)
            continue;
        checked++;
        usable += it->second.verdict == Mod::Verdict::USABLE;
    }
}

ModBrowser::ListState ModBrowser::GetListState() {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    return i.list_state;
}

void ModBrowser::Reload() {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    if (i.list_state == ListState::FAILED)
        i.list_state = ListState::IDLE;
}

void ModBrowser::LoadThread() {
    // Behind the game & the overlay
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    std::vector<Mod> found;
    bool failed = false;

    for (int page = 1; page <= MAX_PAGES && !this->stopping; page++) {
        auto url = std::format("{}/Mod/Index?_nPage={}&_nPerpage={}&_aFilters%5BGeneric_Category%5D={}&_sSort=Generic_MostDownloaded",
            API, page, PER_PAGE, SKINS);

        json response;
        int status = HttpHelper::Get(url, response);
        if (status != 200 || !response.is_object() || !response.contains("_aRecords") || !response["_aRecords"].is_array()) {
            LOGF(WARNING, "Could not get the GameBanana model list (page {}, status {})", page, status);
            failed = found.empty();
            break;
        }

        for (const auto& record : response["_aRecords"]) {
            // Ones with content ratings (mature) are left out
            if (!record.is_object() || Flag(record, "_bHasContentRatings", false) || !Flag(record, "_bHasFiles", true))
                continue;

            // Server-Side Players, or no subcategory
            if (auto sub = record.find("_aSubCategory"); sub != record.end() && sub->is_object() && !Text(*sub, "_sProfileUrl").ends_with(PLAYER_MODELS))
                continue;

            Mod mod;
            mod.id = static_cast<int>(Number(record, "_idRow"));
            mod.name = Text(record, "_sName");
            mod.url = Text(record, "_sProfileUrl");
            mod.likes = static_cast<int>(Number(record, "_nLikeCount"));
            mod.views = static_cast<int>(Number(record, "_nViewCount"));
            mod.added = Number(record, "_tsDateAdded");
            mod.modified = Number(record, "_tsDateModified");

            if (auto submitter = record.find("_aSubmitter"); submitter != record.end() && submitter->is_object())
                mod.author = Text(*submitter, "_sName");

            if (auto media = record.find("_aPreviewMedia"); media != record.end() && media->is_object()) {
                auto images = media->find("_aImages");
                if (images != media->end() && images->is_array() && !images->empty() && (*images)[0].is_object()) {
                    const auto& image = (*images)[0];
                    auto file = Text(image, "_sFile530");
                    if (file.empty())
                        file = Text(image, "_sFile");
                    if (!file.empty())
                        mod.image = Text(image, "_sBaseUrl") + "/" + file;
                }
            }

            if (mod.id && !mod.name.empty())
                found.push_back(std::move(mod));
        }

        auto metadata = response.find("_aMetadata");
        if (metadata == response.end() || !metadata->is_object() || Flag(*metadata, "_bIsComplete", true))
            break;
    }

    LOGF(INFO, "GameBanana: {} player models", found.size());

    std::lock_guard lock(this->mutex);

    // Not reached: the kept list stays, checked as it is
    if (failed && !this->mods.empty()) {
        this->list_state = ListState::READY;
    }
    else {
        if (!failed) {
            this->mods = std::move(found);
            SaveList();
        }
        this->list_state = failed ? ListState::FAILED : ListState::READY;
    }

    // Checked in the order of the list, the first ones show first
    this->check_queue.clear();
    for (const auto& mod : this->mods)
        this->check_queue.push_back(mod.id);
    this->check_next = 0;

    for (; this->check_workers < CHECK_WORKERS && !this->check_queue.empty(); this->check_workers++)
        std::thread(&ModBrowser::CheckThread, this).detach();
}

// Its zip, rar & 7z files, kept until the mod changes
bool ModBrowser::GetFiles(int mod_id, int64_t modified, std::vector<File>& files, std::string& error) {
    {
        std::lock_guard lock(this->mutex);
        LoadChecks();
        auto it = this->mod_files.find(mod_id);
        if (it != this->mod_files.end() && (it->second.modified == modified || !modified)) {
            files = it->second.files;
            return true;
        }
    }

    json info;
    int status = HttpHelper::Get(std::format("{}/Mod/{}?_csvProperties=_sName,_aFiles", API, mod_id), info);
    if (status != 200 || !info.is_object() || !info.contains("_aFiles") || !info["_aFiles"].is_array()) {
        error = std::format("could not get its files (status {})", status);
        return false;
    }

    files.clear();
    for (const auto& entry : info["_aFiles"]) {
        if (!entry.is_object())
            continue;

        File file;
        file.id = static_cast<int>(Number(entry, "_idRow"));
        file.name = Text(entry, "_sFile");
        file.size = static_cast<uint64_t>(Number(entry, "_nFilesize"));
        file.description = Text(entry, "_sDescription");
        file.added = Number(entry, "_tsDateAdded");

        auto extension = Lower(Utf8(FromUtf8(file.name).extension()));
        if (file.id && std::any_of(std::begin(ARCHIVES), std::end(ARCHIVES), [&](const char* a) { return extension == a; }))
            files.push_back(std::move(file));
    }

    std::lock_guard lock(this->mutex);
    this->mod_files[mod_id] = { modified, files };
    this->checks_dirty = true;
    return true;
}

void ModBrowser::CheckThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    int done = 0;

    while (!this->stopping) {
        int mod_id = 0;
        int64_t modified = 0;
        {
            std::lock_guard lock(this->mutex);
            if (this->check_next >= this->check_queue.size())
                break;

            mod_id = this->check_queue[this->check_next++];
            auto mod = std::find_if(this->mods.begin(), this->mods.end(), [&](const Mod& m) { return m.id == mod_id; });
            modified = mod != this->mods.end() ? mod->modified : 0;
        }

        std::vector<File> files;
        std::string error;
        auto verdict = Mod::Verdict::UNUSABLE;
        std::string note;
        bool own_hands = true;

        if (!GetFiles(mod_id, modified, files, error)) {
            verdict = Mod::Verdict::UNCHECKED;
            note = error;
        }
        else if (files.empty()) {
            note = "it has no zip, rar or 7z";
        }
        else {
            // Usable when one file is (best one with hands of its own), else checked on download when one could not
            // be checked, else why not
            std::string unchecked_note, unusable_note;
            for (const auto& file : files) {
                auto check = CheckFile(file);
                if (check && check->state == FileState::USABLE) {
                    own_hands = verdict != Mod::Verdict::USABLE ? check->own_hands : own_hands || check->own_hands;
                    verdict = Mod::Verdict::USABLE;
                    if (own_hands)
                        break;
                    continue;
                }

                auto& slot = !check || check->state == FileState::UNCHECKED ? unchecked_note : unusable_note;
                if (slot.empty())
                    slot = check ? check->note : "Could not be checked now";
            }

            if (verdict != Mod::Verdict::USABLE) {
                verdict = unchecked_note.empty() ? Mod::Verdict::UNUSABLE : Mod::Verdict::UNCHECKED;
                note = unchecked_note.empty() ? unusable_note : unchecked_note;
                own_hands = true;
            }
        }

        std::lock_guard lock(this->mutex);
        this->verdicts[mod_id] = { verdict, note, own_hands };
        if (++done % 10 == 0 && this->checks_dirty)
            SaveChecks();
    }

    std::lock_guard lock(this->mutex);
    if (--this->check_workers == 0 && this->checks_dirty)
        SaveChecks();
}

// Kept by file: a file of GameBanana never changes
std::optional<ModBrowser::FileCheck> ModBrowser::CheckFile(const File& file) {
    // The files of the game are part of the check, without them every model would miss some
    if (CustomModels::Folder().empty())
        return std::nullopt;

    {
        std::lock_guard lock(this->mutex);
        LoadChecks();
        if (auto it = this->file_checks.find(file.id); it != this->file_checks.end())
            return it->second;
    }

    std::optional<FileCheck> check;
    auto extension = Lower(Utf8(FromUtf8(file.name).extension()));
    if (extension == ".zip") {
        check = CheckZip(file);
        if (!check)
            return std::nullopt;    // Tried again another time
    }

    // Not readable in parts: by when it was uploaded
    if (!check || check->state == FileState::UNCHECKED) {
        if (file.added && file.added < NEW_ANIMATIONS_SINCE)
            check = FileCheck{ FileState::UNUSABLE, "Uploaded before the animations of CS2 changed (2026), made for the old ones like every model of then that was checked" };
        else
            check = FileCheck{ FileState::UNCHECKED, "A rar or 7z can't be checked before downloading, it is checked then, before anything goes in the game" };
    }

    if (check && !this->stopping) {
        std::lock_guard lock(this->mutex);
        this->file_checks[file.id] = *check;
        this->checks_dirty = true;
    }
    return check;
}

namespace {
    // What the models of a file came to: usable when one of them is (or is once moved to where it belongs). Else
    // why the main one is not, the extras (arms, copies without hitboxes) say little
    ModBrowser::FileCheck Judge(const std::vector<CustomModel>& models);
}

std::optional<ModBrowser::FileCheck> ModBrowser::CheckZip(const File& file) {
    auto url = DownloadUrl(file.id);

    std::vector<ZipEntry> entries;
    auto result = ReadZipDirectory(url, file.size, entries);
    if (result == Fetch::NETWORK)
        return std::nullopt;
    if (result == Fetch::BAD)
        return FileCheck{ FileState::UNCHECKED, "its zip could not be read in parts" };

    // Its files where they go in csgo/
    std::unordered_set<std::string> names;
    std::vector<std::pair<const ZipEntry*, std::filesystem::path>> models;
    for (const auto& entry : entries) {
        auto game_path = GamePath(FromUtf8(entry.name));
        if (!game_path)
            continue;

        names.insert(Lower(Generic(*game_path)));
        if (Lower(Utf8(game_path->extension())) == ".vmdl_c" && models.size() < MAX_MODELS_CHECKED)
            models.emplace_back(&entry, *game_path);
    }

    if (models.empty())
        return FileCheck{ FileState::UNUSABLE, "there is no CS2 model in it" };

    std::vector<CustomModel> checked;
    for (const auto& [entry, game_path] : models) {
        if (this->stopping)
            return std::nullopt;

        std::vector<uint8_t> bytes;
        auto read = ReadZipEntry(url, file.size, *entry, bytes);
        if (read == Fetch::NETWORK)
            return std::nullopt;
        if (read == Fetch::BAD)
            continue;

        checked.push_back(CustomModels::CheckData(bytes, ResourceName(game_path),
            [&](const std::string& name) { return names.contains(name); }));
    }

    if (checked.empty())
        return FileCheck{ FileState::UNCHECKED, "its models could not be read in parts" };
    return Judge(checked);
}

// The models of an unpacked archive, before they go in the game
ModBrowser::FileCheck ModBrowser::CheckFolder(const std::filesystem::path& folder) {
    std::error_code ec;
    std::unordered_set<std::string> names;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> models;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec))
            continue;

        auto game_path = GamePath(std::filesystem::relative(it->path(), folder, entry_ec));
        if (entry_ec || !game_path)
            continue;

        names.insert(Lower(Generic(*game_path)));
        if (Lower(Utf8(game_path->extension())) == ".vmdl_c" && models.size() < MAX_MODELS_CHECKED)
            models.emplace_back(it->path(), *game_path);
    }

    std::vector<CustomModel> checked;
    for (const auto& [path, game_path] : models) {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty() || bytes.size() > MAX_MODEL_FILE)
            continue;

        checked.push_back(CustomModels::CheckData(bytes, ResourceName(game_path),
            [&](const std::string& name) { return names.contains(name); }));
    }

    return Judge(checked);
}

namespace {
    ModBrowser::FileCheck Judge(const std::vector<CustomModel>& models) {
        if (models.empty())
            return { ModBrowser::FileState::UNUSABLE, "there is no CS2 model in it" };

        // Usable: with first person hands of its own when one of the usable models has them
        bool usable = false, own_hands = false;
        for (const auto& model : models) {
            if (model.status != CustomModel::Status::BAD || model.fits_there) {
                usable = true;
                own_hands |= model.hands_mask != 0;
            }
        }
        if (usable)
            return { ModBrowser::FileState::USABLE, "", own_hands };

        auto extra = [](const CustomModel& model) {
            auto name = Lower(model.name);
            return name.find("hitbox") != std::string::npos || name.find("arm") != std::string::npos;
        };
        auto main = std::find_if(models.begin(), models.end(), [&](const CustomModel& model) { return !extra(model); });
        const auto& reason = main != models.end() ? *main : models.front();
        return { ModBrowser::FileState::UNUSABLE, reason.note };
    }
}

// cache/models/checks.json, with mutex
void ModBrowser::LoadChecks() {
    if (this->checks_loaded)
        return;
    this->checks_loaded = true;

    std::ifstream f(cache_dir / "checks.json");
    if (!f.good())
        return;

    try {
        auto data = json::parse(f);
        if (data.value("version", 0) != CHECK_VERSION)
            return;

        for (const auto& [key, value] : data["files"].items())
            this->file_checks[std::stoi(key)] = { static_cast<FileState>(value.value("state", 0)), value.value("note", ""), value.value("hands", true) };

        for (const auto& [key, value] : data["verdicts"].items())
            this->verdicts[std::stoi(key)] = { static_cast<Mod::Verdict>(value.value("verdict", 0)), value.value("note", ""), value.value("hands", true) };

        for (const auto& [key, value] : data["mods"].items()) {
            ModFiles entry;
            entry.modified = value.value("modified", int64_t(0));
            for (const auto& file : value["files"])
                entry.files.push_back({ file.value("id", 0), file.value("name", ""), file.value("size", uint64_t(0)), file.value("description", ""),
                    file.value("added", int64_t(0)) });
            this->mod_files[std::stoi(key)] = std::move(entry);
        }
    }
    catch (...) {
        LOGF(WARNING, "GameBanana: the kept checks could not be read, they are made again");
        this->file_checks.clear();
        this->mod_files.clear();
        this->verdicts.clear();
    }
}

void ModBrowser::SaveChecks() {
    json files = json::object();
    for (const auto& [id, check] : this->file_checks)
        files[std::to_string(id)] = { { "state", static_cast<int>(check.state) }, { "note", check.note }, { "hands", check.own_hands } };

    json mods = json::object();
    for (const auto& [id, entry] : this->mod_files) {
        json list = json::array();
        for (const auto& file : entry.files)
            list.push_back({ { "id", file.id }, { "name", file.name }, { "size", file.size }, { "description", file.description }, { "added", file.added } });
        mods[std::to_string(id)] = { { "modified", entry.modified }, { "files", list } };
    }

    json verdicts = json::object();
    for (const auto& [id, verdict] : this->verdicts)
        if (verdict.verdict != Mod::Verdict::PENDING)
            verdicts[std::to_string(id)] = { { "verdict", static_cast<int>(verdict.verdict) }, { "note", verdict.note }, { "hands", verdict.own_hands } };

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::ofstream f(cache_dir / "checks.json", std::ios::trunc);
    f << json{ { "version", CHECK_VERSION }, { "files", files }, { "mods", mods }, { "verdicts", verdicts } }.dump();
    this->checks_dirty = false;
}

// cache/models/list.json: the list of the last run, with mutex
void ModBrowser::LoadList() {
    if (this->list_loaded)
        return;
    this->list_loaded = true;

    LoadChecks();   // With the verdicts of its mods

    std::ifstream f(cache_dir / "list.json");
    if (!f.good())
        return;

    try {
        for (const auto& entry : json::parse(f)) {
            Mod mod;
            mod.id = entry.value("id", 0);
            mod.name = entry.value("name", "");
            mod.author = entry.value("author", "");
            mod.image = entry.value("image", "");
            mod.url = entry.value("url", "");
            mod.likes = entry.value("likes", 0);
            mod.views = entry.value("views", 0);
            mod.added = entry.value("added", int64_t(0));
            mod.modified = entry.value("modified", int64_t(0));
            if (mod.id && !mod.name.empty())
                this->mods.push_back(std::move(mod));
        }
    }
    catch (...) {
        this->mods.clear();
    }
}

void ModBrowser::SaveList() {
    json list = json::array();
    for (const auto& mod : this->mods)
        list.push_back({ { "id", mod.id }, { "name", mod.name }, { "author", mod.author }, { "image", mod.image }, { "url", mod.url },
            { "likes", mod.likes }, { "views", mod.views }, { "added", mod.added }, { "modified", mod.modified } });

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::ofstream f(cache_dir / "list.json", std::ios::trunc);
    f << list.dump();
}

void ModBrowser::Install(const Mod& mod, int file_id) {
    auto& i = GetInstance();
    if (i.busy.exchange(true))
        return;

    i.cancel = false;
    std::thread(&ModBrowser::InstallThread, &i, mod, file_id).detach();
}

ModBrowser::Job ModBrowser::GetJob(int mod_id) {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    auto it = i.jobs.find(mod_id);
    return it != i.jobs.end() ? it->second : Job{};
}

void ModBrowser::ClearJob(int mod_id) {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    if (auto it = i.jobs.find(mod_id); it != i.jobs.end() && it->second.state == Job::State::DONE)
        i.jobs.erase(it);
}

bool ModBrowser::IsBusy() {
    return GetInstance().busy;
}

void ModBrowser::Cancel() {
    GetInstance().cancel = true;
}

void ModBrowser::SetJob(int mod_id, const Job& job) {
    std::lock_guard lock(this->mutex);
    this->jobs[mod_id] = job;
}

void ModBrowser::InstallThread(Mod mod, int file_id) {
    Job job;
    auto fail = [&](const std::string& why) {
        LOGF(WARNING, "GameBanana: {} not installed, {}", mod.name, why);
        job.state = Job::State::FAILED;
        job.message = why;
        SetJob(mod.id, job);
        this->busy = false;
    };

    job.state = Job::State::FETCHING;
    job.message = "Getting its files...";
    SetJob(mod.id, job);

    // The csgo folder of the running game
    auto models_folder = CustomModels::Folder();
    if (models_folder.empty())
        return fail("start CS2 first, the model goes in its folder");
    auto csgo = models_folder.parent_path().parent_path();

    std::vector<File> files;
    std::string files_error;
    if (!GetFiles(mod.id, mod.modified, files, files_error))
        return fail(files_error);

    if (files.empty())
        return fail("it has no zip, rar or 7z to download");

    // Only the files the check found usable, when it found any
    {
        std::lock_guard lock(this->mutex);
        std::vector<File> usable;
        for (const auto& file : files)
            if (auto it = this->file_checks.find(file.id); it != this->file_checks.end() && it->second.state == FileState::USABLE)
                usable.push_back(file);
        if (!usable.empty())
            files = std::move(usable);
    }
    int status = 0;

    // Several files (T & CT, variants): the user picks one
    if (!file_id && files.size() > 1) {
        job.state = Job::State::CHOOSE;
        job.message = "Pick a file";
        job.files = files;
        SetJob(mod.id, job);
        this->busy = false;
        return;
    }

    auto file = file_id ? std::find_if(files.begin(), files.end(), [&](const File& f) { return f.id == file_id; }) : files.begin();
    if (file == files.end())
        return fail("the file is not in the mod anymore");
    if (file->size > MAX_ARCHIVE)
        return fail("the file is too large");

    // Download
    std::error_code ec;
    auto downloads = std::filesystem::absolute(cache_dir / "downloads", ec);
    std::filesystem::create_directories(downloads, ec);
    auto archive = downloads / std::format("{}{}", file->id, Lower(Utf8(FromUtf8(file->name).extension())));

    job.state = Job::State::DOWNLOADING;
    job.message = std::format("Downloading {}...", file->name);
    SetJob(mod.id, job);

    auto last = std::chrono::steady_clock::now();
    status = HttpHelper::Download(DownloadUrl(file->id), archive, MAX_ARCHIVE,
        [&](uint64_t done, uint64_t total) {
            auto now = std::chrono::steady_clock::now();
            if (total && now - last > std::chrono::milliseconds(100)) {
                last = now;
                job.progress = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
                SetJob(mod.id, job);
            }
            return !this->cancel && !this->stopping;
        });

    if (this->cancel || this->stopping) {
        std::filesystem::remove(archive, ec);
        job = {};
        SetJob(mod.id, job);
        this->busy = false;
        return;
    }

    if (status != 200) {
        std::filesystem::remove(archive, ec);
        return fail(std::format("the download failed (status {})", status));
    }

    // Unpack next to it
    job.state = Job::State::UNPACKING;
    job.message = "Unpacking...";
    job.progress = 1.f;
    SetJob(mod.id, job);

    auto folder = std::filesystem::absolute(cache_dir / "unpack" / std::to_string(file->id), ec);
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);

    std::string error;
    bool unpacked = Unpack(archive, folder, error, [&]() { return this->cancel || this->stopping; });
    std::filesystem::remove(archive, ec);

    auto cleanup = [&]() { std::filesystem::remove_all(folder, ec); };
    if (!unpacked) {
        cleanup();
        return fail(error);
    }

    // Its models first: nothing goes in the game when none can be used
    auto check = CheckFolder(folder);
    {
        // The list knows now too: one that can't be used leaves it
        std::lock_guard lock(this->mutex);
        this->file_checks[file->id] = check;
        this->checks_dirty = true;
        if (check.state == FileState::USABLE)
            this->verdicts[mod.id] = { Mod::Verdict::USABLE, "", check.own_hands };
        else if (files.size() == 1)
            this->verdicts[mod.id] = { Mod::Verdict::UNUSABLE, check.note, true };
    }
    if (check.state != FileState::USABLE) {
        cleanup();
        return fail("none of its models can be used: " + check.note);
    }

    // The game files in it, at their paths in csgo/
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> copies;
    bool has_model = false, has_package = false;

    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec))
            continue;

        if (Lower(Utf8(it->path().extension())) == ".vpk")
            has_package = true;
        if (!IsResource(it->path()))
            continue;

        auto game_path = GamePath(std::filesystem::relative(it->path(), folder, entry_ec));
        if (entry_ec || !game_path)
            continue;

        if (Lower(Utf8(it->path().extension())) == ".vmdl_c")
            has_model = true;
        copies.emplace_back(it->path(), *game_path);
    }

    if (!has_model) {
        cleanup();
        return fail(has_package ? "it comes as a .vpk, which can't be put in the game this way" : "there is no CS2 model in it");
    }

    Installed record;
    record.name = mod.name;
    record.file_id = file->id;

    int failed = 0;
    for (const auto& [source, game_path] : copies) {
        auto target = csgo / game_path;
        std::error_code copy_ec;
        std::filesystem::create_directories(target.parent_path(), copy_ec);
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
            failed++;
            continue;
        }

        record.files.push_back(Generic(game_path));
        if (Lower(Utf8(game_path.extension())) == ".vmdl_c")
            record.models.push_back(ResourceName(game_path));
    }
    cleanup();

    if (record.models.empty())
        return fail("its files could not be copied to the game folder, is the game using them?");

    {
        std::lock_guard lock(this->mutex);
        LoadInstalled();
        this->installed[mod.id] = record;
        SaveInstalled();
    }

    CustomModels::Rescan();

    LOGF(INFO, "GameBanana: {} installed, {} files, models {}", mod.name, record.files.size(), record.models.size());

    job = {};
    job.state = Job::State::DONE;
    job.message = failed ? std::format("Installed, {} file(s) could not be copied", failed) : "Installed, pick it in the agent list";
    SetJob(mod.id, job);
    this->busy = false;
}

// tar of Windows (libarchive): zip, rar & 7z. It refuses paths out of the folder by itself
bool ModBrowser::Unpack(const std::filesystem::path& archive, const std::filesystem::path& folder, std::string& error,
    const std::function<bool()>& stop) {
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) {
        error = "the Windows folder was not found";
        return false;
    }

    auto tar = std::filesystem::path(system) / L"tar.exe";
    std::error_code ec;
    if (!std::filesystem::exists(tar, ec)) {
        error = "tar.exe of Windows is missing (Windows 10 1803 or newer has it)";
        return false;
    }

    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + folder.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process)) {
        error = std::format("could not start tar.exe ({})", GetLastError());
        return false;
    }

    auto started = GetTickCount64();
    DWORD wait = WAIT_TIMEOUT;
    while ((wait = WaitForSingleObject(process.hProcess, 250)) == WAIT_TIMEOUT) {
        if (stop() || GetTickCount64() - started > UNPACK_TIMEOUT_MS) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (wait == WAIT_TIMEOUT) {
        error = stop() ? "cancelled" : "unpacking took too long";
        return false;
    }

    // Warnings give an exit code too, what it unpacked still counts
    if (code != 0) {
        LOGF(VERBOSE, "GameBanana: tar.exe ended with {}", code);
        if (std::filesystem::is_empty(folder, ec)) {
            error = std::format("could not unpack it (tar.exe {})", code);
            return false;
        }
    }

    return true;
}

bool ModBrowser::IsInstalled(int mod_id) {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    i.LoadInstalled();
    return i.installed.contains(mod_id);
}

std::vector<std::string> ModBrowser::ModelsOf(int mod_id) {
    auto& i = GetInstance();
    std::lock_guard lock(i.mutex);
    i.LoadInstalled();
    auto it = i.installed.find(mod_id);
    return it != i.installed.end() ? it->second.models : std::vector<std::string>{};
}

bool ModBrowser::Remove(int mod_id, std::vector<std::string>& models, std::string& error) {
    auto& i = GetInstance();
    if (i.busy) {
        error = "Wait for the download to finish";
        return false;
    }

    auto models_folder = CustomModels::Folder();
    if (models_folder.empty()) {
        error = "Start CS2 first, the model is in its folder";
        return false;
    }
    auto csgo = models_folder.parent_path().parent_path();

    std::lock_guard lock(i.mutex);
    i.LoadInstalled();

    auto it = i.installed.find(mod_id);
    if (it == i.installed.end()) {
        error = "It was not installed from here";
        return false;
    }

    // Files another installed mod has too stay
    std::unordered_set<std::string> shared;
    for (const auto& [id, other] : i.installed)
        if (id != mod_id)
            for (const auto& file : other.files)
                shared.insert(Lower(file));

    auto depth = [&](const std::filesystem::path& path) {
        std::error_code ec;
        auto relative = std::filesystem::relative(path, csgo, ec);
        return ec ? 0 : std::distance(relative.begin(), relative.end());
    };

    int left = 0;
    for (const auto& file : it->second.files) {
        if (shared.contains(Lower(file)))
            continue;

        std::error_code ec;
        auto path = csgo / FromUtf8(file);
        std::filesystem::remove(path, ec);
        if (ec) {
            left++;
            continue;
        }

        // Folders left empty, down to csgo/characters/models & the like
        for (auto parent = path.parent_path(); depth(parent) > 2 && std::filesystem::is_empty(parent, ec) && !ec; parent = parent.parent_path())
            std::filesystem::remove(parent, ec);
    }

    models = it->second.models;
    LOGF(INFO, "GameBanana: {} removed{}", it->second.name, left ? std::format(", {} files could not be removed", left) : "");

    i.installed.erase(it);
    i.jobs.erase(mod_id);
    i.SaveInstalled();

    CustomModels::Rescan();

    if (left) {
        error = std::format("{} file(s) could not be removed, is the game using them?", left);
        return false;
    }
    return true;
}

// cache/models/installed.json, with mutex
void ModBrowser::LoadInstalled() {
    if (this->installed_loaded)
        return;
    this->installed_loaded = true;

    std::ifstream f(cache_dir / "installed.json");
    if (!f.good())
        return;

    try {
        auto data = json::parse(f);
        for (const auto& [key, value] : data.items()) {
            Installed record;
            record.name = value.value("name", "");
            record.file_id = value.value("file", 0);
            record.files = value.value("files", std::vector<std::string>{});
            record.models = value.value("models", std::vector<std::string>{});
            this->installed[std::stoi(key)] = std::move(record);
        }
    }
    catch (...) {
        LOGF(WARNING, "GameBanana: the list of installed models could not be read");
    }
}

void ModBrowser::SaveInstalled() {
    json data = json::object();
    for (const auto& [id, record] : this->installed)
        data[std::to_string(id)] = { { "name", record.name }, { "file", record.file_id }, { "files", record.files }, { "models", record.models } };

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::ofstream f(cache_dir / "installed.json", std::ios::trunc);
    f << data.dump(2);
}
