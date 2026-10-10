#include "Sounds.hpp"

#include "updater/http/HttpHelper.hpp"
#include "core/engine/Engine.hpp"
#include "core/engine/GameThread.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Offsets.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {
    // Our assets repository: the sounds are in its sounds folder (the agents of the preview in another)
    constexpr auto REPOSITORY_TREE = "https://api.github.com/repos/Keydak/cs2-external-assets/git/trees/main?recursive=1";
    constexpr auto REPOSITORY_FILES = "https://raw.githubusercontent.com/Keydak/cs2-external-assets/main/";
    constexpr auto REPOSITORY_SOUNDS = "sounds/";

    const std::filesystem::path cache_dir = "cache/sounds";

    constexpr int VOICES = 10;      // Of one sound playing at once (the pellets of a shotgun), the oldest is cut off by a new one
    constexpr int MAX_AT_ONCE = 10;
    constexpr auto GAME_VOLUME_REFRESH = std::chrono::seconds(1);

    // Our folder in the file system of the game: the sounds are copied to <it>/sounds/cs2ext/
    const std::filesystem::path game_dir = "cache/game";
    constexpr auto GAME_SOUND_DIR = "sounds/cs2ext";

    // Page in the game: the code, then a CCommand of "playvol" with its arguments, the folder to add
    constexpr size_t GAME_PAGE_SIZE = 0x1000;
    constexpr size_t GAME_PLAY_CODE = 0x000;
    constexpr size_t GAME_MOUNT_CODE = 0x080;
    constexpr size_t GAME_COMMAND = 0x100;     // 0x448 bytes, argc at +0x438 & argv at +0x440
    constexpr size_t GAME_ARGV = 0x560;
    constexpr size_t GAME_ARG_COMMAND = 0x580;
    constexpr size_t GAME_ARG_NAME = 0x5A0;    // 256
    constexpr size_t GAME_ARG_VOLUME = 0x6A0;  // 32
    constexpr size_t GAME_PLAY_COUNT = 0x0F8;  // Times playvol is called by one call
    constexpr DWORD GAME_CALL_TIMEOUT = 100;   // ms, later it is played by Windows instead
    constexpr int GAME_MAX_FAILURES = 3;
    constexpr size_t GAME_PATH_ID = 0x6C0;
    constexpr size_t GAME_FOLDER = 0x700;      // To the end

    // A resource name the game takes: lowercase letters, digits & underscores
    std::string SafeName(const std::string& stem) {
        std::string out;
        for (unsigned char c : stem) {
            if (std::isalnum(c))
                out += static_cast<char>(std::tolower(c));
            else if (!out.empty() && out.back() != '_')
                out += '_';
        }
        while (!out.empty() && out.back() == '_')
            out.pop_back();
        return out.empty() ? std::string("sound") : out;
    }
    constexpr size_t MAX_QUEUE = 24;

    const std::filesystem::path sound_dir = "sound";

    std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    bool IsSound(const std::filesystem::path& path) {
        auto ext = Lower(path.extension().string());
        return ext == ".vsnd_c" || ext == ".wav" || ext == ".mp3";
    }

    std::string Utf8(const std::filesystem::path& path) {
        auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::filesystem::path FromUtf8(const std::string& text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string Narrow(const std::wstring& text) {
        return Utf8(std::filesystem::path(text));
    }

    template<class T>
    T At(const std::string& bytes, size_t offset) {
        T value{};
        if (offset + sizeof(T) <= bytes.size())
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    // An MPEG layer III frame header at offset, with the length of its frame
    bool Mp3FrameAt(const std::string& bytes, size_t offset, size_t& length) {
        if (offset + 4 > bytes.size())
            return false;

        auto h1 = static_cast<uint8_t>(bytes[offset]);
        auto h2 = static_cast<uint8_t>(bytes[offset + 1]);
        auto h3 = static_cast<uint8_t>(bytes[offset + 2]);

        if (h1 != 0xFF || (h2 & 0xE0) != 0xE0)
            return false;

        int version = (h2 >> 3) & 3;    // 3 MPEG 1, 2 MPEG 2, 0 MPEG 2.5
        int layer = (h2 >> 1) & 3;      // 1 layer III
        int bitrate_index = h3 >> 4;
        int rate_index = (h3 >> 2) & 3;
        int padding = (h3 >> 1) & 1;

        if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3)
            return false;

        static const int v1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
        static const int v2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
        static const int rates[] = { 44100, 48000, 32000 };

        int bitrate = (version == 3 ? v1 : v2)[bitrate_index] * 1000;
        int rate = rates[rate_index] / (version == 3 ? 1 : version == 2 ? 2 : 4);

        length = static_cast<size_t>((version == 3 ? 144 : 72) * bitrate / rate + padding);
        return length > 4;
    }

    void AppendWavHeader(std::string& out, uint32_t data_size, uint16_t channels, uint32_t rate, uint16_t bits) {
        auto put = [&](const void* data, size_t size) { out.append(static_cast<const char*>(data), size); };
        auto u32 = [&](uint32_t v) { put(&v, 4); };
        auto u16 = [&](uint16_t v) { put(&v, 2); };

        put("RIFF", 4); u32(36 + data_size); put("WAVE", 4);
        put("fmt ", 4); u32(16); u16(1); u16(channels); u32(rate); u32(rate * channels * bits / 8); u16(channels * bits / 8); u16(bits);
        put("data", 4); u32(data_size);
    }

    // The sound inside a .vsnd_c: a Source 2 resource, its blocks first & the sound right after them. Its DATA block
    // (version 4, 48 bytes) tells what the sound is: { format & channels, loop start, samples, duration, ... }
    bool Unpack(const std::string& bytes, std::string& audio, const wchar_t*& ext) {
        if (bytes.size() < 16 || At<uint16_t>(bytes, 4) != 12)
            return false;

        size_t table = 8 + At<uint32_t>(bytes, 8);
        uint32_t count = At<uint32_t>(bytes, 12);
        if (count > 64)
            return false;

        size_t end = 16;
        size_t data = 0, data_size = 0;

        for (uint32_t i = 0; i < count; i++) {
            size_t entry = table + i * 12;
            if (entry + 12 > bytes.size())
                return false;

            size_t start = entry + 4 + At<uint32_t>(bytes, entry + 4);
            size_t size = At<uint32_t>(bytes, entry + 8);
            end = std::max({ end, entry + 12, start + size });

            if (bytes.compare(entry, 4, "DATA") == 0) {
                data = start;
                data_size = size;
            }
        }

        if (end >= bytes.size())
            return false;

        auto sound = bytes.substr(end);

        if (data && data_size >= 32) {
            auto packed = At<uint32_t>(bytes, data);
            int format = (packed >> 16) & 0xFF;        // 0 16 bit samples, 1 8 bit, 2 MP3
            int channels = (packed >> 24) & 0xFF;
            auto samples = At<uint32_t>(bytes, data + 8);
            auto duration = At<float>(bytes, data + 12);

            if (format == 2) {
                audio = sound;
                ext = L".mp3";
                return true;
            }

            if ((format == 0 || format == 1) && channels >= 1 && channels <= 8 && samples && duration > 0.f) {
                // The rate is samples over duration, the nearest usual one
                static const uint32_t rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
                float measured = samples / duration;
                uint32_t rate = 44100;
                for (auto r : rates)
                    if (std::abs(static_cast<float>(r) - measured) < std::abs(static_cast<float>(rate) - measured))
                        rate = r;

                uint16_t bits = format == 1 ? 8 : 16;
                size_t block = static_cast<size_t>(channels) * bits / 8;
                sound.resize(sound.size() - sound.size() % block);

                audio.clear();
                AppendWavHeader(audio, static_cast<uint32_t>(sound.size()), static_cast<uint16_t>(channels), rate, bits);
                audio += sound;
                ext = L".wav";
                return true;
            }
        }

        // Unknown header: a whole file (wav, mp3) right after the blocks, maybe after a little padding
        for (size_t i = end; i < std::min(bytes.size(), end + 256); i++) {
            size_t length = 0;

            if (bytes.compare(i, 4, "RIFF") == 0) {
                audio = bytes.substr(i);
                ext = L".wav";
                return true;
            }

            if (bytes.compare(i, 3, "ID3") == 0 || (Mp3FrameAt(bytes, i, length) && Mp3FrameAt(bytes, i + length, length))) {
                audio = bytes.substr(i);
                ext = L".mp3";
                return true;
            }
        }

        return false;
    }

    template<class T>
    void Release(T*& object) {
        if (object) {
            object->Release();
            object = nullptr;
        }
    }

    // A wav or mp3 file to plain samples through Media Foundation, with their format
    bool Decode(const std::wstring& file, std::vector<uint8_t>& pcm, std::vector<uint8_t>& format) {
        IMFSourceReader* reader = nullptr;
        IMFMediaType* wanted = nullptr;
        IMFMediaType* actual = nullptr;
        bool success = false;

        do {
            if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader)))
                break;

            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

            if (FAILED(MFCreateMediaType(&wanted)))
                break;
            wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

            if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, wanted)))
                break;
            if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual)))
                break;

            WAVEFORMATEX* wave = nullptr;
            UINT32 wave_size = 0;
            if (FAILED(MFCreateWaveFormatExFromMFMediaType(actual, &wave, &wave_size)) || !wave)
                break;
            format.assign(reinterpret_cast<uint8_t*>(wave), reinterpret_cast<uint8_t*>(wave) + wave_size);
            CoTaskMemFree(wave);

            pcm.clear();
            while (true) {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)))
                    break;

                if (sample) {
                    IMFMediaBuffer* buffer = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                        BYTE* data = nullptr;
                        DWORD length = 0;
                        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                            pcm.insert(pcm.end(), data, data + length);
                            buffer->Unlock();
                        }
                        buffer->Release();
                    }
                    sample->Release();
                }

                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                    break;
            }

            success = !pcm.empty();
        } while (false);

        Release(actual);
        Release(wanted);
        Release(reader);
        return success;
    }

    // The volume of the game in the volume mixer of Windows, 0 when muted, 1 when it has no session yet
    float GameVolume(DWORD pid) {
        float level = 1.f;

        IMMDeviceEnumerator* devices = nullptr;
        IMMDevice* device = nullptr;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* sessions = nullptr;

        do {
            if (!pid || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))))
                break;
            if (FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
                break;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))))
                break;
            if (FAILED(manager->GetSessionEnumerator(&sessions)))
                break;

            int count = 0;
            sessions->GetCount(&count);
            for (int i = 0; i < count; i++) {
                IAudioSessionControl* control = nullptr;
                IAudioSessionControl2* control2 = nullptr;
                ISimpleAudioVolume* volume = nullptr;

                DWORD owner = 0;
                if (SUCCEEDED(sessions->GetSession(i, &control))
                    && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2)))
                    && SUCCEEDED(control2->GetProcessId(&owner)) && owner == pid
                    && SUCCEEDED(control2->QueryInterface(IID_PPV_ARGS(&volume)))) {
                    float value = 1.f;
                    BOOL muted = FALSE;
                    volume->GetMasterVolume(&value);
                    volume->GetMute(&muted);
                    level = muted ? 0.f : value;
                }

                Release(volume);
                Release(control2);
                Release(control);

                if (owner == pid)
                    break;
            }
        } while (false);

        Release(sessions);
        Release(manager);
        Release(device);
        Release(devices);
        return level;
    }

    std::string HashName(const std::string& key) {
        uint64_t hash = 14695981039346656037ull; // FNV-1a
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        return std::format("{:016x}", hash);
    }

    std::string UrlEncode(const std::string& text) {
        std::string out;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                out += static_cast<char>(c);
            else
                out += std::format("%{:02X}", c);
        }
        return out;
    }

    bool ReadFile(const std::filesystem::path& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.good())
            return false;

        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !out.empty();
    }
}

