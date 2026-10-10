#include "MaterialChams.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/GameThread.hpp"
#include "core/offsets/Dumper.hpp"

namespace {
    namespace mat = offsets::materials;
    namespace vis = offsets::visuals;

    constexpr auto POLL = std::chrono::milliseconds(20);
    constexpr auto MATERIAL_SETTLE = std::chrono::milliseconds(250);   // A color dragged in the menu: made once it stays
    constexpr auto CALIBRATE_TELL = std::chrono::seconds(15);           // Players in the table & still no owner field
    constexpr int MAX_FAILURES = 3;                                     // Making the materials, then it gives up

    // Page of the hook: our data, the table of scene objects, the stub
    constexpr size_t PAGE_SIZE = 0x2000;
    constexpr uintptr_t PAGE_MAGIC = 0x00;      // uint64
    constexpr uintptr_t PAGE_ORIGINAL = 0x08;   // uint64, DrawArray of the game
    constexpr uintptr_t DATA_OWNER = 0x10;      // int32, field of a mesh holding its scene object, -1 while not known
    constexpr uintptr_t DATA_ENABLED = 0x14;    // uint32
    constexpr uintptr_t DATA_COUNT = 0x18;      // uint32, entries of the table
    constexpr uintptr_t DATA_MATERIALS = 0x80;  // CMaterial2*[SLOT_COUNT] by Slot, 0 leaves the own material of the mesh
    constexpr uintptr_t DATA_VOTES = 0x40;      // uint32[8], meshes whose field k * 8 is one of the table
    constexpr uintptr_t TABLE = 0x200;          // { scene object, class (1 enemy, 2 team) } each
    constexpr size_t TABLE_ENTRY = 0x10;
    constexpr size_t MAX_ENTRIES = 192;         // Players, their weapons & the rest, the table ends before the stub
    constexpr uintptr_t STUB = mat::stubOffset;
    constexpr uint64_t MAGIC = mat::stubMagic;      // A mark to find our page again
    constexpr int OWNER_FIELDS = 8;             // Fields 0x00 - 0x38 of a mesh are candidates
    // Classes of the table, a slot pair each (Slot): the low 3 bits of an entry
    constexpr uint32_t ENEMY = 1, TEAM = 2, BOMB = 3, ITEMS = 4, LOCAL = 5, WEAPON = 6, HANDS = 7;
    constexpr uint32_t CLASS_COUNT = 7;
    constexpr int MAX_MAKE = 4;                 // Materials made in one call, what the page of a making holds
    // Seen right now: no behind walls pass. The parts of a model are drawn in separate calls, the pass of one drawn
    // after another went over it without the depth test, even where the model covers itself
    constexpr uint32_t NO_HIDDEN = 0x100;
    // Not drawn at all (with NO_HIDDEN, no behind walls pass either)
    constexpr uint32_t SKIP = 0x200;

    // Stack of the stub: 8 pushes, 0x58 of its own. The 4 stack arguments of DrawArray, then ours
    constexpr int32_t ARGS = 0xC0;
    constexpr uint8_t SAVED = 0x40;             // The own material of the mesh drawn

    // Page of one making of the materials: code, texts, names, results, KeyValues3 contexts
    constexpr size_t MAKE_SIZE = 0x8000;
    constexpr uintptr_t MAKE_TEXTS = 0x800;     // 0x600 each
    constexpr size_t TEXT_SIZE = 0x600;
    constexpr uintptr_t MAKE_NAMES = 0x2000;    // 0x40 each
    constexpr uintptr_t MAKE_EMPTY = 0x2100;    // ""
    constexpr uintptr_t MAKE_OUT = 0x2140;      // CStrongHandle<CMaterial2> each
    constexpr uintptr_t MAKE_ERRORS = 0x2180;   // CUtlString each, 0x10
    constexpr uintptr_t MAKE_CONTEXTS = 0x3000; // 0x1400 each
    constexpr size_t CONTEXT_SIZE = 0x1400;
    static_assert(MAKE_CONTEXTS + 4 * CONTEXT_SIZE <= MAKE_SIZE);
    static_assert(mat::kv3ContextSize <= static_cast<std::ptrdiff_t>(CONTEXT_SIZE));

    constexpr const char* LOAD_KV3 = "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z";

    // Small x64 assembler of what the stubs need
    struct Code {
        std::vector<uint8_t> bytes;

        void put(std::initializer_list<uint8_t> list) { bytes.insert(bytes.end(), list); }
        void put32(int32_t value) { for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); }
        void put64(uint64_t value) { for (int i = 0; i < 8; i++) bytes.push_back(static_cast<uint8_t>(value >> (i * 8))); }
        size_t size() const { return bytes.size(); }

        // jcc / jmp rel32 to a label set later, or back to one before
        size_t jump(std::initializer_list<uint8_t> opcode) { put(opcode); put32(0); return size(); }
        void land(size_t jump_end) {
            int32_t rel = static_cast<int32_t>(size() - jump_end);
            std::memcpy(&bytes[jump_end - 4], &rel, 4);
        }
        void back(std::initializer_list<uint8_t> opcode, size_t label) {
            put(opcode);
            put32(static_cast<int32_t>(static_cast<int64_t>(label) - static_cast<int64_t>(size() + 4)));
        }
    };

    // The group of a class of the table
    const cfg::visuals::material_chams::group_t& GroupOf(uint32_t kind) {
        namespace chams = cfg::visuals::material_chams;
        switch (kind) {
        case ENEMY:     return chams::enemy;
        case TEAM:      return chams::team;
        case BOMB:      return chams::bomb;
        case ITEMS:     return chams::items;
        case LOCAL:     return chams::local;
        case HANDS:     return chams::local;
        default:        return chams::weapon;
        }
    }

    // Ours are never behind a wall from where we look
    bool HasHidden(uint32_t kind) {
        return kind != LOCAL && kind != WEAPON && kind != HANDS;
    }

    // Our materials only for what the ESP of that group draws
    const cfg::visuals::material_chams::group_t& Group(bool enemy) {
        return GroupOf(enemy ? ENEMY : TEAM);
    }

    // Visible & behind walls split per pixel. The parts of a model come in separate calls: the behind walls pass of a
    // later part goes over the visible pass of an earlier one (no depth test). When the visible material writes no
    // depth (translucent, opacity 1) & is left out of the depth prepass, its depth test only meets the walls: the visible
    // pass of the later part paints its pixels back wherever no wall is in front, whatever part is in front of it.
    // Only Flat & Glow have a behind walls pass (MaterialChams::HasBehindWalls). Without a visible material of ours the
    // own material of the game is the visible pass: the split stays, parts in front drawn first can keep patches (the
    // material of the game writes its depth)
    bool Hidden(const cfg::visuals::material_chams::group_t& group) {
        namespace chams = cfg::visuals::material_chams;
        return group.hidden.enabled && group.material != chams::MATERIAL_HOLOGRAM && group.material != chams::MATERIAL_METALLIC;
    }

    bool Layered(const cfg::visuals::material_chams::group_t& group) {
        return Hidden(group);
    }
}

