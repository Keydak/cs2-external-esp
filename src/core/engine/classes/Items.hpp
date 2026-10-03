#pragma once

enum class ItemKind {
    Weapon,
    Utility,    // Grenades
    Bomb,       // Dropped C4
    Kit,        // Defuse kit
};

// Something lying on the ground nobody owns
struct Item {
    ItemKind kind = ItemKind::Weapon;
    int item_index = 0;
    std::string name;
    const char* icon = "";
    Vec3_t pos{};
    int ammo = -1;
    uintptr_t entity = 0;
    uintptr_t node = 0;     // Scene node, where the position is read from
};

// Scans the entity list for dropped weapons, grenades, the bomb & defuse kits
class Items {
public:
    bool Update(uintptr_t entity_list);

    // Reads where the items found by the last scan are now, so falling ones move smoothly
    void UpdatePositions();

public:
    std::vector<Item> list;

private:
    // What a designer name is, looked up once per name
    enum class NameKind { None, Weapon, Kit };
    NameKind GetKind(uintptr_t name_address);

    std::unordered_map<uintptr_t, NameKind> kinds;
    std::chrono::steady_clock::time_point kinds_cleared{};
    std::vector<uint8_t> buffer;
};
