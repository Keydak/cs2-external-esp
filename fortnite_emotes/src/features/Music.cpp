#include "features/Music.h"
#include "core/Journal.h"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace emotes {
namespace {

struct Clip {
    WAVEFORMATEX fmt{};
    std::vector<uint8_t> data;
};

constexpr int kMaxInFlight = 16;
constexpr uintmax_t kMaxMusicBytes = 64u * 1024u * 1024u;  // a dance's song: ~6 min of 44.1 kHz stereo

std::atomic<int> g_inFlight{0};  // queued or playing workers (they run our code)
std::atomic<bool> g_shutdown{false};

// A queued play: loaded on its own thread from path (no file I/O on the game thread), its volume read live from
// `live`, and stopped early once g_musicGen moves on.
struct Job {
    std::shared_ptr<const Clip> clip;
    std::filesystem::path path;
    unsigned gen;
    const std::atomic<float>* live;
};
std::atomic<unsigned> g_musicGen{0};

void Scale(std::vector<uint8_t>& buf, WORD bits, float vol) {
    if (vol >= 1.0f) return;
    if (bits == 16) {
        auto* s = reinterpret_cast<int16_t*>(buf.data());
        for (size_t i = 0, n = buf.size() / 2; i < n; ++i) s[i] = static_cast<int16_t>(std::lrintf(s[i] * vol));
    } else {
        for (uint8_t& b : buf) b = static_cast<uint8_t>(std::lrintf((b - 128) * vol) + 128);
    }
}

// The songs are mastered anywhere from quiet to very loud (the newer ones ~8 dB over the older): each is turned
// down to one loudness (RMS ~3000 of 32767, where the quieter ones sit), never up.
float MusicNorm(const Clip& clip) {
    double sum = 0.0;
    size_t n = 0;
    if (clip.fmt.wBitsPerSample == 16) {
        const auto* s = reinterpret_cast<const int16_t*>(clip.data.data());
        for (size_t i = 0, count = clip.data.size() / 2; i < count; i += 7, ++n) sum += static_cast<double>(s[i]) * s[i];
    } else {
        for (size_t i = 0; i < clip.data.size(); i += 7, ++n) sum += ((clip.data[i] - 128) * 256.0) * ((clip.data[i] - 128) * 256.0);
    }
    const double rms = n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
    return rms > 3000.0 ? static_cast<float>(3000.0 / rms) : 1.0f;
}

// The volume's 0-1 on a squared curve: half-way sounds about half as loud (a straight gain is loud until the end).
float MusicGain(const std::atomic<float>& live) {
    const float v = std::clamp(live.load(std::memory_order_relaxed), 0.0f, 1.0f);
    return std::isnan(v) ? 0.49f : v * v;
}

// Music in 50 ms blocks, each scaled as it is queued: a volume change acts within ~0.2 s, not at the next loop.
// Never waveOutSetVolume: on Vista+ it sets the whole process session, i.e. CS2's own volume.
void PlayMusic(HWAVEOUT wo, const Clip& clip, const Job& job) {
    constexpr int kBlocks = 4;
    const size_t align = std::max<size_t>(clip.fmt.nBlockAlign, 1);
    const size_t block = std::max(align, clip.fmt.nAvgBytesPerSec / 20 / align * align);
    const float norm = MusicNorm(clip);
    std::vector<uint8_t> bufs[kBlocks];
    WAVEHDR hdrs[kBlocks]{};
    bool queued[kBlocks]{};
    size_t pos = 0;
    bool stop = false;
    ULONGLONG stoppedAt = 0, progressAt = GetTickCount64();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        // unload, the music stopped, or a stuck device (no block finished for 2 s): stop now
        stop = stop || g_shutdown.load() || job.gen != g_musicGen.load() || now - progressAt > 2000;
        bool busy = false;
        for (int k = 0; k < kBlocks; ++k) {
            if (queued[k] && !(static_cast<volatile DWORD&>(hdrs[k].dwFlags) & WHDR_DONE)) {
                busy = true;
                continue;
            }
            if (queued[k]) {
                waveOutUnprepareHeader(wo, &hdrs[k], sizeof(WAVEHDR));
                queued[k] = false;
                progressAt = now;
            }
            if (stop || pos >= clip.data.size()) continue;
            const size_t n = std::min(block, clip.data.size() - pos);
            bufs[k].assign(clip.data.begin() + static_cast<ptrdiff_t>(pos), clip.data.begin() + static_cast<ptrdiff_t>(pos + n));
            Scale(bufs[k], clip.fmt.wBitsPerSample, MusicGain(*job.live) * norm);
            hdrs[k] = {};
            hdrs[k].lpData = reinterpret_cast<LPSTR>(bufs[k].data());
            hdrs[k].dwBufferLength = static_cast<DWORD>(n);
            if (waveOutPrepareHeader(wo, &hdrs[k], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                stop = true;
                continue;
            }
            if (waveOutWrite(wo, &hdrs[k], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                waveOutUnprepareHeader(wo, &hdrs[k], sizeof(WAVEHDR));
                stop = true;
                continue;
            }
            queued[k] = busy = true;
            pos += n;
        }
        if (!busy) break;
        if (stop && !stoppedAt) {
            waveOutReset(wo);  // marks every queued block done
            stoppedAt = now;
        }
        if (stoppedAt && now - stoppedAt > 500) break;  // the device never gave them back
        Sleep(5);
    }
    for (int k = 0; k < kBlocks; ++k)
        if (queued[k]) waveOutUnprepareHeader(wo, &hdrs[k], sizeof(WAVEHDR));
}

// ---- WAV loader ----

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

// RIFF PCM, 8/16-bit, mono/stereo, any rate, <= maxBytes.
std::shared_ptr<const Clip> LoadWav(const std::filesystem::path& path, uintmax_t maxBytes) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size < 44 || size > maxBytes) return nullptr;
    std::vector<uint8_t> d(static_cast<size_t>(size));
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()))) return nullptr;
    if (std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return nullptr;

    auto clip = std::make_shared<Clip>();
    bool haveFmt = false;
    for (size_t pos = 12; pos + 8 <= d.size();) {
        const uint8_t* id = d.data() + pos;
        const size_t len = U32(id + 4), body = pos + 8, avail = std::min(len, d.size() - body);
        if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
            const uint8_t* b = d.data() + body;
            const uint16_t tag = U16(b), ch = U16(b + 2), bits = U16(b + 14);
            const uint32_t rate = U32(b + 4);
            if (tag != WAVE_FORMAT_PCM || ch < 1 || ch > 2 || (bits != 8 && bits != 16) || rate < 1 || rate > 384000)
                return nullptr;
            clip->fmt.wFormatTag = WAVE_FORMAT_PCM;
            clip->fmt.nChannels = ch;
            clip->fmt.nSamplesPerSec = rate;
            clip->fmt.wBitsPerSample = bits;
            clip->fmt.nBlockAlign = static_cast<WORD>(ch * bits / 8);
            clip->fmt.nAvgBytesPerSec = rate * clip->fmt.nBlockAlign;
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFmt) return nullptr;
            const size_t n = avail - avail % clip->fmt.nBlockAlign;
            if (!n) return nullptr;
            clip->data.assign(d.begin() + body, d.begin() + body + n);
            return clip;
        }
        pos = body + len + (len & 1);
    }
    return nullptr;
}

