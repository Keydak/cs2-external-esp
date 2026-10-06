#include "VoteEvents.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/GameThread.hpp"
#include "core/offsets/Offsets.hpp"

namespace {
    // Our page
    constexpr uintptr_t LISTENER = 0x000;       // The listener object, its vtable pointer first
    constexpr uintptr_t PAGE_MAGIC = 0x008;     // uint64
    constexpr uint64_t MAGIC = 0x53544E4556544F56; // "VOTEVNTS"
    constexpr uintptr_t COUNT = 0x010;          // uint32, events written so far
    constexpr uintptr_t ADDED = 0x014;          // uint8 each, what AddListener said per event name
    constexpr uintptr_t VTABLE = 0x040;
    constexpr size_t VTABLE_ENTRIES = 16;
    constexpr uintptr_t KEYS = 0x0C0;           // Key symbols of the fields, 16 bytes each
    constexpr uintptr_t STRINGS = 0x120;        // Event & field names
    constexpr uintptr_t NOTHING = 0x200;        // xor eax, eax; ret, every other entry of the vtable
    constexpr uintptr_t FIRE = 0x210;           // FireGameEvent
    constexpr uintptr_t ADD = 0x380;            // Run on the game thread: adds the listener
    constexpr uintptr_t REMOVE = 0x440;         // Run on the game thread: removes it
    constexpr uintptr_t RING = 0x800;
    constexpr size_t RING_SIZE = 64;            // Records, a power of two
    constexpr size_t RECORD_SIZE = 32;
    constexpr size_t PAGE_SIZE = 0x1000;

    // A record: the event name, then the fields, -1 when the event has none
    constexpr uint8_t RECORD_NAME = 0x00;       // const char*
    constexpr uint8_t RECORD_OPTION = 0x08;
    constexpr uint8_t RECORD_TEAM = 0x0C;
    constexpr uint8_t RECORD_SLOT = 0x10;
    constexpr uint8_t RECORD_YES = 0x14;
    constexpr uint8_t RECORD_NO = 0x18;
    constexpr uint8_t RECORD_POTENTIAL = 0x1C;

    // IGameEvent
    constexpr uint8_t EVENT_GET_NAME = 0x08;
    constexpr uint8_t EVENT_GET_INT = 0x38;     // (key, default)
    constexpr uint8_t EVENT_GET_SLOT = 0x78;    // (CPlayerSlot* out, key)

    // CGameEventManager
    constexpr uint8_t MANAGER_ADD_LISTENER = 0x18;      // (listener, name, server side)
    constexpr uint8_t MANAGER_REMOVE_LISTENER = 0x28;   // (listener)

    const char* EVENTS[] = { "vote_cast", "vote_changed" };

    enum Key { OPTION, TEAM, USERID, OPTION1, OPTION2, POTENTIAL, KEY_COUNT };
    const char* KEY_NAMES[KEY_COUNT] = { "vote_option", "team", "userid", "vote_option1", "vote_option2", "potentialVotes" };

    // How the game hashes the names of event fields: MurmurHash2 of the lower case name
    uint32_t HashKey(std::string_view name) {
        constexpr uint32_t m = 0x5BD1E995;
        uint32_t h = 0x31415926 ^ static_cast<uint32_t>(name.size());

        auto lower = [](char c) { return static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c + 0x20 : c); };

        size_t i = 0;
        for (; i + 4 <= name.size(); i += 4) {
            uint32_t k = lower(name[i]) | lower(name[i + 1]) << 8 | lower(name[i + 2]) << 16 | lower(name[i + 3]) << 24;
            k *= m; k ^= k >> 24; k *= m;
            h *= m; h ^= k;
        }

        switch (name.size() - i) {
        case 3: h ^= lower(name[i + 2]) << 16; [[fallthrough]];
        case 2: h ^= lower(name[i + 1]) << 8; [[fallthrough]];
        case 1: h ^= lower(name[i]); h *= m;
        }

