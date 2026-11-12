// EmuNDS - low-latency audio output via AAudio
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>

namespace melonDS { class SPU; }

class AudioOut
{
public:
    static AudioOut& Get();

    void Init();
    void Start();
    void Stop();

    // The SPU owns a thread-safe ring buffer; the audio callback pulls from it directly.
    void SetSource(melonDS::SPU* spu);
    void SetVolume(int v) { volume.store(v); }
    void SetEnabled(bool e) { enabled.store(e); }
    int SampleRate() const { return sampleRate; }
    // true while the device is actually pulling samples (used as the master clock)
    bool IsActive() const;

private:
    AudioOut() = default;
    bool OpenStream();
    void CloseStream();

    static aaudio_data_callback_result_t DataCb(AAudioStream* s, void* user, void* data, int32_t frames);
    static void ErrorCb(AAudioStream* s, void* user, aaudio_result_t err);

    AAudioStream* stream = nullptr;
    std::mutex lock;          // guards stream + source swap
    std::mutex srcLock;       // held by the callback while reading the SPU
    melonDS::SPU* source = nullptr;
    std::atomic<int> volume{256};
    std::atomic<bool> enabled{true};
    std::atomic<bool> wantRunning{false};
    std::atomic<int64_t> lastCallbackNs{0};
    int sampleRate = 48000;
    short lastL = 0, lastR = 0;
};