bool Sounds::Init() {
    auto& sounds = GetInstance();
    if (sounds.running.exchange(true))
        return true;

    std::error_code ec;
    std::filesystem::create_directories(sound_dir, ec);

    // The two folders of before, hitsound & killsound, moved into the one. Empty ones are removed
    for (const char* old : { "hitsound", "killsound" }) {
        if (!std::filesystem::is_directory(old, ec))
            continue;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(old, ec))
            if (entry.is_regular_file(ec))
                files.push_back(entry.path());

        for (const auto& file : files) {
            auto target = sound_dir / file.filename();

            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::rename(file, target, ec);
                continue;
            }

            // The download of before put every sound in both folders: a copy the same as the moved one goes
            std::string a, b;
            if (ReadFile(file, a) && ReadFile(target, b) && a == b)
                std::filesystem::remove(file, ec);
        }

        // Only removed when empty, a different file of the same name keeps it
        std::filesystem::remove(old, ec);
    }

    // The sounds of the config ready before the first hit
    if (cfg::misc::hitsound)
        Preload(cfg::misc::hitsound_file);
    if (cfg::misc::killsound)
        Preload(cfg::misc::killsound_file);

    std::thread(&Sounds::Thread, &sounds).detach();
    return true;
}

void Sounds::Shutdown() {
    auto& sounds = GetInstance();
    sounds.stopping = true;
    sounds.wake.notify_all();
}

