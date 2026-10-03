#include "MapExport.hpp"

#include "core/engine/Engine.hpp"

#include <external/zstd/zstd.h>

namespace {
    constexpr uint32_t VPK_SIGNATURE = 0x55AA1234;
    constexpr char TRI_MAGIC[4] = { 'C', 'S', '2', 'T' };
    constexpr uint32_t TRI_VERSION = 1;

    // Collision layers grenades & bullets of sight pass through
    const char* IGNORED_LAYERS[] = { "playerclip", "npcclip", "sky" };

    template<class T>
    T Read(const std::vector<uint8_t>& data, size_t offset) {
        T value{};
        if (offset + sizeof(T) <= data.size())
            memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }

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

    // PHYS block of a compiled Source 2 resource
    std::optional<std::vector<uint8_t>> PhysicsBlock(const std::vector<uint8_t>& resource) {
        auto block_offset = Read<uint32_t>(resource, 8);
        auto block_count = Read<uint32_t>(resource, 12);
        size_t base = 8 + block_offset;

        for (uint32_t i = 0; i < block_count; i++) {
            size_t entry = base + i * 12;
            if (entry + 12 > resource.size())
                break;

            if (memcmp(resource.data() + entry, "PHYS", 4) != 0)
                continue;

            size_t start = entry + 4 + Read<uint32_t>(resource, entry + 4);
            size_t size = Read<uint32_t>(resource, entry + 8);
            if (start + size > resource.size())
                return std::nullopt;

            return std::vector<uint8_t>(resource.begin() + start, resource.begin() + start + size);
        }

        return std::nullopt;
    }

    // Binary KeyValues3, version 5 with zstd, only what the physics data needs
    struct Value {
        enum Type { NONE, BOOL, INT, FLOAT, STRING, BLOB, ARRAY, OBJECT } type = NONE;
        int64_t i = 0;
        double d = 0.0;
        std::string s;                  // Strings & blobs
        std::vector<Value> items;       // Arrays & object values
        std::vector<std::string> keys;  // Objects

        const Value* Get(const std::string& key) const {
            for (size_t k = 0; k < keys.size(); k++)
                if (keys[k] == key)
                    return &items[k];
            return nullptr;
        }

        const Value& operator[](const std::string& key) const {
            static const Value empty;
            auto value = Get(key);
            return value ? *value : empty;
        }
    };

    class KV3 {
    public:
        bool Parse(const std::vector<uint8_t>& data, Value& root);

    private:
        // Bytes, shorts, ints & 8 byte values, each section aligned to its size
        struct Buffer {
            const std::vector<uint8_t>* data = nullptr;
            size_t p1 = 0, p2 = 0, p4 = 0, p8 = 0, end = 0;

            void Init(const std::vector<uint8_t>& d, size_t offset, uint32_t b1, uint32_t b2, uint32_t b4, uint32_t b8) {
                data = &d;
                size_t p = offset;
                p1 = p; p += b1;
                p = (p + 1) & ~size_t(1);
                p2 = p; p += b2 * 2;
                p = (p + 3) & ~size_t(3);
                p4 = p; p += b4 * 4;
                p = (p + 7) & ~size_t(7);
                p8 = p; p += b8 * 8;
                end = p;
            }

            template<class T> T Take(size_t& p) { T v = Read<T>(*data, p); p += sizeof(T); return v; }
            uint8_t U8() { return Take<uint8_t>(p1); }
            int8_t I8() { return Take<int8_t>(p1); }
            int16_t I16() { return Take<int16_t>(p2); }
            uint16_t U16() { return Take<uint16_t>(p2); }
            int32_t I32() { return Take<int32_t>(p4); }
            uint32_t U32() { return Take<uint32_t>(p4); }
            float F32() { return Take<float>(p4); }
            int64_t I64() { return Take<int64_t>(p8); }
            uint64_t U64() { return Take<uint64_t>(p8); }
            double F64() { return Take<double>(p8); }
        };

        enum : uint8_t {
            T_NULL = 1, T_BOOLEAN, T_INT64, T_UINT64, T_DOUBLE, T_STRING, T_BLOB, T_ARRAY, T_OBJECT, T_ARRAY_TYPED,
            T_INT32, T_UINT32, T_TRUE, T_FALSE, T_I64_ZERO, T_I64_ONE, T_D_ZERO, T_D_ONE,
            T_FLOAT, T_INT16, T_UINT16, T_U32_BYTE, T_I32_BYTE, T_ARRAY_BYTE_LEN, T_ARRAY_AUX
        };

