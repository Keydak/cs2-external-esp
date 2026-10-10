#include "ClanTag.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/GameThread.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Offsets.hpp"
#include "core/features/View.hpp"

#include <cstring>


namespace {
    constexpr size_t BLOCK_SIZE = 0x100;
    constexpr size_t TEXT = 0x80;           // Where the clan text goes in the block
    constexpr size_t MAX_TAG = 31;
    constexpr size_t NAME_SIZE = 128;       // m_iszPlayerName

    // setinfo: code, the old flags of "name", a CCommand (argc at 0x438, argv at 0x440), argv, the texts
    constexpr size_t SERVER_SIZE = 0x1000;
    constexpr uintptr_t SERVER_FLAGS = 0x100;
    constexpr uintptr_t SERVER_COMMAND = 0x200;
    constexpr uintptr_t COMMAND_ARGC = 0x438;
    constexpr uintptr_t COMMAND_ARGV = 0x440;
    constexpr uintptr_t SERVER_ARGV = 0x700;
    constexpr uintptr_t SERVER_TEXTS = 0x740;   // "setinfo", "name", then the value at SERVER_VALUE
    constexpr uintptr_t SERVER_VALUE = 0x780;
    constexpr size_t SERVER_VALUE_SIZE = 128;
    constexpr uintptr_t CONVAR_FLAGS = 0x30;    // uint64 - ConVarData
    constexpr uint32_t FCVAR_USERINFO = 0x200;  // What setinfo checks (bit 9)
    // The server is told a new name at most this often: each one is a name change there
    constexpr auto SERVER_EVERY = std::chrono::milliseconds(1000);

    enum Mode { STATIC, BLINK, SCROLL, TYPING, BRUTEFORCE, WAVE, FADE, DECRYPT, GLITCH, EXPAND, PULSE, SLIDE };
    enum Target { CLAN, BEFORE, AFTER, NAME };