bool MaterialChams::Init() {
    return GetInstance().InitImpl();
}

bool MaterialChams::HasBehindWalls(int material) {
    namespace chams = cfg::visuals::material_chams;
    return material != chams::MATERIAL_HOLOGRAM && material != chams::MATERIAL_METALLIC;
}

void MaterialChams::SetHiddenBodies(const std::vector<uintptr_t>& pawns) {
    auto& i = GetInstance();
    std::lock_guard lock(i.bodies_mutex);
    i.hidden_bodies = pawns;
}

bool MaterialChams::Wanted(const cfg::visuals::material_chams::group_t& group) {
    return group.visible.enabled || Hidden(group);
}

bool MaterialChams::IsAvailable() {
    return Engine::IsInsecure() && mat::fnDrawArray && mat::drawArrayEntry && mat::fnCreateMaterial && mat::fnKv3Context;
}

std::string MaterialChams::GetStatus() {
    auto& i = GetInstance();
    std::lock_guard lock(i.status_mutex);
    return i.status;
}

bool MaterialChams::InitImpl() {
    if (!IsAvailable())
        return false;

    std::thread(&MaterialChams::Thread, this).detach();
    LOGF(INFO, "Successfully initialized material chams...");
    return true;
}

void MaterialChams::SetStatus(const std::string& text) {
    std::lock_guard lock(this->status_mutex);
    if (this->status != text && !text.empty())
        LOGF(WARNING, "Material chams: {}", text);
    this->status = text;
}

void MaterialChams::Thread() {
    while (!this->stopping) {
        std::this_thread::sleep_for(POLL);
        Update();
    }
}

void MaterialChams::Update() {
    auto p = Engine::GetProcess();
    if (!p || !Engine::IsInsecure())
        return;

    namespace chams = cfg::visuals::material_chams;
    bool bodies = false;
    {
        std::lock_guard lock(this->bodies_mutex);
        bodies = !this->hidden_bodies.empty();
    }
    bool wanted = bodies;
    for (uint32_t kind = 1; kind <= CLASS_COUNT; kind++) {
        const auto& group = GroupOf(kind);
        wanted |= group.visible.enabled || (HasHidden(kind) && group.hidden.enabled);
    }

    // Hooked once wanted, then it stays: off is only the switch of the page
    if (!this->installed) {
        if (!wanted || this->failures >= MAX_FAILURES)
            return;
        if (!Install()) {
            this->failures = MAX_FAILURES;
            SetStatus("the drawing of the game could not be hooked");
            return;
        }
    }

    if (!wanted) {
        p->write<uint32_t>(this->page + DATA_ENABLED, 0);
        return;
    }

    if (this->failures < MAX_FAILURES && !UpdateMaterials()) {
        if (++this->failures >= MAX_FAILURES)
            SetStatus("the game did not make the materials");
    }

    WriteTargets();
    Calibrate();
    p->write<uint32_t>(this->page + DATA_ENABLED, 1);
}