        h ^= h >> 13; h *= m; h ^= h >> 15;
        return h;
    }
}

bool VoteEvents::Init() {
    if (!Engine::IsInsecure() || !offsets::votes::dwGameEventManager)
        return false;

    std::thread(&VoteEvents::Thread, &GetInstance()).detach();
    return true;
}

bool VoteEvents::IsAvailable() {
    return GetInstance().registered;
}

std::vector<VoteEvents::Event> VoteEvents::Take() {
    auto& i = GetInstance();
    std::lock_guard<std::mutex> lock(i.mtx);
    return std::exchange(i.pending, {});
}

void VoteEvents::Shutdown() {
    auto& i = GetInstance();
    i.stopping = true;
    std::this_thread::sleep_for(100ms);

    // The page stays, the game might be inside our FireGameEvent right now
    if (i.registered && i.Register(false))
        i.registered = false;
}

void VoteEvents::Thread() {
    auto next_try = std::chrono::steady_clock::now();

    while (!this->stopping) {
        std::this_thread::sleep_for(50ms);

        bool wanted = cfg::world::votes::enabled && cfg::world::votes::names;

        // Turned off: our code is left alone, the game does not call it anymore
        if (!wanted) {
            if (this->registered && Register(false))
                this->registered = false;
            continue;
        }

        if (!this->registered) {
            // The call waits for the main thread, which does not run it while the game loads
            if (std::chrono::steady_clock::now() < next_try)
                continue;
            next_try = std::chrono::steady_clock::now() + 5s;

            // A page that was not fully written must never be run: freed, built again next time
            if (!this->page && !Build()) {
                if (auto p = Engine::GetProcess(); p && this->page)
                    p->free_remote(this->page);
                this->page = 0;
                continue;
            }

            this->registered = Register(true);
            continue;
        }

        Drain();
    }
}