std::filesystem::path Sounds::Folder() {
    return sound_dir;
}

std::vector<std::string> Sounds::List() {
    auto& sounds = GetInstance();
    std::lock_guard lock(sounds.list_mutex);

    auto& listing = sounds.listing;
    auto now = std::chrono::steady_clock::now();

    if (now - listing.at > std::chrono::seconds(1)) {
        listing.at = now;
        listing.files.clear();

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(sound_dir, ec)) {
            if (entry.is_regular_file(ec) && IsSound(entry.path()))
                listing.files.push_back(Utf8(entry.path().filename()));
        }

        std::sort(listing.files.begin(), listing.files.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    }

    return listing.files;
}

void Sounds::Play(const std::string& file, int volume, int count) {
    if (file.empty())
        return;

    auto& sounds = GetInstance();
    {
        std::lock_guard lock(sounds.mutex);
        if (sounds.queue.size() >= MAX_QUEUE)
            sounds.queue.pop_front();

        sounds.queue.push_back({ sound_dir / FromUtf8(file), std::clamp(volume, 0, 100),
            std::chrono::steady_clock::now(), std::clamp(count, 1, MAX_AT_ONCE) });
    }
    sounds.wake.notify_one();
}

// The sounds play through XAudio2 on this thread, from samples decoded once: playing one is only handing its buffer
// to a voice, a few ms. Their volume follows the one of the game in the volume mixer of Windows
void Sounds::Thread() {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool media = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    if (FAILED(XAudio2Create(&this->engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(this->engine->CreateMasteringVoice(&this->master))) {
        LOGF(WARNING, "Could not start XAudio2, the hit & kill sounds stay silent");
        Release(this->engine);
        this->master = nullptr;
    }

    auto next_volume = std::chrono::steady_clock::now();

    while (!this->stopping) {
        // The requests that are due, the delayed ones stay queued until then
        std::deque<Request> work;
        {
            std::unique_lock lock(this->mutex);

            auto now = std::chrono::steady_clock::now();
            auto wait = std::chrono::steady_clock::duration(std::chrono::milliseconds(100));
            for (const auto& request : this->queue)
                wait = std::min(wait, std::max(std::chrono::steady_clock::duration::zero(), request.at - now));

            // A new request wakes it too, it may be due sooner than the one waited for
            size_t seen = this->queue.size();
            this->wake.wait_for(lock, wait, [&] {
                auto now = std::chrono::steady_clock::now();
                return this->stopping.load() || this->queue.size() != seen
                    || std::any_of(this->queue.begin(), this->queue.end(), [&](const Request& r) { return r.at <= now; });
            });

            now = std::chrono::steady_clock::now();
            for (auto it = this->queue.begin(); it != this->queue.end();) {
                if (it->at <= now) {
                    work.push_back(std::move(*it));
                    it = this->queue.erase(it);
                }
                else
                    ++it;
            }
        }

        if (auto now = std::chrono::steady_clock::now(); now >= next_volume) {
            next_volume = now + GAME_VOLUME_REFRESH;
            auto p = Engine::GetProcess();
            this->game_volume = GameVolume(p ? static_cast<DWORD>(p->pid_) : 0);
        }

        for (const auto& request : work)
            PlayNow(request.path, request.volume, request.count);
    }

    for (auto& [file, sound] : this->loaded)
        for (auto voice : sound.voices)
            voice->DestroyVoice();
    this->loaded.clear();

    if (this->master)
        this->master->DestroyVoice();
    Release(this->engine);

    if (media)
        MFShutdown();
    if (SUCCEEDED(com))
        CoUninitialize();
}

// A volume below 0 only loads it, for the sounds picked in the menu before they are heard
void Sounds::PlayNow(const std::filesystem::path& source, int volume, int count) {
    // The game plays it when it can, a .vsnd_c with -insecure. Loading ahead is the game's own business then
    if (Lower(source.extension().string()) == ".vsnd_c" && EnsureGameSound()) {
        if (volume < 0) {
            GameName(source);
            return;
        }
        if (PlayInGame(source, volume, count))
            return;
    }

    if (!this->engine)
        return;

    auto file = Playable(source);
    if (file.empty())
        return;

    auto it = this->loaded.find(file);
    if (it == this->loaded.end()) {
        Loaded sound;
        if (!Decode(file, sound.pcm, sound.format)) {
            LOGF(WARNING, "Could not decode the sound {}", Utf8(source));
            this->loaded[file] = {};  // Not tried again
            return;
        }

        auto wave = reinterpret_cast<const WAVEFORMATEX*>(sound.format.data());
        for (int i = 0; i < VOICES; i++) {
            IXAudio2SourceVoice* voice = nullptr;
            if (SUCCEEDED(this->engine->CreateSourceVoice(&voice, wave)))
                sound.voices.push_back(voice);
        }

        it = this->loaded.emplace(file, std::move(sound)).first;
    }

    auto& sound = it->second;
    if (volume < 0 || sound.voices.empty())
        return;

    // Loudness rises gently like a volume slider of the game
    float level = volume / 100.f;

    // All of them in one operation set, so they start on the same sample & stack up
    constexpr UINT32 OPERATION_SET = 1;
    for (int i = 0; i < count; i++) {
        auto voice = sound.voices[sound.next];
        sound.next = (sound.next + 1) % sound.voices.size();

        // Cut off when it still plays, then from the start
        voice->Stop();
        voice->FlushSourceBuffers();

        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sound.pcm.size());
        buffer.pAudioData = sound.pcm.data();
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        voice->SetVolume(level * level * this->game_volume, OPERATION_SET);
        voice->SubmitSourceBuffer(&buffer);
        voice->Start(0, OPERATION_SET);
    }
    this->engine->CommitChanges(OPERATION_SET);
}

bool Sounds::EnsureGameSound() {
    if (this->game_tried)
        return this->game_ready;

    if (!Engine::IsInsecure() || !offsets::sounds::fnPlayVol)
        return false;

    this->game_tried = true;

    auto p = Engine::GetProcess();
    auto sound = p ? p->GetModule("soundsystem.dll") : ProcessModule{};
    auto file_system = p ? p->FindInterface("filesystem_stdio.dll", "VFileSystem017") : 0;

    if (!sound.base || !file_system || !GameThread::Ensure()) {
        LOGF(WARNING, "Hit & kill sounds: the sound system of the game cannot be used, Windows plays them");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(game_dir / GAME_SOUND_DIR, ec);
    auto folder = std::filesystem::absolute(game_dir, ec);
    auto folder_text = Utf8(folder);
    if (ec || folder_text.size() + 1 > GAME_PAGE_SIZE - GAME_FOLDER) {
        LOGF(WARNING, "Hit & kill sounds: no folder for the game, Windows plays them");
        return false;
    }

    auto page = p->allocate_remote(GAME_PAGE_SIZE, PAGE_EXECUTE_READWRITE);
    if (!page)
        return false;

    std::vector<uint8_t> data(GAME_PAGE_SIZE, 0);
    auto put = [&](size_t at, const void* bytes, size_t size) { std::memcpy(data.data() + at, bytes, size); };
    auto put64 = [&](size_t at, uint64_t value) { put(at, &value, 8); };

    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto emit64 = [&](uint64_t value) { for (int i = 0; i < 8; i++) code.push_back(static_cast<uint8_t>(value >> (i * 8))); };

    // playvol(0, &command), count times in the same frame so they stack up
    emit({ 0x53 });                                             // push rbx
    emit({ 0x48, 0x83, 0xEC, 0x20 });                           // sub rsp, 0x20
    emit({ 0x48, 0xB8 }); emit64(page + GAME_PLAY_COUNT);       // mov rax, count
    emit({ 0x8B, 0x18 });                                       // mov ebx, [rax]
    // loop:
    emit({ 0x85, 0xDB });                                       // test ebx, ebx
    emit({ 0x7E, 0x1C });                                       // jle done
    emit({ 0x31, 0xC9 });                                       // xor ecx, ecx
    emit({ 0x48, 0xBA }); emit64(page + GAME_COMMAND);          // mov rdx, command
    emit({ 0x48, 0xB8 }); emit64(sound.base + offsets::sounds::fnPlayVol); // mov rax, playvol
    emit({ 0xFF, 0xD0 });                                       // call rax
    emit({ 0xFF, 0xCB });                                       // dec ebx
    emit({ 0xEB, 0xE0 });                                       // jmp loop
    // done:
    emit({ 0x48, 0x83, 0xC4, 0x20 });                           // add rsp, 0x20
    emit({ 0x5B });                                             // pop rbx
    emit({ 0xC3 });                                             // ret
    put(GAME_PLAY_CODE, code.data(), code.size());

    // file_system->AddSearchPath(folder, "GAME", PATH_ADD_TO_TAIL, 0, 0)
    code.clear();
    uint32_t slot = static_cast<uint32_t>(offsets::sounds::fileSystemAddSearchPath * 8);
    emit({ 0x48, 0x83, 0xEC, 0x38 });                           // sub rsp, 0x38
    emit({ 0x48, 0xB9 }); emit64(file_system);                  // mov rcx, file system
    emit({ 0x48, 0xBA }); emit64(page + GAME_FOLDER);           // mov rdx, folder
    emit({ 0x49, 0xB8 }); emit64(page + GAME_PATH_ID);          // mov r8, "GAME"
    emit({ 0x41, 0xB9, 0x01, 0x00, 0x00, 0x00 });               // mov r9d, 1
    emit({ 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00 }); // mov qword [rsp + 0x20], 0
    emit({ 0x48, 0xC7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00 }); // mov qword [rsp + 0x28], 0
    emit({ 0x48, 0x8B, 0x01 });                                 // mov rax, [rcx]
    emit({ 0xFF, 0x90 });                                       // call [rax + slot]
    for (int i = 0; i < 4; i++) code.push_back(static_cast<uint8_t>(slot >> (i * 8)));
    emit({ 0x48, 0x83, 0xC4, 0x38 });                           // add rsp, 0x38
    emit({ 0xC3 });                                             // ret
    put(GAME_MOUNT_CODE, code.data(), code.size());

    // The command: 3 arguments, "playvol" <name> <volume>
    int32_t argc = 3;
    put(GAME_COMMAND + offsets::sounds::ccommandArgc, &argc, 4);
    put64(GAME_COMMAND + offsets::sounds::ccommandArgv, page + GAME_ARGV);
    put64(GAME_ARGV, page + GAME_ARG_COMMAND);
    put64(GAME_ARGV + 8, page + GAME_ARG_NAME);
    put64(GAME_ARGV + 16, page + GAME_ARG_VOLUME);
    put(GAME_ARG_COMMAND, "playvol", 8);
    put(GAME_PATH_ID, "GAME", 5);
    put(GAME_FOLDER, folder_text.c_str(), folder_text.size() + 1);

    p->write_bytes(page, data);
    FlushInstructionCache(p->handle_, reinterpret_cast<void*>(page), GAME_PAGE_SIZE);

    if (!GameThread::Call(page + GAME_MOUNT_CODE)) {
        // Might still run later, the page stays
        LOGF(WARNING, "Hit & kill sounds: adding our folder to the game did not finish in time, Windows plays them");
        return false;
    }

    this->game_page = page;
    this->game_ready = true;
    // The game sees the folder as one of its own: "sounds/cs2ext/x" in it is <folder>\sounds\cs2ext\x.vsnd_c
    LOGF(INFO, "Hit & kill sounds are played by the game, copied to {}", Utf8((folder / GAME_SOUND_DIR).make_preferred()));
    return true;
}

std::string Sounds::GameName(const std::filesystem::path& source) {
    std::error_code ec;
    auto size = std::filesystem::file_size(source, ec);
    if (ec)
        return {};

    auto time = std::filesystem::last_write_time(source, ec).time_since_epoch().count();
    auto key = std::format("{}|{}|{}", Utf8(source), size, time);
    if (auto it = this->game_names.find(key); it != this->game_names.end())
        return it->second;

    // The same name for the same file, a new one when another file would take it
    auto stem = SafeName(Utf8(source.stem()));
    auto name = std::string(GAME_SOUND_DIR) + "/" + stem;
    for (int i = 2; std::any_of(this->game_names.begin(), this->game_names.end(), [&](const auto& entry) { return entry.second == name; }); i++)
        name = std::format("{}/{}_{}", GAME_SOUND_DIR, stem, i);

    auto target = game_dir / (name + ".vsnd_c");
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        LOGF(WARNING, "Could not copy the sound {} for the game", Utf8(source));
        return this->game_names[key] = {};
    }

    return this->game_names[key] = name;
}

bool Sounds::PlayInGame(const std::filesystem::path& source, int volume, int count) {
    auto name = GameName(source);
    if (name.empty() || name.size() + 1 > GAME_ARG_VOLUME - GAME_ARG_NAME)
        return false;

    auto p = Engine::GetProcess();
    if (!p)
        return false;

    // Our calls run from a function of the game's input, which only runs in a match. In the menu (the preview of
    // the sounds) Windows plays them
    if (!Cache::Current()->globals.in_match)
        return false;

    // The game mutes itself while it is not the window in front (our menu open, alt tab): what it plays then is
    // never heard, the preview of a picked sound or volume included. Windows plays those
    if (GetForegroundWindow() != p->hwnd_)
        return false;

    // Loudness rises gently like the volume slider of the game, which still applies on top
    float level = volume / 100.f;
    auto volume_text = std::format("{:.3f}", level * level);

    std::vector<uint8_t> args(GAME_ARG_VOLUME + 32 - GAME_ARG_NAME, 0);
    std::memcpy(args.data(), name.c_str(), name.size() + 1);
    std::memcpy(args.data() + (GAME_ARG_VOLUME - GAME_ARG_NAME), volume_text.c_str(), volume_text.size() + 1);
    p->write_bytes(this->game_page + GAME_ARG_NAME, args);
    p->write<int32_t>(this->game_page + GAME_PLAY_COUNT, std::clamp(count, 1, MAX_AT_ONCE));

    auto start = std::chrono::steady_clock::now();
    if (!GameThread::Call(this->game_page + GAME_PLAY_CODE, GAME_CALL_TIMEOUT)) {
        if (++this->game_failures >= GAME_MAX_FAILURES) {
            this->game_ready = false;
            LOGF(WARNING, "Hit & kill sounds: the game did not take {} calls in a row, Windows plays them from now on", GAME_MAX_FAILURES);
        }
        return false;
    }

    this->game_failures = 0;
    if (!this->game_played) {
        this->game_played = true;
        LOGF(INFO, "Hit & kill sounds: the game played {} ({} ms)", name,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }
    return true;
}

void Sounds::Preload(const std::string& file) {
    if (file.empty())
        return;

    auto& sounds = GetInstance();
    {
        std::lock_guard lock(sounds.mutex);
        sounds.queue.push_back({ sound_dir / FromUtf8(file), -1, {} });
    }
    sounds.wake.notify_one();
}

std::wstring Sounds::Playable(const std::filesystem::path& source) {
    std::error_code ec;
    auto ext = Lower(source.extension().string());

    if (ext == ".wav" || ext == ".mp3") {
        auto absolute = std::filesystem::absolute(source, ec);
        return ec ? std::wstring() : absolute.wstring();
    }

    if (ext != ".vsnd_c")
        return {};

    // Unpacked once per version of the file
    auto size = std::filesystem::file_size(source, ec);
    if (ec)
        return {};

    auto time = std::filesystem::last_write_time(source, ec).time_since_epoch().count();
    auto key = std::format("{}|{}|{}", Utf8(source), size, time);

    if (auto it = this->playable.find(key); it != this->playable.end())
        return it->second;

    auto& result = this->playable[key];

    std::string bytes, audio;
    const wchar_t* out_ext = L".wav";

    if (!ReadFile(source, bytes) || !Unpack(bytes, audio, out_ext)) {
        LOGF(WARNING, "No sound found in {}, is it a .vsnd_c of Source 2?", Utf8(source));
        return result;
    }

    std::filesystem::create_directories(cache_dir, ec);
    auto target = std::filesystem::absolute(cache_dir / (HashName(key) + Narrow(out_ext)), ec);
    if (ec)
        return result;

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(audio.data(), static_cast<std::streamsize>(audio.size()));
    if (!out.good()) {
        LOGF(WARNING, "Could not write the unpacked sound of {}", Utf8(source));
        return result;
    }

    result = target.wstring();
    return result;
}

void Sounds::Download() {
    auto& sounds = GetInstance();
    if (sounds.downloading.exchange(true))
        return;

    std::thread(&Sounds::DownloadThread, &sounds).detach();
}

bool Sounds::IsDownloading() {
    return GetInstance().downloading;
}

std::string Sounds::GetStatus() {
    auto& sounds = GetInstance();
    std::lock_guard lock(sounds.status_mutex);
    return sounds.status;
}

void Sounds::DownloadThread() {
    auto set_status = [&](const std::string& text) {
        std::lock_guard lock(this->status_mutex);
        this->status = text;
    };

    set_status("Downloading...");

    // Every file of the repository, from the GitHub API
    json tree;
    int http_status = HttpHelper::Get(REPOSITORY_TREE, tree);
    if (http_status != 200 || !tree.is_object() || !tree.contains("tree") || !tree["tree"].is_array()) {
        LOGF(WARNING, "Could not get the sound list (status {})", http_status);
        set_status(std::format("Could not get the sound list (status {})", http_status));
        this->downloading = false;
        return;
    }

    int added = 0, present = 0, failed = 0;

    std::error_code ec;
    std::filesystem::create_directories(sound_dir, ec);

    for (const auto& item : tree["tree"]) {
        if (!item.is_object() || item.value("type", "") != "blob")
            continue;

        auto repo_path = item.value("path", "");
        auto path = FromUtf8(repo_path);
        if (repo_path.empty() || repo_path.find("..") != std::string::npos || !repo_path.starts_with(REPOSITORY_SOUNDS) || !IsSound(path))
            continue;

        std::string url = REPOSITORY_FILES;
        bool first = true;
        for (const auto& part : path) {
            if (!first)
                url += '/';
            first = false;
            url += UrlEncode(Utf8(part));
        }

        auto target = sound_dir / path.filename();
        if (std::filesystem::exists(target, ec)) {
            present++;
            continue;
        }

        std::string body;
        if (HttpHelper::GetRaw(url, body) != 200 || body.empty()) {
            LOGF(WARNING, "Could not download the sound {}", repo_path);
            failed++;
            continue;
        }

        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.good() ? added++ : failed++;
    }

    // Seen in the lists right away
    {
        std::lock_guard lock(this->list_mutex);
        this->listing.at = {};
    }

    auto text = std::format("{} new, {} already there", added, present);
    if (failed)
        text += std::format(", {} failed", failed);

    LOGF(INFO, "Sounds downloaded: {}", text);
    set_status(text);
    this->downloading = false;
}