bool MaterialChams::Install() {
    auto p = Engine::GetProcess();
    auto scene = p->GetModule("scenesystem.dll");
    if (!scene.base)
        return false;

    this->material_system = p->FindInterface("materialsystem2.dll", "VMaterialSystem2_001");
    this->load_kv3 = p->FindExport("tier0.dll", LOAD_KV3);
    if (!this->material_system || !this->load_kv3) {
        LOGF(WARNING, "Material chams: material system 0x{:X}, LoadKV3 0x{:X}", this->material_system, this->load_kv3);
        return false;
    }

    this->entry = scene.base + mat::drawArrayEntry;
    this->original = scene.base + mat::fnDrawArray;

    // Still our stub from an earlier run that was closed without putting the entry back
    auto current = p->read<uintptr_t>(this->entry);
    if (current != this->original) {
        auto old = current - STUB;
        if (p->read<uint64_t>(old + PAGE_MAGIC) == MAGIC && p->read<uintptr_t>(old + PAGE_ORIGINAL) == this->original)
            LOGF(INFO, "The DrawArray entry was still ours from an earlier run");
        else {
            LOGF(WARNING, "Material chams: the DrawArray entry points somewhere else (0x{:X})", current);
            return false;
        }
    }

    this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return false;

    // rcx desc, rdx render context, r8 meshes, r9d count, 4 more on the stack
    Code c;
    c.put({ 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 });  // push rbx, rbp, rsi, rdi, r12 - r15
    c.put({ 0x48, 0x83, 0xEC, 0x58 });              // sub rsp, 0x58
    c.put({ 0x48, 0x8B, 0xD9 });                    // mov rbx, rcx
    c.put({ 0x48, 0x8B, 0xEA });                    // mov rbp, rdx
    c.put({ 0x49, 0x8B, 0xF0 });                    // mov rsi, r8
    c.put({ 0x45, 0x8B, 0xF9 });                    // mov r15d, r9d
    c.put({ 0x49, 0xBC }); c.put64(this->page);     // mov r12, page

    // DrawArray of the game: all meshes (rsi, r15d) or the one in r14. The callee owns its stack arguments, copied each time
    auto call_original = [&](bool all) {
        for (int32_t k = 0; k < 4; k++) {
            c.put({ 0x48, 0x8B, 0x84, 0x24 }); c.put32(ARGS + 8 * k);              // mov rax, [rsp + argument]
            c.put({ 0x48, 0x89, 0x44, 0x24, static_cast<uint8_t>(0x20 + 8 * k) }); // mov [rsp + 0x20 + k * 8], rax
        }
        c.put({ 0x48, 0x8B, 0xCB });                // mov rcx, rbx
        c.put({ 0x48, 0x8B, 0xD5 });                // mov rdx, rbp
        if (all) {
            c.put({ 0x4C, 0x8B, 0xC6 });            // mov r8, rsi
            c.put({ 0x45, 0x8B, 0xCF });            // mov r9d, r15d
        }
        else {
            c.put({ 0x4D, 0x8B, 0xC6 });            // mov r8, r14
            c.put({ 0x41, 0xB9, 0x01, 0x00, 0x00, 0x00 }); // mov r9d, 1
        }
        c.put({ 0x41, 0xFF, 0x54, 0x24, PAGE_ORIGINAL });  // call [r12 + original]
    };

    // r14 the mesh r13d
    auto mesh = [&]() {
        c.put({ 0x49, 0x69, 0xC5 }); c.put32(static_cast<int32_t>(mat::meshStride)); // imul rax, r13, stride
        c.put({ 0x4C, 0x8D, 0x34, 0x06 });          // lea r14, [rsi + rax]
    };

    // eax the class of the mesh r14 in the table, 0 when not there
    auto find_class = [&]() {
        c.put({ 0x49, 0x63, 0x44, 0x24, DATA_OWNER });  // movsxd rax, [r12 + owner]
        c.put({ 0x4D, 0x8B, 0x04, 0x06 });          // mov r8, [r14 + rax]
        c.put({ 0x41, 0x8B, 0x4C, 0x24, DATA_COUNT });  // mov ecx, [r12 + count]
        c.put({ 0x49, 0x8D, 0x94, 0x24 }); c.put32(TABLE); // lea rdx, [r12 + table]
        c.put({ 0x31, 0xC0 });                      // xor eax, eax
        size_t loop = c.size();
        c.put({ 0x85, 0xC9 });                      // test ecx, ecx
        auto done = c.jump({ 0x0F, 0x84 });         // jz done
        c.put({ 0x4C, 0x39, 0x02 });                // cmp [rdx], r8
        auto hit = c.jump({ 0x0F, 0x84 });          // je hit
        c.put({ 0x48, 0x83, 0xC2, TABLE_ENTRY });   // add rdx, entry
        c.put({ 0xFF, 0xC9 });                      // dec ecx
        c.back({ 0xE9 }, loop);                     // jmp loop
        c.land(hit);
        c.put({ 0x8B, 0x42, 0x08 });                // mov eax, [rdx + 8]
        c.land(done);
    };

    std::vector<size_t> to_all;
    c.put({ 0x41, 0x83, 0x7C, 0x24, DATA_ENABLED, 0x00 }); // cmp dword [r12 + enabled], 0
    to_all.push_back(c.jump({ 0x0F, 0x84 }));       // je all
    c.put({ 0x45, 0x85, 0xFF });                    // test r15d, r15d
    to_all.push_back(c.jump({ 0x0F, 0x8E }));       // jle all
    c.put({ 0x41, 0x83, 0x7C, 0x24, DATA_COUNT, 0x00 }); // cmp dword [r12 + count], 0
    to_all.push_back(c.jump({ 0x0F, 0x84 }));       // je all
    c.put({ 0x41, 0x83, 0x7C, 0x24, DATA_OWNER, 0xFF }); // cmp dword [r12 + owner], -1
    auto to_check = c.jump({ 0x0F, 0x85 });         // jne check

    // Owner field not known yet: each field of each mesh that is a scene object of the table gets a vote
    c.put({ 0x45, 0x33, 0xED });                    // xor r13d, r13d
    size_t vote_mesh = c.size();
    c.put({ 0x45, 0x3B, 0xEF });                    // cmp r13d, r15d
    to_all.push_back(c.jump({ 0x0F, 0x8D }));       // jge all
    mesh();
    c.put({ 0x33, 0xFF });                          // xor edi, edi (field * 8)
    size_t vote_field = c.size();
    c.put({ 0x83, 0xFF, OWNER_FIELDS * 8 });        // cmp edi, fields
    auto fields_done = c.jump({ 0x0F, 0x8D });      // jge next mesh
    c.put({ 0x4D, 0x8B, 0x04, 0x3E });              // mov r8, [r14 + rdi]
    c.put({ 0x4D, 0x85, 0xC0 });                    // test r8, r8
    auto field_empty = c.jump({ 0x0F, 0x84 });      // jz next field
    c.put({ 0x41, 0x8B, 0x4C, 0x24, DATA_COUNT });  // mov ecx, [r12 + count]
    c.put({ 0x49, 0x8D, 0x94, 0x24 }); c.put32(TABLE); // lea rdx, [r12 + table]
    size_t vote_entry = c.size();
    c.put({ 0x85, 0xC9 });                          // test ecx, ecx
    auto field_missing = c.jump({ 0x0F, 0x84 });    // jz next field
    c.put({ 0x4C, 0x39, 0x02 });                    // cmp [rdx], r8
    auto field_hit = c.jump({ 0x0F, 0x84 });        // je vote
    c.put({ 0x48, 0x83, 0xC2, TABLE_ENTRY });       // add rdx, entry
    c.put({ 0xFF, 0xC9 });                          // dec ecx
    c.back({ 0xE9 }, vote_entry);                   // jmp entry
    c.land(field_hit);
    c.put({ 0x8B, 0xC7 });                          // mov eax, edi
    c.put({ 0xD1, 0xE8 });                          // shr eax, 1 (field * 4)
    c.put({ 0xF0, 0x41, 0xFF, 0x44, 0x04, DATA_VOTES }); // lock inc dword [r12 + rax + votes]
    c.land(field_empty);
    c.land(field_missing);
    c.put({ 0x83, 0xC7, 0x08 });                    // add edi, 8
    c.back({ 0xE9 }, vote_field);                   // jmp field
    c.land(fields_done);
    c.put({ 0x41, 0xFF, 0xC5 });                    // inc r13d
    c.back({ 0xE9 }, vote_mesh);                    // jmp mesh

    // Any mesh of ours? Else all of them in one call, like the game
    c.land(to_check);
    c.put({ 0x45, 0x33, 0xED });                    // xor r13d, r13d
    size_t check = c.size();
    c.put({ 0x45, 0x3B, 0xEF });                    // cmp r13d, r15d
    to_all.push_back(c.jump({ 0x0F, 0x8D }));       // jge all
    mesh();
    find_class();
    c.put({ 0x85, 0xC0 });                          // test eax, eax
    auto to_each = c.jump({ 0x0F, 0x85 });          // jnz each
    c.put({ 0x41, 0xFF, 0xC5 });                    // inc r13d
    c.back({ 0xE9 }, check);                        // jmp check

    // One mesh at a time. All behind walls passes first: one drawn after a visible mesh would cover it, without the
    // depth test it goes over the model itself. Then the visible ones (or their own material). Their own put back
    auto slots = [&]() {
        c.put({ 0x83, 0xE0, 0x07 });                // and eax, 7 (the class)
        c.put({ 0x4C, 0x8B, 0xD0 });                // mov r10, rax
        c.put({ 0x49, 0xC1, 0xE2, 0x04 });          // shl r10, 4
        c.put({ 0x4D, 0x03, 0xD4 });                // add r10, r12
        c.put({ 0x49, 0x83, 0xC2, DATA_MATERIALS - 0x10 }); // add r10, materials - 16: the two of the class
    };
    // rax the material to draw r14 with
    auto draw_with = [&]() {
        c.put({ 0x49, 0x8B, 0x56, static_cast<uint8_t>(mat::meshMaterial) });   // mov rdx, [r14 + material]
        c.put({ 0x48, 0x89, 0x54, 0x24, SAVED });   // mov [rsp + saved], rdx
        c.put({ 0x49, 0x89, 0x46, static_cast<uint8_t>(mat::meshMaterial) });   // mov [r14 + material], rax
        call_original(false);
        c.put({ 0x48, 0x8B, 0x44, 0x24, SAVED });   // mov rax, [rsp + saved]
        c.put({ 0x49, 0x89, 0x46, static_cast<uint8_t>(mat::meshMaterial) });   // mov [r14 + material], rax
    };

    c.land(to_each);
    c.put({ 0x45, 0x33, 0xED });                    // xor r13d, r13d
    size_t hidden = c.size();
    c.put({ 0x45, 0x3B, 0xEF });                    // cmp r13d, r15d
    auto to_visible = c.jump({ 0x0F, 0x8D });       // jge visible
    mesh();
    find_class();
    c.put({ 0x85, 0xC0 });                          // test eax, eax
    auto hidden_none = c.jump({ 0x0F, 0x84 });      // jz next
    c.put({ 0xA9 }); c.put32(NO_HIDDEN);            // test eax, no hidden
    auto hidden_seen = c.jump({ 0x0F, 0x85 });      // jnz next
    slots();
    c.put({ 0x49, 0x8B, 0x42, 0x08 });              // mov rax, [r10 + 8] (behind walls)
    c.put({ 0x48, 0x85, 0xC0 });                    // test rax, rax
    auto hidden_off = c.jump({ 0x0F, 0x84 });       // jz next
    draw_with();
    c.land(hidden_none);
    c.land(hidden_seen);
    c.land(hidden_off);
    c.put({ 0x41, 0xFF, 0xC5 });                    // inc r13d
    c.back({ 0xE9 }, hidden);                       // jmp hidden

    c.land(to_visible);
    c.put({ 0x45, 0x33, 0xED });                    // xor r13d, r13d
    size_t visible = c.size();
    c.put({ 0x45, 0x3B, 0xEF });                    // cmp r13d, r15d
    auto to_exit = c.jump({ 0x0F, 0x8D });          // jge exit
    mesh();
    find_class();
    c.put({ 0x85, 0xC0 });                          // test eax, eax
    auto visible_none = c.jump({ 0x0F, 0x84 });     // jz plain
    c.put({ 0xA9 }); c.put32(SKIP);                 // test eax, skip
    auto visible_skip = c.jump({ 0x0F, 0x85 });     // jnz next
    slots();
    c.put({ 0x49, 0x8B, 0x02 });                    // mov rax, [r10] (visible)
    c.put({ 0x48, 0x85, 0xC0 });                    // test rax, rax
    auto visible_off = c.jump({ 0x0F, 0x84 });      // jz plain
    draw_with();
    auto to_next = c.jump({ 0xE9 });                // jmp next
    c.land(visible_none);
    c.land(visible_off);
    call_original(false);
    c.land(to_next);
    c.land(visible_skip);
    c.put({ 0x41, 0xFF, 0xC5 });                    // inc r13d
    c.back({ 0xE9 }, visible);                      // jmp visible

    for (auto jump : to_all)
        c.land(jump);
    call_original(true);
    c.land(to_exit);
    c.put({ 0x48, 0x83, 0xC4, 0x58 });              // add rsp, 0x58
    c.put({ 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B }); // pop r15 - r12, rdi, rsi, rbp, rbx
    c.put({ 0xC3 });                                // ret

    if (STUB + c.size() > PAGE_SIZE) {
        LOGF(WARNING, "Material chams stub too large");
        p->free_remote(this->page);
        this->page = 0;
        return false;
    }

    std::vector<uint8_t> data(PAGE_SIZE, 0xCC);
    std::fill(data.begin(), data.begin() + STUB, 0);
    auto put64 = [&](uintptr_t at, uint64_t value) { std::memcpy(&data[at], &value, sizeof(value)); };
    put64(PAGE_MAGIC, MAGIC);
    put64(PAGE_ORIGINAL, this->original);
    int32_t unknown = -1;
    std::memcpy(&data[DATA_OWNER], &unknown, sizeof(unknown));
    std::copy(c.bytes.begin(), c.bytes.end(), data.begin() + STUB);

    p->write_bytes(this->page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), data.size());

    // The vtable is read only data of scenesystem
    auto stub = this->page + STUB;
    if (!p->patch_code(this->entry, &stub, sizeof(stub))) {
        LOGF(WARNING, "Material chams: could not write the DrawArray entry ({})", GetLastError());
        return false;
    }

    this->installed = true;
    this->calibrate_since = std::chrono::steady_clock::now();
    LOGF(INFO, "Players are drawn through our stub (DrawArray 0x{:X})", this->original);
    return true;
}

