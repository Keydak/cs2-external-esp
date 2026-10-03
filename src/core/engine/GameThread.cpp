#include "GameThread.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"

#include <tlhelp32.h>

namespace {
    // Job at the start of the page
    enum JobState : uint32_t {
        JOB_IDLE = 0,
        JOB_QUEUED = 1,
        JOB_RUNNING = 2,
        JOB_DONE = 3,
    };

    constexpr uintptr_t JOB_STATE = 0x00;       // uint32
    constexpr uintptr_t JOB_FUNCTION = 0x08;    // uint64
    constexpr uintptr_t JOB_THREAD = 0x20;      // uint32, id of the main thread

    constexpr uintptr_t COMMON_STUB = 0x40;
    constexpr uintptr_t ENTRY_STUBS = 0x100;
    constexpr size_t ENTRY_STUB_SIZE = 16;
    constexpr size_t MAX_ENTRIES = 512;
    constexpr size_t PAGE_SIZE = 0x4000;        // Stubs & the vtable copy

    constexpr auto POLL_INTERVAL = 1ms;
}

bool GameThread::Init() {
    return GetInstance().InitImpl();
}

bool GameThread::IsAvailable() {
    return GetInstance().installed;
}

bool GameThread::Call(uintptr_t function, DWORD timeout_ms) {
    return GetInstance().CallImpl(function, timeout_ms);
}

uint32_t GameThread::FindMainThread() {
    auto p = Engine::GetProcess();

    // The first thread of the process runs the game loop
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    uint32_t result = 0;
    ULONGLONG earliest = ULLONG_MAX;

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != p->pid_)
                continue;

            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread)
                continue;

            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                auto time = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                if (time < earliest) {
                    earliest = time;
                    result = entry.th32ThreadID;
                }
            }

            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

bool GameThread::InitImpl() {
    if (this->installed)
        return true;

    auto p = Engine::GetProcess();
    auto client = Engine::GetClient();

    if (!Engine::IsInsecure() || !offsets::input::dwCSGOInput) {
        LOGF(WARNING, "Game thread calls are not available, calls into the game use a thread of their own");
        return false;
    }

    this->object = p->read<uintptr_t>(client.base + offsets::input::dwCSGOInput);
    this->vtable = this->object ? p->read<uintptr_t>(this->object) : 0;

    // Code of client.dll, from the size in its headers
    auto pe_header = client.base + p->read<int32_t>(client.base + 0x3C);
    auto image_size = p->read<uint32_t>(pe_header + 0x50);

    auto in_client = [&](uintptr_t address) {
        return address >= client.base && address < client.base + image_size;
    };

    if (!this->vtable || !in_client(this->vtable)) {
        LOGF(WARNING, "Could not read the CCSGOInput vtable, calls into the game use a thread of their own");
        return false;
    }

    std::vector<uintptr_t> entries;
    for (size_t i = 0; i < MAX_ENTRIES; i++) {
        auto entry = p->read<uintptr_t>(this->vtable + i * sizeof(uintptr_t));
        if (!in_client(entry))
            break;

        entries.push_back(entry);
    }

    auto main_thread = FindMainThread();
    if (entries.empty() || !main_thread) {
        LOGF(WARNING, "Could not prepare game thread calls ({} entries, thread {})", entries.size(), main_thread);
        return false;
    }

    this->page = p->allocate_remote(PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!this->page)
        return false;

    std::vector<uint8_t> code(PAGE_SIZE, 0xCC);
    auto put = [&](size_t& at, std::initializer_list<uint8_t> bytes) { for (auto b : bytes) code[at++] = b; };
    auto put32 = [&](size_t& at, uint32_t value) { for (int i = 0; i < 4; i++) code[at++] = static_cast<uint8_t>(value >> (i * 8)); };
    auto put64 = [&](size_t& at, uint64_t value) { for (int i = 0; i < 8; i++) code[at++] = static_cast<uint8_t>(value >> (i * 8)); };

    // Job
    size_t at = JOB_STATE;
    put32(at, JOB_IDLE);
    at = JOB_THREAD;
    put32(at, main_thread);

    // Common stub, rax holds the real function. Keeps the argument registers for it
    at = COMMON_STUB;
    put(at, { 0x50 });                                      // push rax (the real function, ret goes there)
    put(at, { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 }); // push rcx, rdx, r8, r9, r10, r11
    put(at, { 0x48, 0x83, 0xEC, 0x60 });                    // sub rsp, 0x60
    put(at, { 0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20 });        // movdqu [rsp + 0x20], xmm0
    put(at, { 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30 });        // movdqu [rsp + 0x30], xmm1
    put(at, { 0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40 });        // movdqu [rsp + 0x40], xmm2
    put(at, { 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 });        // movdqu [rsp + 0x50], xmm3

    put(at, { 0x49, 0xBB }); put64(at, this->page);         // mov r11, job
    put(at, { 0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00 }); // mov eax, gs:[0x48] (thread id)
    put(at, { 0x41, 0x3B, 0x43, static_cast<uint8_t>(JOB_THREAD) }); // cmp eax, [r11 + thread]
    put(at, { 0x75 }); size_t skip_thread = at++;           // jne done

    put(at, { 0xB8 }); put32(at, JOB_QUEUED);               // mov eax, queued
    put(at, { 0xB9 }); put32(at, JOB_RUNNING);              // mov ecx, running
    put(at, { 0xF0, 0x41, 0x0F, 0xB1, 0x0B });              // lock cmpxchg [r11], ecx
    put(at, { 0x75 }); size_t skip_job = at++;              // jne done

    put(at, { 0x49, 0x8B, 0x43, static_cast<uint8_t>(JOB_FUNCTION) }); // mov rax, [r11 + function]
    put(at, { 0xFF, 0xD0 });                                // call rax
    put(at, { 0x49, 0xBB }); put64(at, this->page);         // mov r11, job
    put(at, { 0x41, 0xC7, 0x03 }); put32(at, JOB_DONE);     // mov dword ptr [r11], done

    size_t done = at;
    code[skip_thread] = static_cast<uint8_t>(done - (skip_thread + 1));
    code[skip_job] = static_cast<uint8_t>(done - (skip_job + 1));

    put(at, { 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20 });        // movdqu xmm0, [rsp + 0x20]
    put(at, { 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30 });        // movdqu xmm1, [rsp + 0x30]
    put(at, { 0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40 });        // movdqu xmm2, [rsp + 0x40]
    put(at, { 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50 });        // movdqu xmm3, [rsp + 0x50]
    put(at, { 0x48, 0x83, 0xC4, 0x60 });                    // add rsp, 0x60
    put(at, { 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 }); // pop r11, r10, r9, r8, rdx, rcx
    put(at, { 0xC3 });                                      // ret, into the real function

    if (at > ENTRY_STUBS) {
        LOGF(WARNING, "Game thread stub too large");
        return false;
    }

    // One stub per entry: mov rax, real function; jmp common
    for (size_t i = 0; i < entries.size(); i++) {
        at = ENTRY_STUBS + i * ENTRY_STUB_SIZE;
        put(at, { 0x48, 0xB8 }); put64(at, entries[i]);
        put(at, { 0xE9 }); put32(at, static_cast<uint32_t>(static_cast<int32_t>(COMMON_STUB - (at + 4))));
    }

    // The copy, with the RTTI pointer right before it like the real one
    size_t table = ENTRY_STUBS + MAX_ENTRIES * ENTRY_STUB_SIZE;
    at = table;
    put64(at, p->read<uintptr_t>(this->vtable - sizeof(uintptr_t)));

    size_t first = at;
    for (size_t i = 0; i < entries.size(); i++)
        put64(at, this->page + ENTRY_STUBS + i * ENTRY_STUB_SIZE);

    if (at > PAGE_SIZE) {
        LOGF(WARNING, "Game thread vtable copy too large");
        return false;
    }

    p->write_bytes(this->page, code);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(this->page), code.size());

    this->copy = this->page + first;
    p->write<uintptr_t>(this->object, this->copy);
    this->installed = true;

    LOGF(INFO, "Calls into the game run on its main thread ({} entries, thread {})", entries.size(), main_thread);
    return true;
}

