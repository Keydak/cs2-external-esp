#include "MapExport.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/resource/Resource.hpp"

namespace {
    using Resource::Value;

    constexpr uint32_t VPK_SIGNATURE = 0x55AA1234;
    constexpr char TRI_MAGIC[4] = { 'C', 'S', '2', 'T' };
    constexpr uint32_t TRI_VERSION = 2;

    // Collision layers grenades & bullets of sight pass through
    const char* IGNORED_LAYERS[] = { "playerclip", "npcclip", "sky" };

    // Single file inside a vpk that is not split into archives (maps are not)
    std::optional<std::vector<uint8_t>> ReadVpkEntry(const std::filesystem::path& path, const std::string& wanted) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
            return std::nullopt;

        uint32_t header[3]{};
        f.read(reinterpret_cast<char*>(header), sizeof(header));
        if (header[0] != VPK_SIGNATURE)
            return std::nullopt;

        uint32_t version = header[1], tree_size = header[2];
        uint32_t header_size = 12;
        if (version == 2) {
            f.seekg(16, std::ios::cur);
            header_size += 16;
        }

        std::vector<char> tree(tree_size);
        f.read(tree.data(), tree_size);
        if (!f)
            return std::nullopt;

        size_t p = 0;
        auto read_string = [&]() {
            std::string out;
            while (p < tree.size() && tree[p])
                out += tree[p++];
            p++;
            return out;
        };

        std::optional<std::pair<uint32_t, uint32_t>> found;

        for (std::string extension; p < tree.size() && !(extension = read_string()).empty();) {
            for (std::string directory; p < tree.size() && !(directory = read_string()).empty();) {
                for (std::string name; p < tree.size() && !(name = read_string()).empty();) {
                    if (p + 18 > tree.size())
                        return std::nullopt;

                    uint16_t preload;
                    uint32_t offset, length;
                    memcpy(&preload, &tree[p + 4], 2);
                    memcpy(&offset, &tree[p + 8], 4);
                    memcpy(&length, &tree[p + 12], 4);
                    p += 18 + preload;

                    if (directory + "/" + name + "." + extension == wanted)
                        found = { offset, length };
                }
            }
        }

        if (!found)
            return std::nullopt;

        std::vector<uint8_t> data(found->second);
        f.seekg(static_cast<std::streamoff>(header_size) + tree_size + found->first);
        f.read(reinterpret_cast<char*>(data.data()), data.size());

        if (!f)
            return std::nullopt;

        return data;
    }

    struct Triangle {
        float v[9];
    };

    bool BlocksSight(const Value& attribute) {
        // Small props players walk through (hanging signs, boxes on walls): grenades fly through them too
        for (const auto& layer : attribute["m_InteractExcludeStrings"].items) {
            auto name = layer.s;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "player")
                return false;
        }

        for (const auto& layer : attribute["m_InteractAsStrings"].items) {
            auto name = layer.s;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            for (auto ignored : IGNORED_LAYERS)
                if (name == ignored)
                    return false;
        }

        return true;
    }

    // Every face of a half edge convex hull, as a fan
    void HullTriangles(const Value& hull, std::vector<Triangle>& out) {
        const auto& positions = hull["m_VertexPositions"].s;
        const auto& edges = hull["m_Edges"].s;
        const auto& faces = hull["m_Faces"].s;

        size_t vertex_count = positions.size() / 12;
        auto vertex = [&](size_t index, float* to) {
            if (index < vertex_count)
                memcpy(to, positions.data() + index * 12, 12);
        };

        // Half edge: next, twin, origin, face (a byte each)
        size_t edge_count = edges.size() / 4;
        std::vector<uint8_t> polygon;

        for (unsigned char start : faces) {
            polygon.clear();
            size_t edge = start;

            for (size_t step = 0; step < edge_count && edge < edge_count; step++) {
                polygon.push_back(static_cast<uint8_t>(edges[edge * 4 + 2]));
                edge = static_cast<unsigned char>(edges[edge * 4]);
                if (edge == start)
                    break;
            }

            for (size_t i = 1; i + 1 < polygon.size(); i++) {
                Triangle t{};
                vertex(polygon[0], t.v);
                vertex(polygon[i], t.v + 3);
                vertex(polygon[i + 1], t.v + 6);
                out.push_back(t);
            }
        }
    }

    void MeshTriangles(const Value& mesh, std::vector<Triangle>& out) {
        const auto& vertices = mesh["m_Vertices"].s;
        const auto& triangles = mesh["m_Triangles"].s;

        size_t vertex_count = vertices.size() / 12;

        for (size_t i = 0; i + 12 <= triangles.size(); i += 12) {
            int32_t index[3];
            memcpy(index, triangles.data() + i, 12);

            Triangle t{};
            bool valid = true;

            for (int k = 0; k < 3; k++) {
                if (index[k] < 0 || static_cast<size_t>(index[k]) >= vertex_count) {
                    valid = false;
                    break;
                }
                memcpy(t.v + k * 3, vertices.data() + index[k] * 12, 12);
            }

            if (valid)
                out.push_back(t);
        }
    }
}

