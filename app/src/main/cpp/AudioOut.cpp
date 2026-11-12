// EmuNDS - low-latency audio output via AAudio
// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioOut.h"

#include <android/log.h>
#include <cstring>
#include <thread>
#include <time.h>

#include "SPU.h"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "EmuNDS-Audio", __VA_ARGS__)

AudioOut& AudioOut::Get()
{
    static AudioOut a;
    return a;
}

void AudioOut::Init()
{
    std::lock_guard<std::mutex> lk(lock);
    if (!stream) OpenStream();
}

bool AudioOut::OpenStream()
{
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;

    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setDataCallback(b, &AudioOut::DataCb, this);
    AAudioStreamBuilder_setErrorCallback(b, &AudioOut::ErrorCb, this);

    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK)
    {
        LOGE("openStream failed: %s", AAudio_convertResultToText(r));
        stream = nullptr;
        return false;
    }

    sampleRate = AAudioStream_getSampleRate(stream);
    // A few bursts of device buffering; the real cushion lives in the SPU FIFO,
    // which the emulator keeps topped up (audio-clock pacing in EmuCore).
    int32_t burst = AAudioStream_getFramesPerBurst(stream);
    int32_t want = burst * 3;
    if (want < 512) want = 512;
    AAudioStream_setBufferSizeInFrames(stream, want);
    return true;
}

void AudioOut::CloseStream()
{
    if (!stream) return;
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    stream = nullptr;
}

void AudioOut::Start()
{
    std::lock_guard<std::mutex> lk(lock);
    wantRunning = true;
    if (!stream && !OpenStream()) return;
    AAudioStream_requestStart(stream);
}

void AudioOut::Stop()
{
    std::lock_guard<std::mutex> lk(lock);
    wantRunning = false;
    if (stream) AAudioStream_requestPause(stream);
}

void AudioOut::SetSource(melonDS::SPU* spu)
{
    std::lock_guard<std::mutex> lk(srcLock);
    source = spu;
}

static int64_t MonoNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool AudioOut::IsActive() const
{
    return wantRunning.load() && enabled.load() && (MonoNs() - lastCallbackNs.load()) < 100000000LL;
}

aaudio_data_callback_result_t AudioOut::DataCb(AAudioStream*, void* user, void* data, int32_t frames)
{
    auto* self = static_cast<AudioOut*>(user);
    self->lastCallbackNs.store(MonoNs());
    auto* out = static_cast<int16_t*>(data);

    int got = 0;
    if (self->enabled.load() && self->srcLock.try_lock())
    {
        if (self->source)
            got = self->source->ReadOutput(out, frames);
        self->srcLock.unlock();
    }

    int vol = self->volume.load();
    if (vol != 256)
    {
        for (int i = 0; i < got * 2; i++)
            out[i] = (int16_t)((out[i] * vol) >> 8);
    }

    if (got > 0)
    {
        self->lastL = out[got * 2 - 2];
        self->lastR = out[got * 2 - 1];
    }

    // underrun: hold the last sample and decay it to avoid clicks
    for (int i = got; i < frames; i++)
    {
        self->lastL = (short)(self->lastL * 15 / 16);
        self->lastR = (short)(self->lastR * 15 / 16);
        out[i * 2] = self->lastL;
        out[i * 2 + 1] = self->lastR;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioOut::ErrorCb(AAudioStream*, void* user, aaudio_result_t err)
{
    // Device changed (headphones plugged etc.): reopen on another thread,
    // closing a stream from inside its own callback is not allowed.
    auto* self = static_cast<AudioOut*>(user);
    if (err != AAUDIO_ERROR_DISCONNECTED) return;
    std::thread([self]() {
        std::lock_guard<std::mutex> lk(self->lock);
        self->CloseStream();
        if (self->OpenStream() && self->wantRunning)
            AAudioStream_requestStart(self->stream);
    }).detach();
}
