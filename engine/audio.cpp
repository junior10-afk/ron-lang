// Tiny software synthesizer: a mixer thread feeds waveOut with double-buffered
// 16-bit mono PCM, mixing up to 32 voices (sine / square / saw / noise) with a
// short attack and exponential release so sounds never click.

#include "ron_engine.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>

namespace {

constexpr int kSampleRate = 44100;
constexpr int kBufferSamples = 1024;   // ~23 ms per buffer
constexpr int kBufferCount = 3;
constexpr int kMaxVoices = 32;

struct Voice {
    bool active = false;
    int wave = 0;
    float phase = 0;
    float freq = 440, freqEnd = 440;
    float volume = 0.5f;
    float time = 0, duration = 0;
    uint32_t noise = 0x12345678;
};

HWAVEOUT g_device = nullptr;
WAVEHDR g_headers[kBufferCount];
int16_t g_buffers[kBufferCount][kBufferSamples];
HANDLE g_event = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::mutex g_mutex;
Voice g_voices[kMaxVoices];

float sampleVoice(Voice& v) {
    const float dt = 1.0f / kSampleRate;
    float t = v.duration > 0 ? v.time / v.duration : 1.0f;
    float freq = v.freq + (v.freqEnd - v.freq) * t;
    v.phase += freq * dt;
    v.phase -= std::floor(v.phase);
    float s;
    switch (v.wave) {
    case 1: s = v.phase < 0.5f ? 1.0f : -1.0f; break;
    case 2: s = 2.0f * v.phase - 1.0f; break;
    case 3:
        v.noise ^= v.noise << 13;
        v.noise ^= v.noise >> 17;
        v.noise ^= v.noise << 5;
        s = static_cast<float>(v.noise) / 2147483648.0f - 1.0f;
        break;
    default: s = std::sin(v.phase * 6.2831853f); break;
    }
    float attack = std::min(1.0f, v.time / 0.005f);
    float release = std::exp(-4.0f * t);
    v.time += dt;
    if (v.time >= v.duration) v.active = false;
    return s * v.volume * attack * release;
}

void fill(int16_t* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kBufferSamples; ++i) {
        float mix = 0;
        for (Voice& v : g_voices)
            if (v.active) mix += sampleVoice(v);
        mix = std::tanh(mix);   // soft clip
        out[i] = static_cast<int16_t>(mix * 30000.0f);
    }
}

void mixerThread() {
    while (g_running) {
        WaitForSingleObject(g_event, 50);
        for (WAVEHDR& h : g_headers) {
            if (!(h.dwFlags & WHDR_DONE)) continue;
            fill(reinterpret_cast<int16_t*>(h.lpData));
            h.dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_device, &h, sizeof(h));
        }
    }
}

void startVoice(float from, float to, float seconds, float volume, int wave) {
    if (!g_device || seconds <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Voice* slot = &g_voices[0];
    for (Voice& v : g_voices) {
        if (!v.active) {
            slot = &v;
            break;
        }
        if (v.time / v.duration > slot->time / slot->duration) slot = &v;   // steal the most finished voice
    }
    uint32_t seed = slot->noise;
    *slot = Voice{};
    slot->active = true;
    slot->wave = wave;
    slot->freq = from;
    slot->freqEnd = to;
    slot->volume = std::fmin(std::fmax(volume, 0.0f), 1.0f);
    slot->duration = seconds;
    slot->noise = seed * 1664525u + 1013904223u;
}

} // namespace

void audio_init() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = kSampleRate * 2;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&g_device, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(g_event), 0, CALLBACK_EVENT) !=
        MMSYSERR_NOERROR) {
        g_device = nullptr;   // no audio device: sounds are silently ignored
        return;
    }
    for (int i = 0; i < kBufferCount; ++i) {
        WAVEHDR& h = g_headers[i];
        h = WAVEHDR{};
        h.lpData = reinterpret_cast<LPSTR>(g_buffers[i]);
        h.dwBufferLength = sizeof(g_buffers[i]);
        waveOutPrepareHeader(g_device, &h, sizeof(h));
        fill(g_buffers[i]);
        waveOutWrite(g_device, &h, sizeof(h));
    }
    g_running = true;
    g_thread = std::thread(mixerThread);
}

void audio_shutdown() {
    if (!g_device) return;
    g_running = false;
    SetEvent(g_event);
    if (g_thread.joinable()) g_thread.join();
    waveOutReset(g_device);
    for (WAVEHDR& h : g_headers) waveOutUnprepareHeader(g_device, &h, sizeof(h));
    waveOutClose(g_device);
    CloseHandle(g_event);
    g_device = nullptr;
}

RE_API void engine_play_tone(float frequency, float seconds, float volume, int32_t wave) {
    startVoice(frequency, frequency, seconds, volume, wave);
}

RE_API void engine_play_sweep(float fromHz, float toHz, float seconds, float volume, int32_t wave) {
    startVoice(fromHz, toHz, seconds, volume, wave);
}