    // Same numbers give the same random number: the animations look the same on every call of a step
    uint64_t Hash(uint64_t a, uint64_t b, uint64_t c) {
        uint64_t x = a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full ^ c * 0x165667B19E3779F9ull;
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    std::string ConfigText(const char* text, size_t size) {
        return std::string(text, strnlen(text, size));
    }

    // The names of the others in the match, separated by | so they take turns like texts of the config
    std::string PlayerNames() {
        auto snapshot = Cache::Current();

        std::vector<std::pair<int, std::string>> names;
        for (const auto& player : snapshot->players) {
            if (player.localplayer)
                continue;

            auto name = std::string(player.name, strnlen(player.name, sizeof(player.name)));
            std::replace(name.begin(), name.end(), '|', '/');
            if (!name.empty())
                names.push_back({ player.index, name });
        }

        // Same order every time, players joining only add to it
        std::sort(names.begin(), names.end());

        std::string out;
        for (const auto& [index, name] : names)
            out += (out.empty() ? "" : "|") + name;
        return out;
    }
}

bool ClanTag::Init() {
    auto& tag = GetInstance();
    if (!Engine::IsInsecure())
        return false;

    if (!offsets::controller::fnUpdateClanTag && !offsets::controller::fnUpdateName)
        return false;

    std::thread(&ClanTag::Thread, &tag).detach();
    return true;
}

bool ClanTag::IsAvailable() {
    return Engine::IsInsecure() && offsets::controller::fnUpdateClanTag;
}

bool ClanTag::IsNameAvailable() {
    return Engine::IsInsecure() && offsets::controller::fnUpdateName;
}

bool ClanTag::IsServerAvailable() {
    return Engine::IsInsecure() && offsets::controller::fnSetInfo;
}

bool ClanTag::SendName(const std::string& name) {
    auto p = Engine::GetProcess();
    if (!p || !offsets::controller::fnSetInfo || !GameThread::Ensure())
        return false;

    if (!this->name_convar) {
        this->name_convar = View::FindConVar("name");
        if (!this->name_convar) {
            LOGF(WARNING, "Clan tag: the \"name\" variable was not found, nothing sent to the server");
            return false;
        }
        LOGF(VERBOSE, "Clan tag: \"name\" at 0x{:X}, flags 0x{:X}", this->name_convar, p->read<uint64_t>(this->name_convar + CONVAR_FLAGS));
    }

    if (!this->server_block)
        this->server_block = p->allocate_remote(SERVER_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->server_block)
        return false;
    auto block = this->server_block;

    std::vector<uint8_t> data(SERVER_SIZE, 0);
    auto put = [&](uintptr_t at, const void* value, size_t size) { std::memcpy(&data[at], value, size); };
    auto put32 = [&](uintptr_t at, uint32_t value) { put(at, &value, 4); };
    auto put64 = [&](uintptr_t at, uint64_t value) { put(at, &value, 8); };

    // setinfo name <value>
    put(SERVER_TEXTS, "setinfo", 8);
    put(SERVER_TEXTS + 0x10, "name", 5);
    put(SERVER_VALUE, name.data(), std::min(name.size(), SERVER_VALUE_SIZE - 1));
    put64(SERVER_ARGV + 0x00, block + SERVER_TEXTS);
    put64(SERVER_ARGV + 0x08, block + SERVER_TEXTS + 0x10);
    put64(SERVER_ARGV + 0x10, block + SERVER_VALUE);
    put32(SERVER_COMMAND + COMMAND_ARGC, 3);
    put64(SERVER_COMMAND + COMMAND_ARGV, block + SERVER_ARGV);

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    emit({ 0x48, 0x83, 0xEC, 0x28 });                               // sub rsp, 0x28
    // "name" a user info variable for this call, its flags kept
    emit({ 0x48, 0xB8 }); emit64(this->name_convar);                // mov rax, name
    emit({ 0x48, 0x8B, 0x50, CONVAR_FLAGS });                       // mov rdx, [rax + flags]
    emit({ 0x49, 0xB8 }); emit64(block + SERVER_FLAGS);             // mov r8, &old flags
    emit({ 0x49, 0x89, 0x10 });                                     // mov [r8], rdx
    emit({ 0x48, 0x81, 0xCA, 0x00, 0x02, 0x00, 0x00 });             // or rdx, user info
    emit({ 0x48, 0x89, 0x50, CONVAR_FLAGS });                       // mov [rax + flags], rdx
    // setinfo(0, &command)
    emit({ 0x31, 0xC9 });                                           // xor ecx, ecx
    emit({ 0x48, 0xBA }); emit64(block + SERVER_COMMAND);           // mov rdx, &command
    emit({ 0x48, 0xB8 }); emit64(Engine::GetEngine().base + offsets::controller::fnSetInfo); // mov rax, setinfo
    emit({ 0xFF, 0xD0 });                                           // call rax
    // Its flags back
    emit({ 0x48, 0xB8 }); emit64(this->name_convar);                // mov rax, name
    emit({ 0x49, 0xB8 }); emit64(block + SERVER_FLAGS);             // mov r8, &old flags
    emit({ 0x49, 0x8B, 0x10 });                                     // mov rdx, [r8]
    emit({ 0x48, 0x89, 0x50, CONVAR_FLAGS });                       // mov [rax + flags], rdx
    emit({ 0x48, 0x83, 0xC4, 0x28 });                               // add rsp, 0x28
    emit({ 0x33, 0xC0 });                                           // xor eax, eax
    emit({ 0xC3 });                                                 // ret

    if (code.size() > SERVER_FLAGS)
        return false;
    std::copy(code.begin(), code.end(), data.begin());
    p->write_bytes(block, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(block), code.size());

    if (!GameThread::Call(block)) {
        // Might still be running, never written over
        this->server_block = 0;
        LOGF(WARNING, "Clan tag: the game did not send the name in time");
        return false;
    }
    LOGF(VERBOSE, "Clan tag: sent the name \"{}\" to the server", name);
    return true;
}

void ClanTag::Shutdown() {
    auto& tag = GetInstance();
    tag.stopping = true;
    std::this_thread::sleep_for(100ms);
    tag.Restore();
}

const std::vector<const char*>& ClanTag::GetModes() {
    static const std::vector<const char*> modes = {
        "Static", "Blink", "Scroll",
        "Typing", "Bruteforce", "Wave",
        "Fade", "Decrypt", "Glitch",
        "Expand", "Pulse", "Slide",
    };
    return modes;
}

size_t ClanTag::Cycle(size_t length, int mode) {
    switch (mode) {
    case BLINK:         return 4;
    case SCROLL:        return length + 3;
    case TYPING:        return length * 2 + 4;
    case BRUTEFORCE:    return length + 6;
    case WAVE:          return length;
    case FADE:          return length * 2 + 8;
    case DECRYPT:       return length + 8;
    case GLITCH:        return 8;
    case EXPAND:        return length * 2 + 3;
    case PULSE:         return 6;
    case SLIDE:         return length * 2 + 3;
    default:            return 8;
    }
}

std::string ClanTag::Frame(const std::string& text, int mode, size_t at, uint64_t round) {
    static constexpr char guesses[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&@!?";
    static constexpr char symbols[] = "#$%&@!?*/\\<>";

    size_t length = text.size();
    auto guess = [&](uint64_t a, uint64_t b) { return guesses[Hash(round, a, b) % (sizeof(guesses) - 1)]; };

    // Order the letters come in, shuffled every round: order[i] = when letter i comes
    auto order = [&]() {
        std::vector<size_t> rank(length);
        std::vector<size_t> indices(length);
        for (size_t i = 0; i < length; i++)
            indices[i] = i;
        for (size_t i = length; i > 1; i--)
            std::swap(indices[i - 1], indices[Hash(round, 0x5EED, i) % i]);
        for (size_t i = 0; i < length; i++)
            rank[indices[i]] = i;
        return rank;
    };

    switch (mode) {
    case BLINK:
        return at % 2 ? std::string() : text;

    case SCROLL: {
        // The text moves left through a window of its own length, a gap between the rounds
        auto ring = text + "   ";
        return (ring + ring).substr(at % ring.size(), length);
    }

    case TYPING: {
        // Typed in with a cursor, held, deleted again
        if (at < length)
            return text.substr(0, at) + "_";
        if (at < length + 4)
            return text;
        return text.substr(0, length * 2 + 4 - at);
    }

    case BRUTEFORCE: {
        // Letters found one by one left to right, the rest still being guessed
        size_t found = std::min(at, length);
        std::string out = text;
        for (size_t i = found; i < length; i++)
            if (out[i] != ' ')
                out[i] = guess(at, i);
        return out;
    }

    case WAVE: {
        // One capital moving through the text
        std::string out = text;
        for (size_t i = 0; i < length; i++) {
            auto c = static_cast<unsigned char>(out[i]);
            out[i] = static_cast<char>(i == at ? toupper(c) : tolower(c));
        }
        return out;
    }

    case FADE: {
        // Every letter fades in on its own time ( . : letter), held, then fades out the same way
        auto rank = order();
        std::string out(length, ' ');

        for (size_t i = 0; i < length; i++) {
            if (text[i] == ' ')
                continue;

            int stage;
            if (at < length + 2)
                stage = static_cast<int>(at) - static_cast<int>(rank[i]);           // -1 nothing, 0 ., 1 :, 2 letter
            else if (at < length + 6)
                stage = 2;
            else
                stage = 1 - (static_cast<int>(at - length - 6) - static_cast<int>(rank[i])); // 2+ letter, 1 :, 0 ., -1 nothing

            out[i] = stage <= -1 ? ' ' : stage == 0 ? '.' : stage == 1 ? ':' : text[i];
        }

        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    case DECRYPT: {
        // Everything guessed, the letters lock in random order
        auto rank = order();
        std::string out = text;
        for (size_t i = 0; i < length; i++)
            if (out[i] != ' ' && at < rank[i] + 2)
                out[i] = guess(at, i);
        return out;
    }

    case GLITCH: {
        // Mostly clean, now & then a letter or two turn into symbols
        std::string out = text;
        auto h = Hash(round, at, 0x6117C4);
        if (length && h % 3 == 0) {
            for (uint64_t n = 0; n < 1 + (h >> 8) % 2; n++) {
                auto i = Hash(round, at, n) % length;
                out[i] = symbols[Hash(round, at, n + 7) % (sizeof(symbols) - 1)];
            }
        }
        return out;
    }

    case EXPAND: {
        // Grows out of its middle, held, shrinks back
        size_t shown = at < length ? at + 1 : at < length + 3 ? length : length * 2 + 2 - at;
        shown = std::min(shown, length);
        return text.substr((length - shown) / 2, shown);
    }

    case PULSE: {
        static const char* frames[] = { "", "-", "=", "#", "=", "-" };
        auto frame = frames[at % 6];
        return frame + text + frame;
    }

    case SLIDE: {
        // Comes in from the left (its end first), held, goes out to the right
        if (at < length)
            return text.substr(length - at - 1);
        if (at < length + 3)
            return text;
        return text.substr(0, length * 2 + 2 - at);
    }

    default:
        return text;
    }
}

std::string ClanTag::Animate(const std::string& config, int mode, uint64_t step) {
    // Several texts separated by |, each one a whole animation then the next
    std::vector<std::string> texts;
    size_t start = 0;
    while (start <= config.size()) {
        auto end = config.find('|', start);
        if (end == std::string::npos)
            end = config.size();

        auto text = config.substr(start, end - start);
        if (!text.empty())
            texts.push_back(text);
        start = end + 1;
    }

    if (texts.empty())
        return {};

    uint64_t total = 0;
    for (auto& text : texts)
        total += std::max<size_t>(Cycle(text.size(), mode), 1);

    uint64_t round = step / total;
    uint64_t at = step % total;

    for (size_t i = 0; i < texts.size(); i++) {
        size_t cycle = std::max<size_t>(Cycle(texts[i].size(), mode), 1);
        if (at < cycle)
            return Frame(texts[i], mode, static_cast<size_t>(at), round * texts.size() + i);
        at -= cycle;
    }

    return texts.front();
}

std::string ClanTag::ReadName(uintptr_t controller) {
    char name[NAME_SIZE]{};
    Engine::GetProcess()->read_raw(controller + offsets::controller::m_iszPlayerName, name, sizeof(name) - 1);
    return std::string(name, strnlen(name, sizeof(name)));
}

bool ClanTag::Apply(uintptr_t controller, std::optional<uintptr_t> clan, const std::string* name) {
    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (clan && !offsets::controller::fnUpdateClanTag)
        clan.reset();
    if (name && !offsets::controller::fnUpdateName)
        name = nullptr;
    if (!clan && !name)
        return true;

    if (!this->block)
        this->block = p->allocate_remote(BLOCK_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->block)
        return false;

    // The name is a char array in the controller, the game cleans it into the name the scoreboard shows
    if (name) {
        std::vector<uint8_t> bytes(NAME_SIZE, 0);
        memcpy(bytes.data(), name->data(), std::min(name->size(), NAME_SIZE - 1));
        p->write_bytes(controller + offsets::controller::m_iszPlayerName, bytes);
    }

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit32 = [&](uint32_t value) { for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    // Only while it is still our controller: queued calls can run a moment later, the map might be gone by then
    emit({ 0x48, 0x83, 0xEC, 0x28 });                                                   // sub rsp, 0x28
    emit({ 0x48, 0xB8 }); emit64(client.base + offsets::localPlayerController);         // mov rax, &local controller
    emit({ 0x48, 0x8B, 0x00 });                                                         // mov rax, [rax]
    emit({ 0x48, 0xB9 }); emit64(controller);                                           // mov rcx, controller
    emit({ 0x48, 0x39, 0xC8 });                                                         // cmp rax, rcx
    emit({ 0x75, 0x00 });                                                               // jne done
    size_t skip = code.size() - 1;

    // m_szClan points to the source, the game cleans it the same way whenever it wants
    if (clan) {
        emit({ 0x48, 0xB8 }); emit64(*clan);                                            // mov rax, source
        emit({ 0x48, 0x89, 0x81 }); emit32(static_cast<uint32_t>(offsets::controller::m_szClan)); // mov [rcx + m_szClan], rax
        emit({ 0x49, 0xBB }); emit64(client.base + offsets::controller::fnUpdateClanTag); // mov r11, UpdateClanTag
        emit({ 0x41, 0xFF, 0xD3 });                                                     // call r11
    }

    if (name) {
        emit({ 0x48, 0xB9 }); emit64(controller);                                       // mov rcx, controller
        emit({ 0x49, 0xBB }); emit64(client.base + offsets::controller::fnUpdateName);  // mov r11, UpdateName
        emit({ 0x41, 0xFF, 0xD3 });                                                     // call r11
    }

    code[skip] = static_cast<uint8_t>(code.size() - (skip + 1));
    emit({ 0x48, 0x83, 0xC4, 0x28 });                                                   // done: add rsp, 0x28
    emit({ 0x33, 0xC0 });                                                               // xor eax, eax
    emit({ 0xC3 });                                                                     // ret

    p->write_bytes(this->block, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->block), code.size());

    if (!GameThread::Call(this->block)) {
        // Cancelled, but the game might still be in it: a new block next time
        this->block = 0;
        return false;
    }

    return true;
}

void ClanTag::Restore() {
    // The real name back on the server
    if (this->server_active && !this->original_name.empty() && SendName(this->original_name)) {
        this->server_active = false;
        this->server_sent.clear();
    }

    if (!this->controller)
        return;

    auto p = Engine::GetProcess();
    if (p && p->read<uintptr_t>(Engine::GetClient().base + offsets::localPlayerController) == this->controller) {
        std::optional<uintptr_t> clan;
        if (this->clan_active)
            clan = this->original_clan;

        Apply(this->controller, clan, this->name_active ? &this->original_name : nullptr);
    }

    this->controller = 0;
    this->clan_active = false;
    this->name_active = false;
    this->clan_written.clear();
}

void ClanTag::Thread() {
    auto started = std::chrono::steady_clock::now();

    while (!this->stopping) {
        std::this_thread::sleep_for(50ms);

        auto p = Engine::GetProcess();
        if (!p)
            continue;

        auto controller = p->read<uintptr_t>(Engine::GetClient().base + offsets::localPlayerController);

        // New map: a new controller with its own tag & name, nothing to put back on the old one. The server keeps the
        // name we sent across maps of the same server: sent again from the new controller
        if (controller != this->controller && this->controller) {
            this->controller = 0;
            this->clan_active = false;
            this->name_active = false;
            this->clan_written.clear();
            this->server_sent.clear();
        }

        bool tag_on = cfg::misc::clantag && IsAvailable();
        int target = IsNameAvailable() ? std::clamp(cfg::misc::clantag_target, 0, 3) : CLAN;
        bool name_on = cfg::misc::name_change && IsNameAvailable();

        if (!controller || (!tag_on && !name_on) || !GameThread::Ensure()) {
            Restore();
            continue;
        }

        if (!this->controller) {
            this->controller = controller;
            this->original_clan = p->read<uintptr_t>(controller + offsets::controller::m_szClan);
            // The server might still have our name from before: the real one stays the one seen first
            if (!this->server_active || this->original_name.empty())
                this->original_name = ReadName(controller);
        }

        // The tag at this step of the animation
        std::string tag;
        if (tag_on) {
            float speed = std::clamp(cfg::misc::clantag_speed, 100.f, 1500.f);
            auto elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
            auto texts = cfg::misc::clantag_source == 1
                ? PlayerNames()
                : ConfigText(cfg::misc::clantag_text, sizeof(cfg::misc::clantag_text));
            tag = Animate(texts, cfg::misc::clantag_mode, static_cast<uint64_t>(elapsed / speed));
        }

        // Clan slot: our tag with brackets, else the one of the game
        std::optional<uintptr_t> clan;
        bool want_clan = tag_on && target == CLAN;

        if (want_clan) {
            if (!this->block)
                this->block = p->allocate_remote(BLOCK_SIZE, PAGE_EXECUTE_READWRITE);
            if (!this->block)
                continue;

            bool pointed = p->read<uintptr_t>(controller + offsets::controller::m_szClan) == this->block + TEXT;
            if (!this->clan_active || tag != this->clan_written || !pointed) {
                std::vector<uint8_t> bytes(tag.begin(), tag.begin() + std::min(tag.size(), MAX_TAG));
                bytes.push_back(0);
                p->write_bytes(this->block + TEXT, bytes);
                clan = this->block + TEXT;
            }
        } else if (this->clan_active)
            clan = this->original_clan;

        // Name: ours or the real one, the tag around it or instead of it
        std::string name = name_on && cfg::misc::name_text[0]
            ? ConfigText(cfg::misc::name_text, sizeof(cfg::misc::name_text))
            : this->original_name;

        if (tag_on && target == BEFORE && !tag.empty())
            name = tag + " " + name;
        else if (tag_on && target == AFTER && !tag.empty())
            name = name + " " + tag;
        else if (tag_on && target == NAME)
            name = tag.empty() ? std::string(" ") : tag;

        bool want_name = name_on || (tag_on && target != CLAN);
        const std::string* name_write = nullptr;

        if (want_name) {
            if (!this->name_active || ReadName(controller) != name)
                name_write = &name;
        } else if (this->name_active)
            name_write = &this->original_name;

        // To the server: the whole name, the tag in it ([tag] name for the clan slot, the server takes clans from
        // Steam groups only). Not more often than SERVER_EVERY, the newest one
        bool server_on = IsServerAvailable();
        if (server_on) {
            std::string server_name = name;
            if (tag_on && target == CLAN && !tag.empty())
                server_name = "[" + tag + "] " + name;
            auto now = std::chrono::steady_clock::now();
            if (server_name != this->server_sent && now - this->server_last >= SERVER_EVERY) {
                this->server_last = now;
                if (SendName(server_name)) {
                    this->server_sent = server_name;
                    this->server_active = true;
                }
            }
        }
        else if (this->server_active && SendName(this->original_name)) {
            this->server_active = false;
            this->server_sent.clear();
        }

        if (!clan && !name_write)
            continue;

        if (Apply(controller, clan, name_write)) {
            if (clan) {
                this->clan_active = want_clan;
                this->clan_written = want_clan ? tag : std::string();
            }
            if (name_write)
                this->name_active = want_name;
        }
    }
}