bool VoteEvents::Build() {
    auto p = Engine::GetProcess();
    if (!p)
        return false;

    this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return false;

    std::vector<uint8_t> code(PAGE_SIZE, 0);
    size_t at = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (auto b : bytes) code[at++] = b; };
    auto put32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code[at++] = static_cast<uint8_t>(value >> (i * 8)); };
    auto put64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code[at++] = static_cast<uint8_t>(value >> (i * 8)); };

    // The listener, nothing in it but its vtable
    at = LISTENER;
    put64(this->page + VTABLE);
    at = PAGE_MAGIC;
    put64(MAGIC);

    // Names, the key symbols point at theirs
    at = STRINGS;
    uintptr_t event_names[std::size(EVENTS)]{};
    for (size_t i = 0; i < std::size(EVENTS); i++) {
        event_names[i] = this->page + at;
        for (const char* c = EVENTS[i]; *c; c++)
            code[at++] = static_cast<uint8_t>(*c);
        code[at++] = 0;
    }

    uintptr_t key_names[KEY_COUNT]{};
    for (int i = 0; i < KEY_COUNT; i++) {
        key_names[i] = this->page + at;
        for (const char* c = KEY_NAMES[i]; *c; c++)
            code[at++] = static_cast<uint8_t>(*c);
        code[at++] = 0;
    }

    if (at > NOTHING) {
        LOGF(WARNING, "Vote events: names too long");
        return false;
    }

    // Key symbol: hash, -1, name
    auto key = [&](int i) { return this->page + KEYS + i * 16; };
    for (int i = 0; i < KEY_COUNT; i++) {
        at = KEYS + i * 16;
        put32(HashKey(KEY_NAMES[i]));
        put32(0xFFFFFFFF);
        put64(key_names[i]);
    }

    at = NOTHING;
    put({ 0x31, 0xC0, 0xC3 });                                  // xor eax, eax; ret

    at = VTABLE;
    for (size_t i = 0; i < VTABLE_ENTRIES; i++)
        put64(this->page + (i == 1 ? FIRE : NOTHING));          // 0 destructor, 1 FireGameEvent

    // FireGameEvent(this, event)
    at = FIRE;
    put({ 0x53, 0x56, 0x57 });                                  // push rbx, rsi, rdi
    put({ 0x48, 0x83, 0xEC, 0x20 });                            // sub rsp, 0x20
    put({ 0x48, 0x89, 0xD3 });                                  // mov rbx, rdx (event)
    put({ 0x48, 0xBE }); put64(this->page);                     // mov rsi, page
    put({ 0x8B, 0x46, static_cast<uint8_t>(COUNT) });           // mov eax, [rsi + count]
    put({ 0x83, 0xE0, static_cast<uint8_t>(RING_SIZE - 1) });   // and eax, ring size - 1
    put({ 0xC1, 0xE0, 0x05 });                                  // shl eax, 5 (record size)
    put({ 0x48, 0x8D, 0xBC, 0x06 }); put32(static_cast<uint32_t>(RING)); // lea rdi, [rsi + rax + ring]
    put({ 0x48, 0xC7, 0x47, 0x08 }); put32(0xFFFFFFFF);         // mov qword [rdi + 8], -1
    put({ 0x48, 0xC7, 0x47, 0x10 }); put32(0xFFFFFFFF);         // mov qword [rdi + 16], -1
    put({ 0x48, 0xC7, 0x47, 0x18 }); put32(0xFFFFFFFF);         // mov qword [rdi + 24], -1

    put({ 0x48, 0x8B, 0x03 });                                  // mov rax, [rbx]
    put({ 0x48, 0x89, 0xD9 });                                  // mov rcx, rbx
    put({ 0xFF, 0x50, EVENT_GET_NAME });                        // call [rax + GetName]
    put({ 0x48, 0x89, 0x07 });                                  // mov [rdi], rax
    put({ 0x48, 0x85, 0xC0 });                                  // test rax, rax
    put({ 0x0F, 0x84 }); size_t no_name = at; put32(0);         // jz commit

    // "vote_cast" or "vote_changed": the 7th letter tells them apart
    put({ 0x80, 0x78, 0x06, 'a' });                             // cmp byte [rax + 6], 'a'
    put({ 0x0F, 0x85 }); size_t not_cast = at; put32(0);        // jne changed

    auto get_int = [&](int k, uint8_t field) {
        put({ 0x48, 0x8B, 0x03 });                              // mov rax, [rbx]
        put({ 0x48, 0x89, 0xD9 });                              // mov rcx, rbx
        put({ 0x48, 0xBA }); put64(key(k));                     // mov rdx, key
        put({ 0x41, 0xB8 }); put32(0xFFFFFFFF);                 // mov r8d, -1
        put({ 0xFF, 0x50, EVENT_GET_INT });                     // call [rax + GetInt]
        put({ 0x89, 0x47, field });                             // mov [rdi + field], eax
    };

    get_int(OPTION, RECORD_OPTION);
    get_int(TEAM, RECORD_TEAM);
    put({ 0x48, 0x8B, 0x03 });                                  // mov rax, [rbx]
    put({ 0x48, 0x89, 0xD9 });                                  // mov rcx, rbx
    put({ 0x48, 0x8D, 0x57, RECORD_SLOT });                     // lea rdx, [rdi + slot]
    put({ 0x49, 0xB8 }); put64(key(USERID));                    // mov r8, key
    put({ 0xFF, 0x50, EVENT_GET_SLOT });                        // call [rax + GetPlayerSlot]
    put({ 0xE9 }); size_t cast_done = at; put32(0);             // jmp commit

    size_t changed = at;
    get_int(OPTION1, RECORD_YES);
    get_int(OPTION2, RECORD_NO);
    get_int(POTENTIAL, RECORD_POTENTIAL);

    size_t commit = at;
    put({ 0xF0, 0xFF, 0x46, static_cast<uint8_t>(COUNT) });     // lock inc dword [rsi + count]
    put({ 0x48, 0x83, 0xC4, 0x20 });                            // add rsp, 0x20
    put({ 0x5F, 0x5E, 0x5B });                                  // pop rdi, rsi, rbx
    put({ 0xC3 });                                              // ret

    auto patch = [&](size_t from, size_t to) {
        auto rel = static_cast<uint32_t>(static_cast<int32_t>(to - (from + 4)));
        for (int i = 0; i < 4; i++)
            code[from + i] = static_cast<uint8_t>(rel >> (i * 8));
    };
    patch(no_name, commit);
    patch(not_cast, changed);
    patch(cast_done, commit);

    if (at > ADD) {
        LOGF(WARNING, "Vote events: FireGameEvent too large");
        return false;
    }

    auto client = Engine::GetClient();
    uintptr_t manager = client.base + offsets::votes::dwGameEventManager;

    // AddListener(listener, name, false) for each event, what it said into the page
    at = ADD;
    put({ 0x48, 0x83, 0xEC, 0x28 });                            // sub rsp, 0x28
    std::vector<size_t> skips;
    for (size_t i = 0; i < std::size(EVENTS); i++) {
        put({ 0x48, 0xB8 }); put64(manager);                    // mov rax, &manager
        put({ 0x48, 0x8B, 0x08 });                              // mov rcx, [rax]
        put({ 0x48, 0x85, 0xC9 });                              // test rcx, rcx
        put({ 0x0F, 0x84 }); skips.push_back(at); put32(0);     // jz done
        put({ 0x48, 0xBA }); put64(this->page + LISTENER);      // mov rdx, listener
        put({ 0x49, 0xB8 }); put64(event_names[i]);             // mov r8, name
        put({ 0x45, 0x31, 0xC9 });                              // xor r9d, r9d
        put({ 0x48, 0x8B, 0x01 });                              // mov rax, [rcx]
        put({ 0xFF, 0x50, MANAGER_ADD_LISTENER });              // call [rax + AddListener]
        put({ 0x48, 0xBA }); put64(this->page + ADDED + i);     // mov rdx, &added
        put({ 0x88, 0x02 });                                    // mov [rdx], al
    }
    for (auto skip : skips)
        patch(skip, at);
    put({ 0x48, 0x83, 0xC4, 0x28 });                            // done: add rsp, 0x28
    put({ 0x31, 0xC0, 0xC3 });                                  // xor eax, eax; ret

    if (at > REMOVE) {
        LOGF(WARNING, "Vote events: AddListener call too large");
        return false;
    }

    // RemoveListener(listener)
    at = REMOVE;
    put({ 0x48, 0x83, 0xEC, 0x28 });                            // sub rsp, 0x28
    put({ 0x48, 0xB8 }); put64(manager);                        // mov rax, &manager
    put({ 0x48, 0x8B, 0x08 });                                  // mov rcx, [rax]
    put({ 0x48, 0x85, 0xC9 });                                  // test rcx, rcx
    put({ 0x74 }); size_t no_manager = at++;                    // jz done
    put({ 0x48, 0xBA }); put64(this->page + LISTENER);          // mov rdx, listener
    put({ 0x48, 0x8B, 0x01 });                                  // mov rax, [rcx]
    put({ 0xFF, 0x50, MANAGER_REMOVE_LISTENER });               // call [rax + RemoveListener]
    code[no_manager] = static_cast<uint8_t>(at - (no_manager + 1));
    put({ 0x48, 0x83, 0xC4, 0x28 });                            // done: add rsp, 0x28
    put({ 0x31, 0xC0, 0xC3 });                                  // xor eax, eax; ret

    if (at > RING) {
        LOGF(WARNING, "Vote events: RemoveListener call too large");
        return false;
    }

    p->write_bytes(this->page, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), code.size());
    return true;
}

