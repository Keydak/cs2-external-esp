#include "CustomModels.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/GameThread.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/resource/Resource.hpp"
#include "core/offsets/Offsets.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>

namespace {
    constexpr auto SCAN_INTERVAL = std::chrono::seconds(3);
    constexpr size_t MAX_MODELS = 256;
    constexpr uint32_t MAX_BLOCK_SIZE = 64u << 20;
    constexpr size_t MAX_RESOURCE_NAME = 190;       // The name buffer of the game holds 200, longer goes to the heap
    constexpr float MAX_HANDS_HEIGHT = 45.f;        // First person hands of agents are ~25 units high, a body ~75

    constexpr auto LOAD_TIMEOUT = std::chrono::seconds(15);
    constexpr auto LOAD_POLL = std::chrono::milliseconds(250);
    constexpr auto READY_RECHECK = std::chrono::seconds(5);
    constexpr DWORD PRECACHE_TIMEOUT_MS = 5000;     // The game may load it right there, on its main thread

    // The skeleton of every CS2 agent starts like this, the animations of the game are made for these bones
    struct Bone {
        const char* name;
        int index;
    };
    constexpr Bone REQUIRED_BONES[] = {
        // root_motion (0) is not in many models of the community made for the animations of now, not asked for
        { "pelvis", 1 }, { "spine_0", 2 }, { "spine_1", 3 }, { "spine_2", 4 }, { "spine_3", 5 },
        { "neck_0", 6 }, { "head_0", 7 }, { "clavicle_L", 8 }, { "arm_upper_L", 9 }, { "arm_lower_L", 10 }, { "hand_L", 11 },
        { "clavicle_R", 12 }, { "arm_upper_R", 13 }, { "arm_lower_R", 14 }, { "hand_R", 15 }, { "leg_upper_L", 17 },
        { "leg_lower_L", 18 }, { "ankle_L", 19 }, { "leg_upper_R", 20 }, { "leg_lower_R", 21 }, { "ankle_R", 22 },
        { "wpnPivot", 23 }, { "wpn", 24 },
    };

    // What plays the animations of players: their skeleton & animation graph
    constexpr const char* PLAYER_SKELETON = "animation/skeletons/characters/worldmodel.vnmskel";
    constexpr const char* PLAYER_GRAPH = "animation/graphs/worldmodel/worldmodel.vnmgraph";

    // Where player models are put in csgo/, looked through for models
    constexpr const char* MODEL_FOLDERS[] = { "characters", "agents" };

    // Our page in the game
    constexpr size_t PAGE_SIZE = 0x1000;
    constexpr size_t CODE_PRECACHE = 0x000;
    constexpr size_t CODE_GET_MODEL = 0x080;
    constexpr size_t DATA_NAME = 0x200;         // CResourceNameTyped: a CBufferString of 200 bytes, then its hash & type
    constexpr size_t NAME_SIZE = 0xE0;
    constexpr uint32_t NAME_FLAGS = 0xC00000C8; // Inline buffer of 200 bytes, as the game makes them
    constexpr size_t DATA_PATH = 0x300;
    constexpr size_t PATH_SIZE = 0x100;
    constexpr size_t DATA_REASON = 0x400;       // "", why it is precached
    constexpr size_t DATA_OUT = 0x408;          // The binding GetModel gives

    std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // "Characters\\Models\\X.vmdl_c" -> "characters/models/x.vmdl"
    std::string Normalize(std::string name) {
        std::replace(name.begin(), name.end(), '\\', '/');
        name = Lower(name);
        if (name.size() > 2 && name.ends_with("_c"))
            name.resize(name.size() - 2);
        return name;
    }

