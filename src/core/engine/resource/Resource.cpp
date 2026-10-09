#include "Resource.hpp"

#include <external/zstd/zstd.h>

namespace {
    using Resource::Value;

    template<class T>
    T Read(const std::vector<uint8_t>& data, size_t offset) {
        T value{};
        if (offset + sizeof(T) <= data.size())
            memcpy(&value, data.data() + offset, sizeof(T));
        return value;
    }

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
                if (b2)
                    p = (p + 1) & ~size_t(1);
                p2 = p; p += b2 * 2;
                if (b4)
                    p = (p + 3) & ~size_t(3);
                p4 = p; p += b4 * 4;
                if (b8)
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
        bool legacy = false;            // Version 3 & 4: one buffer, object sizes among its ints
        bool blobs_skipped = false;     // Blobs of a legacy LZ4 buffer, in frames not read: blob values come back empty

        bool ParseLegacy(const std::vector<uint8_t>& data, uint32_t version);
        std::pair<uint8_t, uint8_t> ReadType();
        void ParseValue(uint8_t type, uint8_t flag, Value& out);
    };

    // LZ4 block format, the whole output in one go: the frames of a KV3 buffer follow each other & may refer back
    bool DecompressLz4(const uint8_t* src, size_t size, size_t capacity, std::vector<uint8_t>& out, bool more_after = false) {
        out.clear();
        out.reserve(capacity);

        size_t i = 0;
        while (i < size) {
            uint8_t token = src[i++];

            size_t literals = token >> 4;
            if (literals == 15) {
                uint8_t more;
                do {
                    if (i >= size)
                        return false;
                    more = src[i++];
                    literals += more;
                } while (more == 255);
            }

            if (i + literals > size || out.size() + literals > capacity)
                return false;
            out.insert(out.end(), src + i, src + i + literals);
            i += literals;

            // The last sequence has literals only. Other data after it (the blobs of legacy buffers) is not ours
            if (i >= size || (more_after && out.size() == capacity))
                break;

            if (i + 2 > size)
                return false;
            size_t offset = src[i] | (src[i + 1] << 8);
            i += 2;

            size_t length = token & 15;
            if (length == 15) {
                uint8_t more;
                do {
                    if (i >= size)
                        return false;
                    more = src[i++];
                    length += more;
                } while (more == 255);
            }
            length += 4;

            if (offset == 0 || offset > out.size() || out.size() + length > capacity)
                return false;

            // Byte by byte, the copy may overlap what it writes
            size_t from = out.size() - offset;
            for (size_t k = 0; k < length; k++)
                out.push_back(out[from + k]);
        }

        return out.size() == capacity;
    }

    enum Compression : uint32_t { NOT_COMPRESSED = 0, LZ4 = 1, ZSTD = 2 };

    bool Decompress(uint32_t method, const uint8_t* src, size_t size, size_t capacity, std::vector<uint8_t>& out) {
        if (!capacity) {
            out.clear();
            return true;
        }

        switch (method) {
        case NOT_COMPRESSED:
            if (size < capacity)
                return false;
            out.assign(src, src + capacity);
            return true;

        case LZ4:
            return DecompressLz4(src, size, capacity, out);

        case ZSTD: {
            out.resize(capacity);
            auto result = ZSTD_decompress(out.data(), capacity, src, size);
            if (ZSTD_isError(result))
                return false;

            out.resize(result);
            return true;
        }
        }

        return false;
    }

    bool KV3::Parse(const std::vector<uint8_t>& data, Value& root) {
        if (data.size() < 8 || memcmp(data.data() + 1, "3VK", 3) != 0)
            return false;

        // Models compiled before 2024 have the older versions
        if (data[0] == 3 || data[0] == 4) {
            if (!ParseLegacy(data, data[0]))
                return false;

            current = &main;
            auto [type, flag] = ReadType();
            ParseValue(type, flag, root);
            return !failed;
        }

        if (data.size() < 120 || data[0] != 5)
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

        // Not compressed, the buffers are stored as they are
        if (compression == NOT_COMPRESSED) {
            comp1 = unc1;
            comp2 = unc2;
        }

        size_t pos = 120;
        if (pos + comp1 + comp2 > data.size())
            return false;

        if (!Decompress(compression, data.data() + pos, comp1, unc1, buf1))
            return false;
        pos += comp1;

        if (!Decompress(compression, data.data() + pos, comp2, unc2, buf2))
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
            if (compression == NOT_COMPRESSED)
                blob_compressed = blob_bytes;
            if (pos + blob_compressed > data.size() || !Decompress(compression, data.data() + pos, blob_compressed, blob_bytes, blobs))
                return false;
        }

        current = &main;
        auto [type, flag] = ReadType();
        ParseValue(type, flag, root);

        return !failed;
    }

    // Header: magic, format guid, compression (+ dictionary & frame size), counts of the 1, 4 & 8 byte values & of the
    // types, objects & arrays (shorts), sizes, blobs; version 4 adds the 2 byte values & the size of the frame sizes.
    // One buffer: values by size, the string count first of its ints, strings after the 8 byte values, then types,
    // blob sizes & 0xFFEEDD00
    bool KV3::ParseLegacy(const std::vector<uint8_t>& data, uint32_t version) {
        size_t header = version == 4 ? 72 : 64;
        if (data.size() < header)
            return false;

        auto u32 = [&](size_t o) { return Read<uint32_t>(data, o); };

        uint32_t compression = u32(20);
        uint32_t b1 = u32(28), b4 = u32(32), b8 = u32(36), count_types = u32(40);
        uint32_t uncompressed = u32(48), compressed = u32(52), block_count = u32(56), blob_bytes = u32(60);
        uint32_t b2 = version == 4 ? u32(64) : 0;

        if (uncompressed > (64u << 20) || block_count > (1u << 20))
            return false;

        size_t available = data.size() - header;
        const uint8_t* src = data.data() + header;

        switch (compression) {
        case NOT_COMPRESSED:
            if (available < uncompressed)
                return false;
            buf2.assign(src, src + uncompressed);
            if (block_count && available >= static_cast<size_t>(uncompressed) + blob_bytes)
                blobs.assign(src + uncompressed, src + uncompressed + blob_bytes);
            else
                blobs_skipped = block_count > 0;
            break;

        case LZ4:
            // Blobs follow in frames of their own, not needed for what is read here
            if (!DecompressLz4(src, std::min<size_t>(available, compressed), uncompressed, buf2, block_count > 0))
                return false;
            blobs_skipped = block_count > 0;
            break;

        case ZSTD: {
            std::vector<uint8_t> all;
            if (!Decompress(ZSTD, src, std::min<size_t>(available, compressed), static_cast<size_t>(uncompressed) + blob_bytes, all) || all.size() < uncompressed)
                return false;
            buf2.assign(all.begin(), all.begin() + uncompressed);
            blobs.assign(all.begin() + uncompressed, all.end());
            break;
        }

        default:
            return false;
        }

        legacy = true;
        main.Init(buf2, 0, b1, b2, b4, b8);
        aux = main;
        if (main.end > buf2.size())
            return false;

        int32_t string_count = main.I32();
        if (string_count < 0)
            return false;

        size_t p = main.end;
        strings.reserve(string_count);
        for (int32_t s = 0; s < string_count; s++) {
            auto start = p;
            while (p < buf2.size() && buf2[p])
                p++;
            if (p >= buf2.size())
                return false;

            strings.emplace_back(reinterpret_cast<const char*>(buf2.data() + start), p - start);
            p++;
        }

        // Types up to the blob sizes & the trailer at the end
        size_t tail = static_cast<size_t>(block_count) * 4 + 4;
        if (buf2.size() < tail || p > buf2.size() - tail || Read<uint32_t>(buf2, buf2.size() - 4) != 0xFFEEDD00)
            return false;

        types = p;
        types_end = buf2.size() - tail;
        (void)count_types;

        blob_sizes.resize(block_count);
        if (block_count)
            memcpy(blob_sizes.data(), buf2.data() + types_end, static_cast<size_t>(block_count) * 4);

        return true;
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
            if (blobs_skipped)
                return;
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
            if (!legacy && object_index >= object_lengths.size()) {
                failed = true;
                return;
            }

            int32_t count = std::max(legacy ? b.I32() : object_lengths[object_index++], 0);
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

        LOGF(VERBOSE, "Unknown KV3 type {}", type);
        failed = true;
    }

}