std::string MaterialChams::MaterialText(int material, bool hidden, bool layered, float r, float g, float b, float a,
    bool first_person) {
    namespace chams = cfg::visuals::material_chams;

    // Textures of the game: white, flat normal, white mask
    constexpr const char* WHITE = "resource:\"materials/dev/primary_white_color_tga_21186c76.vtex\"";
    constexpr const char* NORMAL = "resource:\"materials/default/default_normal_tga_7652cb.vtex\"";
    constexpr const char* MASK = "resource:\"materials/default/default_mask_tga_fde710a5.vtex\"";

    std::string body;
    switch (material) {
    case chams::MATERIAL_GLOW:
        // csgo_effects: only the edges lit, added over what is behind
        body = std::format(
            "\tshader = \"csgo_effects.vfx\"\n"
            "\tF_ADDITIVE_BLEND = 1\n"
            "\tF_DISABLE_Z_BUFFERING = {}\n"
            "\tg_vColorTint = [{:.3f}, {:.3f}, {:.3f}, {:.3f}]\n"
            "\tg_flColorBoost = 3.0\n"
            "\tg_flOpacityScale = {:.3f}\n"
            "\tg_flFresnelExponent = 7.0\n"
            "\tg_flFresnelFalloff = 10.0\n"
            "\tg_flFresnelMax = 0.1\n"
            "\tg_flFresnelMin = 1.0\n"
            "\tg_bFogEnabled = 0\n"
            "\tg_tColor = {}\n"
            "\tg_tMask1 = {}\n"
            "\tg_tMask2 = {}\n"
            "\tg_tMask3 = {}\n",
            hidden ? 1 : 0, r, g, b, a, a, WHITE, MASK, MASK, MASK);
        break;

    case chams::MATERIAL_HOLOGRAM:
        // csgo_unlitgeneric, blend mode 4 (additive): the color added over what is behind, so the body is see through.
        // csgo_effects came out solid with its additive switch on. No lines: a texture follows the UVs of the model,
        // which are cut in pieces, its lines came out as stripes all over. First person: the world is not seen through
        // the hands & weapon of the game whatever the material (added or mixed, both came out a solid color). There the
        // look of a hologram: bright edges over a faint body of the color, csgo_effects like Glow but with a body
        if (first_person) {
            body = std::format(
                "\tshader = \"csgo_effects.vfx\"\n"
                "\tF_ADDITIVE_BLEND = 1\n"
                "\tF_DISABLE_Z_BUFFERING = 0\n"
                "\tg_vColorTint = [{:.3f}, {:.3f}, {:.3f}, {:.3f}]\n"
                "\tg_flColorBoost = 2.0\n"
                "\tg_flOpacityScale = {:.3f}\n"
                "\tg_flFresnelExponent = 2.5\n"
                "\tg_flFresnelFalloff = 4.0\n"
                "\tg_flFresnelMax = 0.3\n"
                "\tg_flFresnelMin = 1.0\n"
                "\tg_bFogEnabled = 0\n"
                "\tg_tColor = {}\n"
                "\tg_tMask1 = {}\n"
                "\tg_tMask2 = {}\n"
                "\tg_tMask3 = {}\n",
                r, g, b, a, a, WHITE, MASK, MASK, MASK);
            break;
        }
        body = std::format(
            "\tshader = \"csgo_unlitgeneric.vfx\"\n"
            "\tF_DISABLE_Z_BUFFERING = {}\n"
            "\tF_BLEND_MODE = 4\n"
            "\tF_TRANSLUCENT = 1\n"
            "\tF_RENDER_BACKFACES = 1\n"
            "\tg_vColorTint = [{:.3f}, {:.3f}, {:.3f}, {:.3f}]\n"
            "\tg_flOpacityScale = {:.3f}\n"
            "\tg_bFogEnabled = 0\n"
            "\tg_tColor = {}\n"
            "\tg_tNormal = {}\n"
            "\tg_tAmbientOcclusion = {}\n",
            hidden ? 1 : 0, r, g, b, a, a * 0.5f, WHITE, NORMAL, MASK);
        break;

    case chams::MATERIAL_METALLIC:
        // csgo_complex: lit by the map, full metal in the color. The reflections of the map came out grey over the
        // color, a glow of the color from inside keeps it the color all over
        body = std::format(
            "\tshader = \"csgo_complex.vfx\"\n"
            "\tF_DISABLE_Z_BUFFERING = {}\n"
            "\tF_TRANSLUCENT = {}\n"
            "\tF_SELF_ILLUM = 1\n"
            "\tg_vColorTint = [{:.3f}, {:.3f}, {:.3f}, {:.3f}]\n"
            "\tg_flOpacityScale = {:.3f}\n"
            "\tg_flMetalness = 1.0\n"
            "\tg_flModelTintAmount = 1.0\n"
            "\tg_vSelfIllumTint = [{:.3f}, {:.3f}, {:.3f}, 1.000]\n"
            "\tg_flSelfIllumScale = 0.35\n"
            "\tg_flSelfIllumBrightness = 0.0\n"
            "\tg_flSelfIllumAlbedoFactor = 0.0\n"
            "\tg_tSelfIllumMask = {}\n"
            "\tg_tColor = {}\n"
            "\tg_tNormal = {}\n"
            "\tg_tAmbientOcclusion = {}\n",
            hidden ? 1 : 0, a < 0.999f || layered ? 1 : 0, r, g, b, a, a, r, g, b, WHITE, WHITE, NORMAL, MASK);
        break;

    default:
        // csgo_unlitgeneric: one color, no light
        body = std::format(
            "\tshader = \"csgo_unlitgeneric.vfx\"\n"
            "\tF_DISABLE_Z_BUFFERING = {}\n"
            "\tF_BLEND_MODE = {}\n"
            "\tF_TRANSLUCENT = {}\n"
            "\tg_vColorTint = [{:.3f}, {:.3f}, {:.3f}, {:.3f}]\n"
            "\tg_flOpacityScale = {:.3f}\n"
            "\tg_bFogEnabled = 0\n"
            "\tg_tColor = {}\n"
            "\tg_tNormal = {}\n"
            "\tg_tAmbientOcclusion = {}\n",
            hidden ? 1 : 0, a < 0.999f || layered ? 1 : 0, a < 0.999f || layered ? 1 : 0, r, g, b, a, a, WHITE, NORMAL, MASK);
        break;
    }

    // Layered visible pass: out of the depth prepass, so the depth buffer holds the walls but not the player. All four
    // shaders (unlitgeneric, complex, effects) have the switch
    if (layered && !hidden)
        body += "\tF_DISABLE_Z_PREPASS = 1\n";

    // Behind walls: without the depth test
    return "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n{\n"
        + body + "}\n";
}