bool VoteEvents::Register(bool add) {
    auto p = Engine::GetProcess();
    if (!p || !this->page)
        return false;

    if (add)
        p->write<uint16_t>(this->page + ADDED, 0xFFFF);    // Stays when the manager does not exist yet

    // Each step logged before it is taken, to tell where the game would crash
    auto manager = p->read<uintptr_t>(Engine::GetClient().base + offsets::votes::dwGameEventManager);
    LOGF(VERBOSE, "Vote events: {} the listener at 0x{:X}, manager 0x{:X}", add ? "adding" : "removing", this->page, manager);
    if (!manager)
        return false;

    if (!GameThread::Call(this->page + (add ? ADD : REMOVE)))
        return false;

    if (!add) {
        LOGF(VERBOSE, "Vote events: listener removed");
        return true;
    }

    // AddListener gives -1 for a name it does not know
    auto added = p->read<uint16_t>(this->page + ADDED);
    int cast = added & 0xFF, changed = added >> 8;
    LOGF(VERBOSE, "Vote events: listener at 0x{:X}, vote_cast {}, vote_changed {}", this->page, cast, changed);

    this->read = p->read<uint32_t>(this->page + COUNT);
    return cast != 0xFF || changed != 0xFF;
}

void VoteEvents::ReadPlayer(int slot, Event& event) {
    auto p = Engine::GetProcess();
    if (slot < 0 || slot >= 64)
        return;

    // Controllers are entities 1 to 64, in the first chunk of the list
    auto entity_list = p->read<uintptr_t>(Engine::GetClient().base + offsets::entityList);
    auto list_entry = entity_list ? p->read<uintptr_t>(entity_list + 0x10) : 0;
    auto controller = list_entry ? p->read<uintptr_t>(list_entry + (slot + 1) * 0x70) : 0;
    if (!controller)
        return;

    char name[32]{};
    p->read_raw(controller + offsets::controller::m_iszPlayerName, name, sizeof(name) - 1);
    event.player = name;
    event.steam_id = p->read<uint64_t>(controller + offsets::controller::m_steamID);
}