std::filesystem::path MapExport::FindMapsDir() {
    auto p = Engine::GetProcess();
    if (!p)
        return {};

    // game/bin/win64/cs2.exe -> game/csgo/maps
    wchar_t image[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(p->handle_, 0, image, &size))
        return {};

    auto maps = std::filesystem::path(image).parent_path().parent_path().parent_path() / "csgo" / "maps";

    std::error_code error;
    if (!std::filesystem::is_directory(maps, error))
        return {};

    return maps;
}

bool MapExport::Export(const std::string& name, const std::filesystem::path& out, const std::function<void(float)>& progress) {
    auto report = [&](float value) {
        if (progress)
            progress(value);
    };

    auto maps = FindMapsDir();
    if (maps.empty()) {
        LOGF(WARNING, "Could not find the maps folder of the game to build the collision of {}", name);
        return false;
    }

    auto started = std::chrono::steady_clock::now();

    auto resource = ReadVpkEntry(maps / (name + ".vpk"), "maps/" + name + "/world_physics.vmdl_c");
    if (!resource) {
        LOGF(WARNING, "{} has no world physics", name);
        return false;
    }

    report(0.3f);

    auto phys = Resource::Block(*resource, "PHYS");
    resource.reset();

    if (!phys) {
        LOGF(WARNING, "The world physics of {} has no PHYS block", name);
        return false;
    }

    Value root;
    if (!Resource::ParseKV3(*phys, root)) {
        LOGF(WARNING, "Could not read the world physics of {}", name);
        return false;
    }

    report(0.6f);

    const auto& attributes = root["m_collisionAttributes"].items;
    auto blocks = [&](const Value& shape) {
        auto index = shape["m_nCollisionAttributeIndex"].i;
        return index < 0 || static_cast<size_t>(index) >= attributes.size() || BlocksSight(attributes[index]);
    };

    std::vector<Triangle> triangles;
    size_t skipped = 0;

    for (const auto& part : root["m_parts"].items) {
        const auto& shape = part["m_rnShape"];

        for (const auto& hull : shape["m_hulls"].items) {
            if (blocks(hull))
                HullTriangles(hull["m_Hull"], triangles);
            else
                skipped++;
        }

        for (const auto& mesh : shape["m_meshes"].items) {
            if (blocks(mesh))
                MeshTriangles(mesh["m_Mesh"], triangles);
            else
                skipped++;
        }
    }

    if (triangles.empty()) {
        LOGF(WARNING, "The world physics of {} has no triangles", name);
        return false;
    }

    report(0.75f);

    std::error_code error;
    std::filesystem::create_directories(out.parent_path(), error);

    // Written next to it first, a half written file would be read as a broken map
    auto temp = out;
    temp += ".tmp";

    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            LOGF(WARNING, "Could not write {}", temp.string());
            return false;
        }

        uint32_t version = TRI_VERSION, count = static_cast<uint32_t>(triangles.size());
        f.write(TRI_MAGIC, sizeof(TRI_MAGIC));
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));
        f.write(reinterpret_cast<const char*>(triangles.data()), triangles.size() * sizeof(Triangle));

        if (!f.good())
            return false;
    }

    std::filesystem::rename(temp, out, error);
    if (error) {
        LOGF(WARNING, "Could not write {}: {}", out.string(), error.message());
        return false;
    }

    report(0.85f);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    LOGF(INFO, "Built the collision of {} from the game files ({} triangles, {} non solid shapes skipped, {}ms)",
        name, triangles.size(), skipped, ms);

    return true;
}