std::optional<std::vector<uint8_t>> Resource::Block(const std::vector<uint8_t>& resource, const char* tag) {
    auto block_offset = Read<uint32_t>(resource, 8);
    auto block_count = Read<uint32_t>(resource, 12);
    size_t base = 8 + block_offset;

    for (uint32_t i = 0; i < block_count; i++) {
        size_t entry = base + i * 12;
        if (entry + 12 > resource.size())
            break;

        if (memcmp(resource.data() + entry, tag, 4) != 0)
            continue;

        size_t start = entry + 4 + Read<uint32_t>(resource, entry + 4);
        size_t size = Read<uint32_t>(resource, entry + 8);
        if (start + size > resource.size())
            return std::nullopt;

        return std::vector<uint8_t>(resource.begin() + start, resource.begin() + start + size);
    }

    return std::nullopt;
}

bool Resource::ParseKV3(const std::vector<uint8_t>& data, Value& root) {
    return KV3().Parse(data, root);
}

std::vector<std::string> Resource::References(const std::vector<uint8_t>& rerl) {
    std::vector<std::string> out;

    // Offset of the entries from the start, their count. Each: a 64 bit id, the offset of its name from that field
    auto offset = Read<uint32_t>(rerl, 0);
    auto count = Read<uint32_t>(rerl, 4);

    for (uint32_t i = 0; i < count && i < 4096; i++) {
        size_t entry = offset + i * 16ull;
        if (entry + 16 > rerl.size())
            break;

        size_t name = entry + 8 + Read<uint64_t>(rerl, entry + 8);
        size_t end = name;
        while (end < rerl.size() && rerl[end])
            end++;

        if (end >= rerl.size())
            break;

        out.emplace_back(reinterpret_cast<const char*>(rerl.data() + name), end - name);
    }

    return out;
}