bool GameThread::WaitIdle(DWORD timeout_ms) {
    auto p = Engine::GetProcess();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto state = p->read<uint32_t>(this->page + JOB_STATE);

        if (state == JOB_DONE) {
            p->write<uint32_t>(this->page + JOB_STATE, JOB_IDLE);
            return true;
        }

        if (state == JOB_IDLE)
            return true;

        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        std::this_thread::sleep_for(POLL_INTERVAL);
    }
}

bool GameThread::CallImpl(uintptr_t function, DWORD timeout_ms) {
    auto p = Engine::GetProcess();

    std::lock_guard<std::mutex> lock(this->mtx);

    // The input object might not have existed yet at startup
    if (!this->installed && Engine::IsInsecure() && std::chrono::steady_clock::now() >= this->next_install) {
        this->next_install = std::chrono::steady_clock::now() + 5s;
        InitImpl();
    }

    // Without the hook, a thread of our own like before
    if (!this->installed)
        return p->call_remote(function, 0, timeout_ms);

    // The game put its own vtable back, the stubs are not called anymore
    if (p->read<uintptr_t>(this->object) != this->copy) {
        LOGF(WARNING, "The CCSGOInput vtable was replaced, calls into the game are skipped");
        return false;
    }

    // An earlier call that timed out might still be queued
    if (!WaitIdle(timeout_ms))
        return false;

    p->write<uintptr_t>(this->page + JOB_FUNCTION, function);
    p->write<uint32_t>(this->page + JOB_STATE, JOB_QUEUED);

    if (!WaitIdle(timeout_ms)) {
        LOGF(WARNING, "The game did not run our call in time, it stays queued");
        return false;
    }

    return true;
}

void GameThread::Shutdown() {
    auto& i = GetInstance();
    auto p = Engine::GetProcess();

    if (!i.installed || !p)
        return;

    std::lock_guard<std::mutex> lock(i.mtx);

    // A queued call would run after we are gone, its code stays but its data might not
    i.WaitIdle(500);

    if (p->read<uintptr_t>(i.object) == i.copy)
        p->write<uintptr_t>(i.object, i.vtable);

    // The page stays, the game might still be inside a stub
    i.installed = false;
}