    uint64_t Hash(const std::string& text) {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (unsigned char c : text) {
            hash ^= c;
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    std::string Utf8(const std::filesystem::path& path) {
        auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::filesystem::path FromUtf8(const std::string& text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // A block of a compiled resource by its place in the table
    std::optional<std::vector<uint8_t>> BlockAt(const std::vector<uint8_t>& resource, size_t index) {
        auto read32 = [&](size_t offset) {
            uint32_t value = 0;
            if (offset + 4 <= resource.size())
                memcpy(&value, resource.data() + offset, 4);
            return value;
        };

        size_t entry = 8ull + read32(8) + index * 12;
        if (index >= read32(12) || entry + 12 > resource.size())
            return std::nullopt;

        size_t start = entry + 4 + read32(entry + 4);
        size_t size = read32(entry + 8);
        if (start + size > resource.size())
            return std::nullopt;
        return std::vector<uint8_t>(resource.begin() + start, resource.begin() + start + size);
    }

    // Height of the embedded mesh (its bounds), 0 when it can't be told
    float MeshHeight(const std::vector<uint8_t>& resource, const Resource::Value& ctrl, size_t mesh) {
        for (const auto& embedded : ctrl["embedded_meshes"].items) {
            if (embedded["m_nMeshIndex"].i != static_cast<int64_t>(mesh))
                continue;

            auto block = BlockAt(resource, static_cast<size_t>(embedded["m_nDataBlock"].i));
            Resource::Value data;
            if (!block || !Resource::ParseKV3(*block, data))
                return 0.f;

            float low = FLT_MAX, high = -FLT_MAX;
            for (const auto& object : data["m_sceneObjects"].items) {
                const auto& min = object["m_vMinBounds"].items;
                const auto& max = object["m_vMaxBounds"].items;
                if (min.size() < 3 || max.size() < 3)
                    continue;
                low = std::min(low, static_cast<float>(min[2].d));
                high = std::max(high, static_cast<float>(max[2].d));
            }
            return high > low ? high - low : 0.f;
        }
        return 0.f;
    }

    // Missing, these leave nothing to draw or animate: its meshes, the skeleton & graph of its animations
    bool IsEssential(const std::string& reference) {
        for (auto extension : { ".vmesh", ".vnmskel", ".vnmgraph" })
            if (reference.ends_with(extension))
                return true;
        return false;
    }

    // Materials & textures only change how it looks (the checkerboard of the game)
    bool IsLook(const std::string& reference) {
        for (auto extension : { ".vmat", ".vtex", ".vcompmat" })
            if (reference.ends_with(extension))
                return true;
        return false;
    }

    // Left in models from the animations CS2 had before 2024, the game does not have them anymore nor need them
    bool IsOldAnimation(const std::string& reference) {
        return reference.ends_with(".vanmgrph") || reference.starts_with("characters/models/shared/animsets/");
    }

    std::string Join(const std::vector<std::string>& items, size_t most) {
        std::string out;
        for (size_t k = 0; k < items.size() && k < most; k++)
            out += (k ? ", " : "") + items[k];
        if (items.size() > most)
            out += std::format(" and {} more", items.size() - most);
        return out;
    }
}

void CustomModels::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;
    i.wake.notify_all();
}

bool CustomModels::IsAvailable() {
    auto& i = GetInstance();
    return Engine::IsInsecure() && offsets::models::fnPrecache && offsets::skins::setModel && !i.resolve_failed;
}

// game/ of the install, from the libraries Steam lists (only its files are read)
static std::filesystem::path GameFromSteam() {
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS)
        return {};

    std::vector<std::filesystem::path> libraries{ steam };
    std::ifstream f(std::filesystem::path(steam) / "steamapps" / "libraryfolders.vdf");
    for (std::string line; std::getline(f, line);) {
        // "path"		"D:\\Games"
        auto key = line.find("\"path\"");
        if (key == std::string::npos)
            continue;
        auto start = line.find('"', key + 6);
        auto end = start == std::string::npos ? start : line.find('"', start + 1);
        if (end == std::string::npos)
            continue;

        std::string path;
        for (size_t k = start + 1; k < end; k++) {
            if (line[k] == '\\' && k + 1 < end && line[k + 1] == '\\')
                k++;
            path += line[k];
        }
        libraries.push_back(FromUtf8(path));
    }

    std::error_code ec;
    for (const auto& library : libraries) {
        auto game = library / "steamapps" / "common" / "Counter-Strike Global Offensive" / "game";
        if (std::filesystem::is_directory(game / "csgo", ec))
            return game;
    }
    return {};
}

std::filesystem::path CustomModels::Folder() {
    auto& i = GetInstance();

    std::lock_guard lock(i.mutex);
    if (i.game_dir.empty()) {
        // game/bin/win64/cs2.exe -> game
        wchar_t image[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (auto p = Engine::GetProcess(); p && QueryFullProcessImageNameW(p->handle_, 0, image, &size))
            i.game_dir = std::filesystem::path(image).parent_path().parent_path().parent_path();
        else
            i.game_dir = GameFromSteam();

        if (i.game_dir.empty())
            return {};
    }

    return i.game_dir / "csgo" / "characters" / "models";
}

CustomModel CustomModels::CheckData(const std::vector<uint8_t>& file, const std::string& resource,
    const std::function<bool(const std::string&)>& has_file) {
    auto& i = GetInstance();
    Folder();   // The game folder, for its files

    CustomModel model;
    model.resource = Normalize(resource);
    model.name = model.resource.substr(model.resource.find_last_of('/') + 1);
    if (model.name.ends_with(".vmdl"))
        model.name.resize(model.name.size() - 5);
    model.status = CustomModel::Status::BAD;

    i.Evaluate(model, file, has_file);
    return model;
}

std::vector<CustomModel> CustomModels::List() {
    auto& i = GetInstance();

    if (!i.started.exchange(true))
        std::thread(&CustomModels::ScanThread, &i).detach();

    std::lock_guard lock(i.mutex);
    return i.models;
}

void CustomModels::Rescan() {
    auto& i = GetInstance();
    {
        std::lock_guard lock(i.mutex);
        i.rescan = true;
    }
    i.wake.notify_all();

    // A model that failed to load gets another try, the files might be fixed
    std::lock_guard lock(i.load_mutex);
    for (auto& [resource, load] : i.loads)
        if (load.state == Load::State::FAILED)
            load = {};
}

void CustomModels::ScanThread() {
    while (!this->stopping) {
        Scan();

        std::unique_lock lock(this->mutex);
        this->wake.wait_for(lock, SCAN_INTERVAL, [&] { return this->stopping.load() || this->rescan; });
        this->rescan = false;
    }
}

void CustomModels::Scan() {
    auto folder = Folder();

    std::vector<CustomModel> found;
    std::error_code ec;

    auto csgo = folder.empty() ? folder : folder.parent_path().parent_path();
    for (auto root : MODEL_FOLDERS) {
        if (csgo.empty() || !std::filesystem::is_directory(csgo / root, ec))
            continue;

        auto options = std::filesystem::directory_options::skip_permission_denied;
        for (auto it = std::filesystem::recursive_directory_iterator(csgo / root, options, ec);
             !ec && it != std::filesystem::recursive_directory_iterator() && found.size() < MAX_MODELS; it.increment(ec)) {
            if (this->stopping)
                return;

            std::error_code entry_ec;
            if (!it->is_regular_file(entry_ec) || Lower(Utf8(it->path().extension())) != ".vmdl_c")
                continue;

            auto resource = Normalize(Utf8(std::filesystem::relative(it->path(), csgo, entry_ec)));
            if (entry_ec)
                continue;

            auto size = it->file_size(entry_ec);
            auto time = it->last_write_time(entry_ec).time_since_epoch().count();

            // Checked once, again when the file changes
            auto cached = this->checked.find(resource);
            if (cached == this->checked.end() || cached->second.size != size || cached->second.time != time) {
                Checked entry{ size, time, Check(it->path(), resource) };
                cached = this->checked.insert_or_assign(resource, std::move(entry)).first;
            }

            found.push_back(cached->second.model);
        }
        ec.clear();
    }

    // Packs come with extras next to the model: a copy without hitboxes ("x_nohitbox", "no_hitbox") and models of
    // only the arms, which can't be used. With a usable model in the folder only that one is listed. A folder of only
    // extras keeps them, and a misplaced one keeps its Move button
    auto folder_of = [](const std::string& resource) { return resource.substr(0, resource.find_last_of('/') + 1); };
    auto is_hitbox_copy = [](const CustomModel& model) { return Lower(model.name).find("hitbox") != std::string::npos; };

    std::unordered_set<std::string> has_main;   // Folders with a usable model that is no hitbox copy
    for (const auto& model : found)
        if (model.status != CustomModel::Status::BAD && !is_hitbox_copy(model))
            has_main.insert(folder_of(model.resource));

    std::erase_if(found, [&](const CustomModel& model) {
        if (!has_main.contains(folder_of(model.resource)))
            return false;
        if (model.status == CustomModel::Status::BAD)
            return !model.fits_there;
        return is_hitbox_copy(model);
    });

    std::sort(found.begin(), found.end(), [](const CustomModel& a, const CustomModel& b) { return Lower(a.name) < Lower(b.name); });

    std::lock_guard lock(this->mutex);
    this->models = std::move(found);
}

CustomModel CustomModels::Check(const std::filesystem::path& file, const std::string& resource) {
    CustomModel model;
    model.resource = resource;
    model.name = Utf8(file.stem());     // "x.vmdl_c" -> "x"
    model.file = file;
    model.status = CustomModel::Status::BAD;

    if (resource.size() >= MAX_RESOURCE_NAME) {
        model.note = "Its path is too long for the game, move it to a shorter folder";
        return model;
    }

    // Model files are small, the whole of it (its meshes are looked at too)
    std::error_code ec;
    auto size = std::filesystem::file_size(file, ec);
    std::vector<uint8_t> bytes;
    if (!ec && size <= MAX_BLOCK_SIZE) {
        std::ifstream f(file, std::ios::binary);
        bytes.resize(static_cast<size_t>(size));
        if (!f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            bytes.clear();
    }

    Evaluate(model, bytes, nullptr);
    return model;
}

// What the model is made for: where it belongs, the animations, its bones, the files it names
void CustomModels::Evaluate(CustomModel& model, const std::vector<uint8_t>& file,
    const std::function<bool(const std::string&)>& has_file) {
    const auto& resource = model.resource;
    model.status = CustomModel::Status::BAD;

    auto data = Resource::Block(file, "DATA");
    auto found_rerl = Resource::Block(file, "RERL");
    auto rerl = found_rerl ? *found_rerl : std::vector<uint8_t>{};

    Resource::Value root;
    if (!data || !Resource::ParseKV3(*data, root)) {
        model.note = "Not a model this program can read (not compiled for CS2, or a format it does not know)";
        return;
    }

    // The name it was compiled with is where the game looks for it & its materials, a model moved elsewhere is
    // not found by them (the game hands out another model, its materials show as a checkerboard)
    auto compiled = Normalize(root["m_name"].s);
    bool misplaced = !compiled.empty() && compiled != resource;
    if (misplaced) {
        model.place = compiled.substr(0, compiled.find_last_of('/') + 1);
        model.made_for = compiled;
    }

    // The rest as if it were there. Misplaced, it is not usable until moved, whatever the rest says
    struct Placed {
        CustomModel& model;
        bool misplaced;
        ~Placed() {
            if (!misplaced)
                return;
            model.fits_there = model.status != CustomModel::Status::BAD;
            if (model.fits_there) {
                model.status = CustomModel::Status::BAD;
                model.note = std::format("It was made for csgo/{}, the game only finds it & its materials there. Move puts its folder there", model.place);
            }
        }
    } placed{ model, misplaced };

    auto references = Resource::References(rerl);

    // Made for the animations of CS2 before 2024 (the old animation graph), they stand in a T-pose now
    bool new_graph = std::any_of(references.begin(), references.end(), [](const std::string& r) { return Normalize(r) == PLAYER_GRAPH; });
    bool old_graph = std::any_of(references.begin(), references.end(), [](const std::string& r) { return Normalize(r).ends_with(".vanmgrph"); });
    if (!new_graph && old_graph) {
        model.note = "Made for the animations CS2 had before 2024, it would stand in a T-pose. Look for an updated version of it";
        return;
    }

    // First person hands: the agents of the game have them in "firstperson_default" (the 3rd group), models of the
    // community have their own groups ("first_person_arms"). Not the one without hands for gloves to go over
    {
        const auto& groups = root["m_meshGroups"].items;
        int best = -1, best_score = 0;
        for (size_t k = 0; k < groups.size() && k < 64; k++) {
            // "first_or_third_person_@2_#&firstperson_default": the choice after "#&"
            auto name = Lower(groups[k].s);
            if (auto choice = name.find("#&"); choice != std::string::npos)
                name = name.substr(choice + 2);
            if (name.find("first") == std::string::npos || name.find("hide") != std::string::npos)
                continue;

            int score = 1 + (name.find("default") != std::string::npos || name.find("arm") != std::string::npos || name.find("hand") != std::string::npos);
            if (score > best_score) {
                best = static_cast<int>(k);
                best_score = score;
            }
        }
        model.hands_mask = best >= 0 ? 1ull << best : 0;
        model.empty_mask = groups.size() < 64 ? 1ull << groups.size() : 0;

        // Some models put the whole body in that group (their hands were a model of another pack): shown in first
        // person it fills the view. Hands are a third of a player high at most, then the game's view stays
        Resource::Value ctrl;
        auto ctrl_block = Resource::Block(file, "CTRL");
        if (model.hands_mask && ctrl_block && Resource::ParseKV3(*ctrl_block, ctrl)) {
            const auto& masks = root["m_refMeshGroupMasks"].items;
            for (size_t mesh = 0; mesh < masks.size(); mesh++) {
                if (!(static_cast<uint64_t>(masks[mesh].i) & model.hands_mask))
                    continue;
                if (MeshHeight(file, ctrl, mesh) > MAX_HANDS_HEIGHT) {
                    model.hands_mask = 0;
                    break;
                }
            }
        }
    }

    // Skeleton: the bones of CS2 players, in their order
    std::vector<std::string> bones;
    for (const auto& bone : root["m_modelSkeleton"]["m_boneName"].items)
        bones.push_back(bone.s);
    model.bones = static_cast<int>(bones.size());

    std::vector<std::string> missing;
    bool reordered = false;
    for (const auto& bone : REQUIRED_BONES) {
        auto it = std::find_if(bones.begin(), bones.end(), [&](const std::string& name) { return Lower(name) == Lower(bone.name); });
        if (it == bones.end())
            missing.push_back(bone.name);
        else if (it - bones.begin() != bone.index)
            reordered = true;
    }

    if (!missing.empty()) {
        model.note = std::format("Not rigged for CS2 players, it lacks the bones {}. It would stand in a T-pose or crash the game", Join(missing, 4));
        return;
    }

    // Animations: the player skeleton & graph of the game
    std::vector<std::string> animation = references;
    for (const auto& skeleton : root["m_vecNmSkeletonRefs"].items)
        animation.push_back(skeleton.s);
    for (const auto& graph : root["m_animGraph2Refs"].items)
        animation.push_back(graph["m_hGraph"].s);

    auto refers_to = [&](const char* wanted) {
        return std::any_of(animation.begin(), animation.end(), [&](const std::string& name) { return Normalize(name) == wanted; });
    };

    if (!refers_to(PLAYER_SKELETON) || !refers_to(PLAYER_GRAPH)) {
        model.note = "Not made for the animations of CS2 players (no player skeleton or animation graph). It would stand in a T-pose or crash the game";
        return;
    }

    // Files it needs: its meshes & animations missing make an ERROR model. Its look or other models it names (the
    // first person arms of the pack it came from) only show as missing
    std::vector<std::string> broken, looks, others;
    for (const auto& reference : references) {
        auto name = Normalize(reference);
        if (IsOldAnimation(name) || (has_file && has_file(name + "_c")) || Exists(reference))
            continue;
        (IsEssential(name) ? broken : IsLook(name) ? looks : others).push_back(reference);
    }

    if (!broken.empty()) {
        model.note = std::format("Files it needs are not in the game folder: {}. Copy everything that came with the model", Join(broken, 2));
        return;
    }

    std::vector<std::string> notes;
    if (!looks.empty())
        notes.push_back(std::format("{} material(s) missing, those parts show as a checkerboard", looks.size()));
    if (!others.empty())
        notes.push_back(std::format("files of another model it names are missing ({}), not needed to wear it", Join(others, 1)));
    if (reordered)
        notes.push_back("its bones are in another order than the agents of the game, it might move oddly");

    model.status = notes.empty() ? CustomModel::Status::OK : CustomModel::Status::WARNING;
    model.note = notes.empty() ? std::format("Rigged for CS2 players, {} bones", bones.size()) : Join(notes, 3);
    if (!model.note.empty())
        model.note[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(model.note[0])));
}

bool CustomModels::MoveToPlace(const CustomModel& model, std::string& error) {
    auto folder = Folder();
    if (folder.empty() || model.place.empty() || model.file.empty()) {
        error = "The game folder was not found";
        return false;
    }

    auto csgo = folder.parent_path().parent_path();
    auto source = model.file.parent_path();
    auto target = csgo / FromUtf8(model.place);
    std::error_code ec;

    // Only a folder of its own: not csgo/characters/models or the like, which holds other things of the game
    auto relative = std::filesystem::relative(source, csgo, ec);
    if (ec || std::distance(relative.begin(), relative.end()) < 3) {
        error = "Put the model in a folder of its own first (with its materials)";
        return false;
    }

    if (std::filesystem::equivalent(source, target, ec)) {
        error = "It is in its folder already";
        return false;
    }

    std::filesystem::create_directories(target.parent_path(), ec);
    if (!std::filesystem::exists(target, ec)) {
        std::filesystem::rename(source, target, ec);
    }
    else {
        // The folder is there already: its files are put into it, then the old folder goes
        std::filesystem::copy(source, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec)
            std::filesystem::remove_all(source, ec);
    }

    if (ec) {
        error = std::format("Could not move it ({}), is the game using the files?", ec.message());
        return false;
    }

    // Folders left empty above the old one, down to csgo/characters/models & the like
    auto depth = [&](const std::filesystem::path& path) {
        auto r = std::filesystem::relative(path, csgo, ec);
        return ec ? 0 : std::distance(r.begin(), r.end());
    };
    for (auto parent = source.parent_path(); depth(parent) > 2 && std::filesystem::is_empty(parent, ec) && !ec; parent = parent.parent_path())
        std::filesystem::remove(parent, ec);

    LOGF(INFO, "Custom model: {} moved to csgo/{}", Utf8(source), model.place);
    Rescan();
    return true;
}

bool CustomModels::Exists(const std::string& reference) {
    auto name = Normalize(reference) + "_c";

    std::lock_guard lock(this->package_mutex);
    if (this->game_dir.empty())
        return false;

    // Loose files of the game, where the model is too
    std::error_code ec;
    for (auto search : { "csgo", "core" })
        if (std::filesystem::exists(this->game_dir / search / FromUtf8(name), ec))
            return true;

    IndexPackages();
    return this->packaged.contains(Hash(name));
}

// The file names of the vpks of the game (the search paths of gameinfo.gi: csgo & core), hashed
void CustomModels::IndexPackages() {
    if (this->indexed)
        return;
    this->indexed = true;

    for (auto search : { "csgo", "core" }) {
        std::ifstream f(this->game_dir / search / "pak01_dir.vpk", std::ios::binary);
        if (!f.good())
            continue;

        uint32_t header[3]{};
        f.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!f || header[0] != 0x55AA1234)
            continue;

        if (header[1] == 2)
            f.seekg(16, std::ios::cur);

        std::vector<char> tree(header[2]);
        f.read(tree.data(), tree.size());
        if (!f)
            continue;

        size_t p = 0;
        auto read_string = [&]() {
            std::string out;
            while (p < tree.size() && tree[p])
                out += tree[p++];
            p++;
            return out;
        };

        for (std::string extension; p < tree.size() && !(extension = read_string()).empty();) {
            for (std::string directory; p < tree.size() && !(directory = read_string()).empty();) {
                for (std::string name; p < tree.size() && !(name = read_string()).empty();) {
                    if (p + 18 > tree.size())
                        break;

                    uint16_t preload;
                    memcpy(&preload, &tree[p + 4], 2);
                    p += 18 + preload;

                    auto path = (directory == " " ? std::string() : directory + "/") + name + "." + extension;
                    this->packaged.insert(Hash(Lower(path)));
                }
            }
        }
    }

    LOGF(VERBOSE, "Custom models: {} files of the game indexed", this->packaged.size());
}

bool CustomModels::Prepare(const std::string& resource) {
    auto& i = GetInstance();
    if (resource.empty() || !IsAvailable())
        return false;

    // Only a model the check let through
    auto models = List();
    auto model = std::find_if(models.begin(), models.end(), [&](const CustomModel& m) { return m.resource == resource; });
    if (model == models.end() || model->status == CustomModel::Status::BAD)
        return false;

    auto snapshot = Cache::Current();
    if (!snapshot->globals.in_match)
        return false;
    std::string map(snapshot->globals.map_name, strnlen(snapshot->globals.map_name, sizeof(snapshot->globals.map_name)));

    Load load;
    {
        std::lock_guard lock(i.load_mutex);
        load = i.loads[resource];
    }

    // A new map drops what the old one loaded
    if (load.map != map) {
        load = {};
        load.map = map;
    }

    auto now = std::chrono::steady_clock::now();
    auto store = [&]() {
        std::lock_guard lock(i.load_mutex);
        i.loads[resource] = load;
    };

    if (load.state == Load::State::FAILED) {
        store();
        return false;
    }

    // Loaded: looked at again now & then, a dropped model is loaded again
    if (load.state == Load::State::READY) {
        if (now < load.next_check) {
            store();
            return true;
        }

        bool wrong = false;
        if (i.IsLoaded(resource, wrong)) {
            load.next_check = now + READY_RECHECK;
            store();
            return true;
        }

        load.state = Load::State::IDLE;
    }

    if (load.state == Load::State::IDLE) {
        if (!i.Resolve()) {
            store();
            return false;
        }

        LOGF(INFO, "Custom model: the game loads {}", resource);
        if (!i.CallPrecache(resource)) {
            LOGF(WARNING, "Custom model: the game did not take the load of {}, tried again later", resource);
            store();
            return false;
        }

        load.state = Load::State::LOADING;
        load.started = now;
        load.next_check = {};
    }

    // Loading: is it in memory yet
    if (now >= load.next_check) {
        load.next_check = now + LOAD_POLL;

        bool wrong = false;
        if (i.IsLoaded(resource, wrong)) {
            LOGF(INFO, "Custom model: {} is loaded ({} ms), it is set on the next spawn", resource,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - load.started).count());
            load.state = Load::State::READY;
            load.next_check = now + READY_RECHECK;
        }
        else if (wrong) {
            LOGF(WARNING, "Custom model: the game could not load {} (it gave another model), the agent is used instead", resource);
            load.state = Load::State::FAILED;
        }
        else if (now - load.started > LOAD_TIMEOUT) {
            LOGF(WARNING, "Custom model: {} was not loaded after {} s, the agent is used instead", resource,
                std::chrono::duration_cast<std::chrono::seconds>(LOAD_TIMEOUT).count());
            load.state = Load::State::FAILED;
        }
    }

    store();
    return load.state == Load::State::READY;
}

bool CustomModels::FirstPerson(const std::string& resource, uint64_t& hands_mask, uint64_t& empty_mask) {
    auto& i = GetInstance();
    auto wanted = Normalize(resource);
    std::lock_guard lock(i.mutex);
    auto model = std::find_if(i.models.begin(), i.models.end(), [&](const CustomModel& m) { return m.resource == wanted; });
    if (model == i.models.end())
        return false;

    hands_mask = model->hands_mask;
    empty_mask = model->empty_mask;
    return true;
}

std::string CustomModels::LoadStatus(const std::string& resource) {
    auto& i = GetInstance();
    std::lock_guard lock(i.load_mutex);

    auto it = i.loads.find(resource);
    if (it == i.loads.end())
        return "Loads once you are in a match";

    switch (it->second.state) {
    case Load::State::LOADING:  return "Loading...";
    case Load::State::READY:    return "Ready, on your next spawn";
    case Load::State::FAILED:   return "Could not load, Refresh to retry";
    default:                    return "Loads once you are in a match";
    }
}

// The game code it calls, found from SetModel & the interfaces. Then checked against the model we have on
bool CustomModels::PrecacheResource(const std::string& resource) {
    auto& i = GetInstance();
    if (!IsAvailable())
        return false;
    std::lock_guard lock(i.call_mutex);
    return i.Resolve() && i.CallPrecache(resource);
}

bool CustomModels::Resolve() {
    std::lock_guard lock(this->call_mutex);
    if (this->resolved)
        return true;
    if (this->resolve_failed || !GameThread::Ensure())
        return false;

    // Found already, only the check against our model was waiting for us to be alive
    if (this->set_name) {
        if (!Calibrate())
            return false;
        this->resolved = true;
        return true;
    }

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    auto fail = [&](const char* why) {
        LOGF(WARNING, "Custom models are off: {}", why);
        this->resolve_failed = true;
        return false;
    };

    // SetModel: mov rcx, [GameModelInfo]; lea rdx, [rsp + x]; mov rax, [rcx]; call [rax + GetModel]
    auto set_model = client.base + offsets::skins::setModel;
    uint8_t code[0x20]{};
    p->read_raw(set_model, code, sizeof(code));

    if (code[0x0C] != 0x48 || code[0x0D] != 0x8B || code[0x0E] != 0x0D || code[0x13] != 0x48 || code[0x14] != 0x8D ||
        code[0x18] != 0x48 || code[0x19] != 0x8B || code[0x1A] != 0x01 || code[0x1B] != 0xFF)
        return fail("SetModel is not what was expected");

    if (code[0x1C] == 0x50)
        this->get_model_slot = code[0x1D];
    else if (code[0x1C] == 0x90)
        this->get_model_slot = *reinterpret_cast<uint32_t*>(code + 0x1D);
    else
        return fail("SetModel is not what was expected");

    auto model_info_global = set_model + 0x13 + *reinterpret_cast<int32_t*>(code + 0x0F);
    this->model_info = p->read<uintptr_t>(model_info_global);
    auto get_model = this->model_info ? p->read<uintptr_t>(p->read<uintptr_t>(this->model_info) + this->get_model_slot) : 0;
    if (!get_model)
        return fail("the model info of the game was not found");

    // GetModel: the name into a CResourceNameTyped of 200 bytes (flags 0xC00000C8) by CResourceName::Set, the first
    // call, then a check that it is a 'vmdl'
    uint8_t body[0x80]{};
    p->read_raw(get_model, body, sizeof(body));

    constexpr uint8_t flags[] = { 0xC8, 0x00, 0x00, 0xC0 };
    constexpr uint8_t vmdl[] = { 0xBA, 0x76, 0x6D, 0x64, 0x6C };    // mov edx, 'vmdl'
    auto has = [&](const uint8_t* bytes, size_t size) { return std::search(body, body + sizeof(body), bytes, bytes + size) != body + sizeof(body); };

    constexpr uint8_t after_call[] = { 0xF7, 0x44, 0x24, 0x20, 0xFF, 0xFF, 0xFF, 0x3F };
    const uint8_t* call = nullptr;
    for (size_t k = 0; k + 5 + sizeof(after_call) <= sizeof(body) && !call; k++)
        if (body[k] == 0xE8 && std::equal(std::begin(after_call), std::end(after_call), body + k + 5))
            call = body + k;

    if (!has(flags, sizeof(flags)) || !has(vmdl, sizeof(vmdl)) || !call)
        return fail("GetModel is not what was expected");

    this->set_name = get_model + (call - body) + 5 + *reinterpret_cast<const int32_t*>(call + 1);

    // The precache, a vfunc of the resource system
    auto resources = p->GetModule("resourcesystem.dll");
    this->resource_system = p->FindInterface("resourcesystem.dll", "ResourceSystem013");
    if (!resources.base || !this->resource_system)
        return fail("the resource system was not found");

    auto precache = resources.base + offsets::models::fnPrecache;
    auto table = p->read<uintptr_t>(this->resource_system);
    for (size_t slot = 0; slot < 256 && !this->precache_slot; slot++)
        if (p->read<uintptr_t>(table + slot * 8) == precache)
            this->precache_slot = slot * 8;

    if (!this->precache_slot)
        return fail("the precache is not in the resource system");

    // Waits for us to be alive, unless it found the game different
    if (!Calibrate())
        return false;

    this->resolved = true;
    LOGF(INFO, "Custom models can be loaded by the game (precache slot 0x{:X}, model name at 0x{:X})", this->precache_slot, this->name_offset);
    return true;
}

// Our code & its data in the game: the precache & GetModel with the name & path they take
bool CustomModels::BuildPage() {
    if (this->page)
        return true;

    auto p = Engine::GetProcess();
    this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return false;

    std::vector<uint8_t> page(PAGE_SIZE, 0);
    std::vector<uint8_t> stub;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { stub.insert(stub.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int k = 0; k < 4; k++) stub.push_back(static_cast<uint8_t>(value >> (k * 8))); };
    auto emit64 = [&](uint64_t value) { for (int k = 0; k < 8; k++) stub.push_back(static_cast<uint8_t>(value >> (k * 8))); };
    auto place = [&](size_t at) { std::copy(stub.begin(), stub.end(), page.begin() + at); stub.clear(); };

    // if (CResourceName::Set(&name, path)) resource_system->PreCache(&name, "")
    emit({ 0x48, 0x83, 0xEC, 0x28 });                                   // sub rsp, 0x28
    emit({ 0x48, 0xB9 }); emit64(this->page + DATA_NAME);               // mov rcx, name
    emit({ 0x48, 0xBA }); emit64(this->page + DATA_PATH);               // mov rdx, path
    emit({ 0x48, 0xB8 }); emit64(this->set_name);                       // mov rax, CResourceName::Set
    emit({ 0xFF, 0xD0 });                                               // call rax
    emit({ 0x84, 0xC0 });                                               // test al, al
    emit({ 0x74, 0x27 });                                               // je done
    emit({ 0x48, 0xB9 }); emit64(this->resource_system);                // mov rcx, resource system
    emit({ 0x48, 0xBA }); emit64(this->page + DATA_NAME);               // mov rdx, name
    emit({ 0x49, 0xB8 }); emit64(this->page + DATA_REASON);             // mov r8, ""
    emit({ 0x48, 0x8B, 0x01 });                                         // mov rax, [rcx]
    emit({ 0xFF, 0x90 }); emit32(static_cast<uint32_t>(this->precache_slot)); // call [rax + PreCache]
    emit({ 0x48, 0x83, 0xC4, 0x28 });                                   // done: add rsp, 0x28
    emit({ 0xC3 });                                                     // ret
    place(CODE_PRECACHE);

    // model_info->GetModel(&out, path)
    emit({ 0x48, 0x83, 0xEC, 0x28 });                                   // sub rsp, 0x28
    emit({ 0x48, 0xB9 }); emit64(this->model_info);                     // mov rcx, model info
    emit({ 0x48, 0xBA }); emit64(this->page + DATA_OUT);                // mov rdx, out
    emit({ 0x49, 0xB8 }); emit64(this->page + DATA_PATH);               // mov r8, path
    emit({ 0x48, 0x8B, 0x01 });                                         // mov rax, [rcx]
    emit({ 0xFF, 0x90 }); emit32(static_cast<uint32_t>(this->get_model_slot)); // call [rax + GetModel]
    emit({ 0x48, 0x83, 0xC4, 0x28 });                                   // add rsp, 0x28
    emit({ 0xC3 });                                                     // ret
    place(CODE_GET_MODEL);

    p->write_bytes(this->page, page);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), PAGE_SIZE);
    return true;
}