bool MaterialChams::UpdateMaterials() {
    auto p = Engine::GetProcess();

    // What each slot should be: its text, "" when off
    std::array<std::string, SLOT_COUNT> wanted{};
    auto layer = [&](Slot slot, const cfg::visuals::material_chams::group_t& group, bool hidden, bool first_person) {
        const auto& from = hidden ? group.hidden : group.visible;
        if (hidden ? Hidden(group) : from.enabled)
            wanted[slot] = MaterialText(group.material, hidden, Layered(group), from.color.r, from.color.g, from.color.b, from.color.a,
                first_person);
    };
    for (uint32_t kind = 1; kind <= CLASS_COUNT; kind++) {
        auto visible = static_cast<Slot>((kind - 1) * 2);
        layer(visible, GroupOf(kind), false, kind == HANDS || kind == WEAPON);
        if (HasHidden(kind))
            layer(static_cast<Slot>(visible + 1), GroupOf(kind), true, false);
    }

    auto now = std::chrono::steady_clock::now();
    if (wanted != this->wanted_last) {
        this->wanted_last = wanted;
        this->wanted_since = now;
    }

    // Turned off: at once, nothing to make
    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        if (wanted[slot].empty() && !this->made[slot].empty()) {
            p->write<uintptr_t>(this->page + DATA_MATERIALS + slot * sizeof(uintptr_t), 0);
            this->made[slot].clear();
        }
    }

    // Made already, or still changing
    std::vector<int> make;
    for (int slot = 0; slot < SLOT_COUNT; slot++)
        if (!wanted[slot].empty() && wanted[slot] != this->made[slot])
            make.push_back(slot);
    if (make.empty() || now - this->wanted_since < MATERIAL_SETTLE)
        return true;
    // As many as the page of a making holds, the rest on the next round
    if (make.size() > MAX_MAKE)
        make.resize(MAX_MAKE);

    if (!GameThread::Ensure())
        return true;    // Tried again later, nothing failed

    auto code_page = p->allocate_remote(MAKE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!code_page)
        return false;

    std::vector<uint8_t> data(MAKE_SIZE, 0);
    auto client = Engine::GetClient().base;
    auto create_material = p->GetModule("materialsystem2.dll").base + mat::fnCreateMaterial;
    this->generation++;

    // Per material: context, its root, the text loaded into it, the material made from it
    Code c;
    c.put({ 0x53 });                                // push rbx
    c.put({ 0x48, 0x83, 0xEC, 0x30 });              // sub rsp, 0x30
    for (size_t n = 0; n < make.size(); n++) {
        int slot = make[n];
        auto text = wanted[slot];
        auto name = std::format("cs2ext_chams_{}_{}", slot, this->generation);
        if (text.size() + 1 > TEXT_SIZE || name.size() + 1 > 0x40)
            continue;
        std::memcpy(&data[MAKE_TEXTS + n * TEXT_SIZE], text.c_str(), text.size() + 1);
        std::memcpy(&data[MAKE_NAMES + n * 0x40], name.c_str(), name.size() + 1);

        auto context = code_page + MAKE_CONTEXTS + n * CONTEXT_SIZE;
        c.put({ 0x48, 0xB9 }); c.put64(context);    // mov rcx, context
        c.put({ 0x33, 0xD2 });                      // xor edx, edx
        c.put({ 0x48, 0xB8 }); c.put64(client + mat::fnKv3Context); // mov rax, constructor
        c.put({ 0xFF, 0xD0 });                      // call rax
        c.put({ 0x48, 0xB9 }); c.put64(context);    // mov rcx, context
        c.put({ 0x48, 0xB8 }); c.put64(client + mat::fnKv3Root);    // mov rax, root
        c.put({ 0xFF, 0xD0 });                      // call rax
        c.put({ 0x48, 0x8B, 0xD8 });                // mov rbx, rax
        c.put({ 0x48, 0x85, 0xC0 });                // test rax, rax
        auto no_root = c.jump({ 0x0F, 0x84 });      // jz next

        // LoadKV3(root, &error, text, &id, "", 0)
        c.put({ 0x48, 0x8B, 0xCB });                // mov rcx, rbx
        c.put({ 0x48, 0xBA }); c.put64(code_page + MAKE_ERRORS + n * 0x10);   // mov rdx, error
        c.put({ 0x49, 0xB8 }); c.put64(code_page + MAKE_TEXTS + n * TEXT_SIZE); // mov r8, text
        c.put({ 0x49, 0xB9 }); c.put64(client + mat::kv3IdGeneric);          // mov r9, id
        c.put({ 0x48, 0xB8 }); c.put64(code_page + MAKE_EMPTY);              // mov rax, ""
        c.put({ 0x48, 0x89, 0x44, 0x24, 0x20 });    // mov [rsp + 0x20], rax
        c.put({ 0xC7, 0x44, 0x24, 0x28 }); c.put32(0);  // mov dword [rsp + 0x28], 0
        c.put({ 0x48, 0xB8 }); c.put64(this->load_kv3); // mov rax, LoadKV3
        c.put({ 0xFF, 0xD0 });                      // call rax
        c.put({ 0x84, 0xC0 });                      // test al, al
        auto not_loaded = c.jump({ 0x0F, 0x84 });   // jz next

        // CreateMaterial(system, &out, name, root, 0, true)
        c.put({ 0x48, 0xB9 }); c.put64(this->material_system);              // mov rcx, system
        c.put({ 0x48, 0xBA }); c.put64(code_page + MAKE_OUT + n * 8);       // mov rdx, out
        c.put({ 0x49, 0xB8 }); c.put64(code_page + MAKE_NAMES + n * 0x40);  // mov r8, name
        c.put({ 0x4C, 0x8B, 0xCB });                // mov r9, rbx
        c.put({ 0xC7, 0x44, 0x24, 0x20 }); c.put32(0);  // mov dword [rsp + 0x20], 0
        c.put({ 0xC7, 0x44, 0x24, 0x28 }); c.put32(1);  // mov dword [rsp + 0x28], 1
        c.put({ 0x48, 0xB8 }); c.put64(create_material); // mov rax, CreateMaterial
        c.put({ 0xFF, 0xD0 });                      // call rax

        c.land(no_root);
        c.land(not_loaded);
    }
    c.put({ 0x48, 0x83, 0xC4, 0x30 });              // add rsp, 0x30
    c.put({ 0x5B });                                // pop rbx
    c.put({ 0xC3 });                                // ret

    if (c.size() > MAKE_TEXTS) {
        p->free_remote(code_page);
        return false;
    }
    std::copy(c.bytes.begin(), c.bytes.end(), data.begin());
    p->write_bytes(code_page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(code_page), data.size());

    // Its page stays: the materials might keep pointers into the contexts
    if (!GameThread::Call(code_page, 3000)) {
        LOGF(WARNING, "Material chams: making the materials did not run in time");
        return false;
    }

    bool all = true;
    for (size_t n = 0; n < make.size(); n++) {
        int slot = make[n];
        auto handle = p->read<uintptr_t>(code_page + MAKE_OUT + n * 8);
        auto material = handle ? p->read<uintptr_t>(handle) : 0;
        if (!material) {
            all = false;
            LOGF(WARNING, "Material chams: the game made no material for slot {}", slot);
            continue;
        }

        p->write<uintptr_t>(this->page + DATA_MATERIALS + slot * sizeof(uintptr_t), material);
        this->made[slot] = wanted[slot];
        LOGF(VERBOSE, "Material chams: slot {} is material 0x{:X}", slot, material);
    }

    if (all)
        this->failures = 0;
    return all;
}

