#pragma once
#include <optional>

// Compiled Source 2 resources (the _c files of the game): their blocks, the binary KeyValues3 in them & the other
// resources they refer to. Used for the map collision & to check custom player models before the game loads them
namespace Resource {
    // A KeyValues3 value
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

    // The block of a compiled resource by its four letter name ("DATA", "PHYS", "RERL"...)
    std::optional<std::vector<uint8_t>> Block(const std::vector<uint8_t>& resource, const char* tag);

    // Binary KeyValues3 version 5, compressed with zstd, LZ4 or not at all. False when it could not be read
    bool ParseKV3(const std::vector<uint8_t>& data, Value& root);

    // The resources a RERL block (of the resource) refers to, as the game names them: "materials/x.vmat", without the _c
    std::vector<std::string> References(const std::vector<uint8_t>& rerl);
}