// Our own model, which the game surely has: GetModel must give the handle our pawn holds, its data must be there.
// The name of the model inside it tells later whether the game loaded the model asked for or put the ERROR one
bool CustomModels::Calibrate() {
    auto p = Engine::GetProcess();

    auto pawn = Engine::GetLocalPawn();
    auto node = pawn ? p->read<uintptr_t>(pawn + offsets::pawn::m_pGameSceneNode) : 0;
    if (!node)
        return false;   // Not alive yet, tried again

    auto state = node + offsets::bone::m_modelState;
    auto handle = p->read<uintptr_t>(state + offsets::econ::m_hModel);
    auto name_pointer = p->read<uintptr_t>(state + offsets::econ::m_ModelName);

    char name[260]{};
    if (!handle || !name_pointer || !p->read_raw(name_pointer, name, sizeof(name) - 1) || !name[0])
        return false;

    uintptr_t binding = 0;
    if (!CallGetModel(name, binding))
        return false;

    if (binding != handle) {
        LOGF(WARNING, "Custom models are off: the model handle of the game is not what was expected (0x{:X} for 0x{:X})", binding, handle);
        this->resolve_failed = true;
        return false;
    }

    auto data = p->read<uintptr_t>(binding);
    if (!data) {
        LOGF(WARNING, "Custom models are off: the loaded model of the game was not found");
        this->resolve_failed = true;
        return false;
    }

    auto wanted = Normalize(name);
    for (std::ptrdiff_t offset = 0; offset < 0x100; offset += 8) {
        auto text = p->read<uintptr_t>(data + offset);
        char found[260]{};
        if (text && p->read_raw(text, found, sizeof(found) - 1) && Normalize(found) == wanted) {
            this->name_offset = offset;
            break;
        }
    }

    if (this->name_offset < 0)
        LOGF(VERBOSE, "Custom models: the model name was not found in the model, an ERROR model can't be told apart");

    return true;
}