        std::vector<uint8_t> buf1, buf2, blobs;
        Buffer aux, main;
        Buffer* current = &main;

        std::vector<std::string> strings;
        std::vector<int32_t> object_lengths;
        std::vector<uint32_t> blob_sizes;
        size_t types = 0, types_end = 0, object_index = 0, blob_index = 0, blob_pos = 0;
        int depth = 0;
        bool failed = false;

        std::pair<uint8_t, uint8_t> ReadType();
        void ParseValue(uint8_t type, uint8_t flag, Value& out);
    };

    bool Decompress(const uint8_t* src, size_t size, size_t capacity, std::vector<uint8_t>& out) {
        out.resize(capacity);
        if (!capacity)
            return true;

        auto result = ZSTD_decompress(out.data(), capacity, src, size);
        if (ZSTD_isError(result))
            return false;

        out.resize(result);
        return true;
    }

    bool KV3::Parse(const std::vector<uint8_t>& data, Value& root) {
        if (data.size() < 120 || memcmp(data.data(), "\x05" "3VK", 4) != 0)
            return false;

        auto u32 = [&](size_t o) { return Read<uint32_t>(data, o); };
        auto u16 = [&](size_t o) { return Read<uint16_t>(data, o); };

        uint32_t compression = u32(20);
        uint32_t b1 = u32(28), b4 = u32(32), b8 = u32(36), count_types = u32(40);
        uint32_t comp_total = u32(52), block_count = u32(56), blob_bytes = u32(60), b2 = u32(64);
        uint32_t unc1 = u32(72), comp1 = u32(76), unc2 = u32(80), comp2 = u32(84);
        uint32_t b1_2 = u32(88), b2_2 = u32(92), b4_2 = u32(96), b8_2 = u32(100);
        uint32_t objs_2 = u32(108);
        (void)u16;

        if (compression != 2)   // zstd
            return false;

        size_t pos = 120;
        if (pos + comp1 + comp2 > data.size())
            return false;

        if (!Decompress(data.data() + pos, comp1, unc1, buf1))
            return false;
        pos += comp1;

        if (!Decompress(data.data() + pos, comp2, unc2, buf2))
            return false;
        pos += comp2;

        // Buffer 1: strings in its bytes, then shorts, ints & doubles
        aux.Init(buf1, 0, b1, b2, b4, b8);
        int32_t string_count = aux.I32();
        if (string_count < 0)
            return false;

        strings.reserve(string_count);
        for (int32_t s = 0; s < string_count; s++) {
            auto start = aux.p1;
            while (aux.p1 < buf1.size() && buf1[aux.p1])
                aux.p1++;

            if (aux.p1 >= buf1.size())
                return false;

            strings.emplace_back(reinterpret_cast<const char*>(buf1.data() + start), aux.p1 - start);
            aux.p1++;
        }

        // Buffer 2: object lengths, the values, types, blob sizes & a trailer
        if (static_cast<size_t>(objs_2) * 4 > buf2.size())
            return false;

        object_lengths.resize(objs_2);
        memcpy(object_lengths.data(), buf2.data(), objs_2 * 4);

        main.Init(buf2, objs_2 * 4, b1_2, b2_2, b4_2, b8_2);

        types = main.end;
        types_end = types + count_types;
        size_t p = types_end;

        if (p + block_count * 4 + 4 > buf2.size())
            return false;

        blob_sizes.resize(block_count);
        memcpy(blob_sizes.data(), buf2.data() + p, block_count * 4);
        p += block_count * 4;

        if (Read<uint32_t>(buf2, p) != 0xFFEEDD00)
            return false;

        if (block_count) {
            size_t blob_compressed = comp_total - comp1 - comp2;
            if (pos + blob_compressed > data.size() || !Decompress(data.data() + pos, blob_compressed, blob_bytes, blobs))
                return false;
        }

        current = &main;
        auto [type, flag] = ReadType();
        ParseValue(type, flag, root);

        return !failed;
    }

    std::pair<uint8_t, uint8_t> KV3::ReadType() {
        if (types >= types_end) {
            failed = true;
            return { T_NULL, 0 };
        }

        uint8_t type = buf2[types++];
        uint8_t flag = 0;

        if (type & 0x80) {
            type &= 0x3F;
            flag = types < types_end ? buf2[types++] : 0;
        }

        return { type, flag };
    }