void MaterialChams::WriteTargets() {
    auto p = Engine::GetProcess();
    auto snapshot = Cache::CopySnapshot();
    const auto& local = snapshot.local;

    std::vector<uint8_t> table;
    uint32_t count = 0;

    // The scene objects of the model of an entity
    auto add = [&](uintptr_t entity, uint64_t kind) {
        auto node = entity ? p->read<uintptr_t>(entity + vis::m_pSceneNode) : 0;
        int objects = node ? std::clamp(p->read<int32_t>(node + vis::sceneNodeCount), 0, 16) : 0;
        auto list = objects ? p->read<uintptr_t>(node + vis::sceneNodeList) : 0;

        for (int k = 0; list && k < objects && count < MAX_ENTRIES; k++) {
            auto handle = p->read<uintptr_t>(list + k * sizeof(uintptr_t));
            auto object = handle ? p->read<uintptr_t>(handle + vis::sceneHandleObject) : 0;
            if (!object)
                continue;

            uint64_t entry[2] = { object, kind };
            table.insert(table.end(), reinterpret_cast<uint8_t*>(entry), reinterpret_cast<uint8_t*>(entry) + sizeof(entry));
            count++;
        }
    };

    if (vis::m_pSceneNode && vis::sceneNodeList && vis::sceneHandleObject) {
        // The bodies left undrawn first: a pawn that is one is dead, never in the players below
        {
            std::lock_guard lock(this->bodies_mutex);
            for (auto pawn : this->hidden_bodies)
                add(pawn, SKIP | NO_HIDDEN);
        }

        for (const auto& player : snapshot.players) {
            if (player.localplayer || !player.alive || count >= MAX_ENTRIES)
                continue;

            bool enemy = snapshot.game.IsEnemy(local.team, player.team);
            const auto& group = Group(enemy);
            const auto& esp = enemy ? cfg::esp::enemy : cfg::esp::team;
            if (!esp.enabled || (!group.visible.enabled && !Hidden(group)))
                continue;

            // Split per pixel when layered, else a player partly in sight is the visible material only
            bool no_hidden = player.visible && !Layered(group);
            uint64_t kind = (enemy ? ENEMY : TEAM) | (no_hidden ? NO_HIDDEN : 0);

            // Spawn protection (just respawned, deathmatch & casual): drawn like the game draws it, no chams
            auto pawn = player.GetPawnAddress();
            if (!pawn || p->read<bool>(pawn + vis::m_bGunGameImmunity))
                continue;
            add(pawn, kind);

            // Its weapons too, held or worn (the knife on the leg, the pistol at the hip): in their own material they
            // write their depth & cover the body like a wall would, the body behind them came out behind walls
            auto weapon_services = pawn ? p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices) : 0;
            int weapons = weapon_services ? std::clamp(p->read<int32_t>(weapon_services + offsets::econ::m_hMyWeapons), 0, 16) : 0;
            auto handles = weapons ? p->read<uintptr_t>(weapon_services + offsets::econ::m_hMyWeapons + 0x8) : 0;
            for (int k = 0; handles && k < weapons && count < MAX_ENTRIES; k++)
                add(Engine::GetEntityFromHandle(p->read<uint32_t>(handles + k * sizeof(uint32_t))), kind);
        }
    }

    namespace chams = cfg::visuals::material_chams;
    if (vis::m_pSceneNode && vis::sceneNodeList && vis::sceneHandleObject) {
        // Our weapons, held or worn, by entity
        auto weapons_of = [&](uintptr_t pawn) {
            std::vector<uintptr_t> list;
            auto weapon_services = pawn ? p->read<uintptr_t>(pawn + offsets::pawn::m_pWeaponServices) : 0;
            int weapons = weapon_services ? std::clamp(p->read<int32_t>(weapon_services + offsets::econ::m_hMyWeapons), 0, 16) : 0;
            auto handles = weapons ? p->read<uintptr_t>(weapon_services + offsets::econ::m_hMyWeapons + 0x8) : 0;
            for (int k = 0; handles && k < weapons; k++)
                if (auto weapon = Engine::GetEntityFromHandle(p->read<uint32_t>(handles + k * sizeof(uint32_t))))
                    list.push_back(weapon);
            return list;
        };

        // The planted bomb
        if (Wanted(chams::bomb) && snapshot.bomb.is_planted && snapshot.bomb.GetAddress())
            add(snapshot.bomb.GetAddress(), BOMB);

        // Weapons, grenades, the bomb & kits on the ground
        if (Wanted(chams::items))
            for (const auto& item : snapshot.items)
                add(item.entity, ITEMS);

        auto pawn = local.GetPawnAddress();
        if (pawn && local.alive) {
            // Our agent & what it holds or wears: only drawn in third person
            if (Wanted(chams::local)) {
                add(pawn, LOCAL);
                for (auto weapon : weapons_of(pawn))
                    add(weapon, LOCAL);
            }

            // First person: the arms entity, its children the gloves (the group of the agent, in the hands material) &
            // the models of our weapons (owned by one of them) with their own children (charms...)
            if (Wanted(chams::local) || Wanted(chams::weapon)) {
                constexpr int MAX_CHILDREN = 32;
                auto arms = Engine::GetEntityFromHandle(p->read<uint32_t>(pawn + offsets::econ::m_hHudModelArms));
                auto weapons = weapons_of(pawn);
                if (arms && Wanted(chams::local))
                    add(arms, HANDS);

                auto arms_node = arms ? p->read<uintptr_t>(arms + offsets::pawn::m_pGameSceneNode) : 0;
                auto child = arms_node ? p->read<uintptr_t>(arms_node + offsets::econ::m_pChild) : 0;
                for (int i = 0; child && i < MAX_CHILDREN; i++, child = p->read<uintptr_t>(child + offsets::econ::m_pNextSibling)) {
                    auto owner = p->read<uintptr_t>(child + offsets::econ::m_pOwner);
                    if (!owner)
                        continue;
                    auto owner_of = Engine::GetEntityFromHandle(p->read<uint32_t>(owner + offsets::econ::m_hOwnerEntity));
                    bool weapon_model = std::find(weapons.begin(), weapons.end(), owner_of) != weapons.end();
                    uint32_t kind = weapon_model ? WEAPON : HANDS;
                    if (!Wanted(GroupOf(kind)))
                        continue;
                    add(owner, kind);

                    // What hangs on it
                    auto inner = p->read<uintptr_t>(child + offsets::econ::m_pChild);
                    for (int j = 0; inner && j < MAX_CHILDREN; j++, inner = p->read<uintptr_t>(inner + offsets::econ::m_pNextSibling))
                        if (auto attached = p->read<uintptr_t>(inner + offsets::econ::m_pOwner))
                            add(attached, kind);
                }
            }
        }
    }

    // The entries first, then how many: the stub reads them any time
    if (!table.empty())
        p->write_bytes(this->page + TABLE, table);
    p->write<uint32_t>(this->page + DATA_COUNT, count);
}