bool CustomModels::CallPrecache(const std::string& resource) {
    std::lock_guard lock(this->call_mutex);
    auto p = Engine::GetProcess();
    if (!BuildPage())
        return false;

    // A fresh name each time, the game fills it
    std::vector<uint8_t> name(NAME_SIZE, 0);
    memcpy(name.data() + 4, &NAME_FLAGS, 4);
    p->write_bytes(this->page + DATA_NAME, name);

    std::vector<uint8_t> path(PATH_SIZE, 0);
    memcpy(path.data(), resource.c_str(), std::min(resource.size(), PATH_SIZE - 1));
    p->write_bytes(this->page + DATA_PATH, path);

    if (!GameThread::Call(this->page + CODE_PRECACHE, PRECACHE_TIMEOUT_MS)) {
        this->page = 0;     // The game may still be in it, the next call gets a page of its own
        return false;
    }
    return true;
}

bool CustomModels::CallGetModel(const std::string& resource, uintptr_t& binding) {
    std::lock_guard lock(this->call_mutex);
    auto p = Engine::GetProcess();
    if (!BuildPage())
        return false;

    std::vector<uint8_t> path(PATH_SIZE, 0);
    memcpy(path.data(), resource.c_str(), std::min(resource.size(), PATH_SIZE - 1));
    p->write_bytes(this->page + DATA_PATH, path);
    p->write<uintptr_t>(this->page + DATA_OUT, 0);

    if (!GameThread::Call(this->page + CODE_GET_MODEL, 1000)) {
        this->page = 0;
        return false;
    }

    binding = p->read<uintptr_t>(this->page + DATA_OUT);
    return true;
}

bool CustomModels::IsLoaded(const std::string& resource, bool& wrong_model) {
    wrong_model = false;

    uintptr_t binding = 0;
    if (!CallGetModel(resource, binding) || !binding)
        return false;

    auto p = Engine::GetProcess();
    auto data = p->read<uintptr_t>(binding);
    if (!data)
        return false;

    if (this->name_offset >= 0) {
        char name[260]{};
        auto text = p->read<uintptr_t>(data + this->name_offset);
        if (!text || !p->read_raw(text, name, sizeof(name) - 1))
            return false;

        if (Normalize(name) != Normalize(resource)) {
            wrong_model = true;
            return false;
        }
    }

    return true;
}