    void KV3::ParseValue(uint8_t type, uint8_t flag, Value& out) {
        if (failed || depth > 64) {
            failed = true;
            return;
        }

        auto& b = *current;

        auto read_items = [&](size_t count, bool typed) {
            out.type = Value::ARRAY;
            out.items.resize(count);

            std::pair<uint8_t, uint8_t> item_type{};
            if (typed)
                item_type = ReadType();

            depth++;
            for (auto& item : out.items) {
                auto [t, f] = typed ? item_type : ReadType();
                ParseValue(t, f, item);
                if (failed)
                    break;
            }
            depth--;
        };

        switch (type) {
        case T_NULL:        out.type = Value::NONE; return;
        case T_TRUE:        out.type = Value::BOOL; out.i = 1; return;
        case T_FALSE:       out.type = Value::BOOL; out.i = 0; return;
        case T_I64_ZERO:    out.type = Value::INT; out.i = 0; return;
        case T_I64_ONE:     out.type = Value::INT; out.i = 1; return;
        case T_D_ZERO:      out.type = Value::FLOAT; out.d = 0.0; return;
        case T_D_ONE:       out.type = Value::FLOAT; out.d = 1.0; return;
        case T_BOOLEAN:     out.type = Value::BOOL; out.i = b.U8() == 1; return;
        case T_I32_BYTE:    out.type = Value::INT; out.i = b.I8(); return;
        case T_U32_BYTE:    out.type = Value::INT; out.i = b.U8(); return;
        case T_INT16:       out.type = Value::INT; out.i = b.I16(); return;
        case T_UINT16:      out.type = Value::INT; out.i = b.U16(); return;
        case T_INT32:       out.type = Value::INT; out.i = b.I32(); return;
        case T_UINT32:      out.type = Value::INT; out.i = b.U32(); return;
        case T_FLOAT:       out.type = Value::FLOAT; out.d = b.F32(); return;
        case T_INT64:       out.type = Value::INT; out.i = b.I64(); return;
        case T_UINT64:      out.type = Value::INT; out.i = static_cast<int64_t>(b.U64()); return;
        case T_DOUBLE:      out.type = Value::FLOAT; out.d = b.F64(); return;

        case T_STRING: {
            int32_t index = b.I32();
            out.type = Value::STRING;
            if (index >= 0 && index < static_cast<int32_t>(strings.size()))
                out.s = strings[index];
            return;
        }

        case T_BLOB: {
            out.type = Value::BLOB;
            if (blob_index >= blob_sizes.size()) {
                failed = true;
                return;
            }

            size_t size = blob_sizes[blob_index++];
            if (blob_pos + size > blobs.size()) {
                failed = true;
                return;
            }

            out.s.assign(reinterpret_cast<const char*>(blobs.data() + blob_pos), size);
            blob_pos += size;
            return;
        }

        case T_ARRAY:
            read_items(static_cast<size_t>(std::max(b.I32(), 0)), false);
            return;

        case T_ARRAY_TYPED:
            read_items(static_cast<size_t>(std::max(b.I32(), 0)), true);
            return;

        case T_ARRAY_BYTE_LEN:
            read_items(b.U8(), true);
            return;

        case T_ARRAY_AUX: {
            size_t count = b.U8();
            auto previous = current;
            auto item_type = ReadType();

            current = current == &main ? &aux : &main;
            out.type = Value::ARRAY;
            out.items.resize(count);

            depth++;
            for (auto& item : out.items) {
                ParseValue(item_type.first, item_type.second, item);
                if (failed)
                    break;
            }
            depth--;

            current = previous;
            return;
        }

        case T_OBJECT: {
            if (object_index >= object_lengths.size()) {
                failed = true;
                return;
            }

            int32_t count = std::max(object_lengths[object_index++], 0);
            out.type = Value::OBJECT;
            out.keys.resize(count);
            out.items.resize(count);

            depth++;
            for (int32_t k = 0; k < count; k++) {
                int32_t name = b.I32();
                if (name >= 0 && name < static_cast<int32_t>(strings.size()))
                    out.keys[k] = strings[name];

                auto [t, f] = ReadType();
                ParseValue(t, f, out.items[k]);
                if (failed)
                    break;
            }
            depth--;
            return;
        }
        }

        LOGF(WARNING, "Unknown KV3 type {} in the map physics", type);
        failed = true;
    }

    struct Triangle {
        float v[9];
    };

    bool BlocksSight(const Value& attribute) {
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

    auto phys = PhysicsBlock(*resource);
    resource.reset();

    if (!phys) {
        LOGF(WARNING, "The world physics of {} has no PHYS block", name);
        return false;
    }

    Value root;
    if (!KV3().Parse(*phys, root)) {
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