DWORD WINAPI PlayWorker(void* arg) {
    auto* job = static_cast<Job*>(arg);
    const std::string name = reinterpret_cast<const char*>(job->path.filename().u8string().c_str());
    journal().info("music", "player thread running for " + name);
    if (job->gen == g_musicGen.load()) {
        job->clip = LoadWav(job->path, kMaxMusicBytes);
        if (!job->clip) journal().warn("music", "can't read " + name + " (not a 8/16-bit PCM .wav, or over 64 MB)");
    }
    if (job->clip) {
        WAVEFORMATEX fmt = job->clip->fmt;
        HWAVEOUT wo = nullptr;
        if (const MMRESULT r = waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL); r == MMSYSERR_NOERROR) {
            journal().info("music", "playing " + name);
            PlayMusic(wo, *job->clip, *job);
            waveOutClose(wo);
        } else {
            journal().warn("music", "no audio output for " + name + " (waveOutOpen error " + std::to_string(r) + ")");
        }
    }
    delete job;
    g_inFlight.fetch_sub(1);
    return 0;
}

// Takes ownership of job. false = nothing started.
// A thread of our own, not the thread pool: with a manually mapped DLL the pool never calls back into our code
// (the song was queued and never played), while CreateThread runs it.
bool Queue(Job* job) {
    if (!job) return false;
    HANDLE thread = nullptr;
    if (g_inFlight.fetch_add(1) >= kMaxInFlight || g_shutdown.load() ||
        !(thread = CreateThread(nullptr, 0, PlayWorker, job, 0, nullptr))) {
        journal().warn("music", "song not started (" + std::to_string(g_inFlight.load()) + " playing, shutdown " +
                                    std::to_string(g_shutdown.load()) + ")");
        g_inFlight.fetch_sub(1);
        delete job;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace

void MusicPlay(const std::filesystem::path& wav, const std::atomic<float>* volume) {
    const unsigned gen = ++g_musicGen;  // the current music stops
    if (volume) Queue(new (std::nothrow) Job{nullptr, wav, gen, volume});
}

void MusicStop() { ++g_musicGen; }

void MusicShutdown() {
    g_shutdown = true;  // no new plays; running workers stop their device (waveOutReset) and exit
    for (int i = 0; i < 400 && g_inFlight.load() > 0; ++i) Sleep(5);  // <= 2 s
}

}  // namespace emotes