void MaterialChams::Calibrate() {
    auto p = Engine::GetProcess();
    if (p->read<int32_t>(this->page + DATA_OWNER) != -1)
        return;

    uint32_t votes[OWNER_FIELDS]{};
    p->read_raw(this->page + DATA_VOTES, votes, sizeof(votes));

    // Sure: one field well ahead of the others
    int best = 0;
    for (int k = 1; k < OWNER_FIELDS; k++)
        if (votes[k] > votes[best])
            best = k;
    uint32_t second = 0;
    for (int k = 0; k < OWNER_FIELDS; k++)
        if (k != best)
            second = std::max(second, votes[k]);

    if (votes[best] >= 30 && votes[best] >= second * 4) {
        p->write<int32_t>(this->page + DATA_OWNER, best * 8);
        LOGF(INFO, "Material chams: the scene object of a mesh is at 0x{:X} ({} votes, next {})", best * 8, votes[best], second);
        SetStatus("");
        return;
    }

    // Players were in the table a while & nothing came up: said once, with the votes
    if (p->read<uint32_t>(this->page + DATA_COUNT) == 0)
        this->calibrate_since = std::chrono::steady_clock::now();
    else if (!this->calibrate_told && std::chrono::steady_clock::now() - this->calibrate_since > CALIBRATE_TELL) {
        this->calibrate_told = true;
        SetStatus(std::format("no mesh matched the players yet (votes {} {} {} {} {} {} {} {})",
            votes[0], votes[1], votes[2], votes[3], votes[4], votes[5], votes[6], votes[7]));
    }
}

void MaterialChams::Shutdown() {
    auto& i = GetInstance();
    auto p = Engine::GetProcess();

    i.stopping = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    if (!i.installed || !p)
        return;

    // The entry of the game back. The page stays, a draw might still be inside the stub
    p->write<uint32_t>(i.page + DATA_ENABLED, 0);
    p->patch_code(i.entry, &i.original, sizeof(i.original));
    i.installed = false;
}