void VoteEvents::Drain() {
    auto p = Engine::GetProcess();
    if (!p)
        return;

    auto count = p->read<uint32_t>(this->page + COUNT);
    if (count == this->read)
        return;

    // Fell behind a whole ring, only the newest are still there
    if (count - this->read > RING_SIZE)
        this->read = count - RING_SIZE;

    std::vector<Event> events;
    for (; this->read != count; this->read++) {
        uint8_t record[RECORD_SIZE]{};
        p->read_raw(this->page + RING + (this->read % RING_SIZE) * RECORD_SIZE, record, sizeof(record));

        auto field = [&](uint8_t offset) { return *reinterpret_cast<int32_t*>(record + offset); };

        // The event name is often gone by the time we read it. Only vote_changed has the counts, the code in the game
        // reads them for it alone
        Event event;
        event.cast = field(RECORD_POTENTIAL) < 0;
        event.option = field(RECORD_OPTION);
        event.team = field(RECORD_TEAM);
        event.slot = field(RECORD_SLOT);
        event.yes = field(RECORD_YES);
        event.no = field(RECORD_NO);
        event.potential = field(RECORD_POTENTIAL);
        if (event.cast)
            ReadPlayer(event.slot, event);

        LOGF(VERBOSE, "Vote event {}: option {} team {} slot {} ({}) yes {} no {} of {}", event.cast ? "cast" : "changed", event.option, event.team,
            event.slot, event.player, event.yes, event.no, event.potential);
        events.push_back(std::move(event));
    }

    std::lock_guard<std::mutex> lock(this->mtx);
    this->pending.insert(this->pending.end(), events.begin(), events.end());

    // Nobody takes them while the vote list is off
    if (this->pending.size() > 256)
        this->pending.erase(this->pending.begin(), this->pending.end() - 256);
}
