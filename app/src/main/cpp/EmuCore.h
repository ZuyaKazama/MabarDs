// EmuNDS - Android frontend for the melonDS core
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "types.h"

struct ANativeWindow;

namespace melonDS { class NDS; class Net; }
using melonDS::u8; using melonDS::u16; using melonDS::u32; using melonDS::u64;
using melonDS::s16; using melonDS::s32;

struct EmuConfig
{
    bool jit = true;
    bool jitFastMem = true;
    int  jitBlockSize = 32;
    bool threaded3D = true;
    int  audioInterpolation = 0;   // 0 none, 1 linear, 2 cosine, 3 cubic
    bool audioEnabled = true;
    int  volume = 256;             // 0..256
    bool directBoot = true;
    int  renderer = 0;             // 0 software, 1 OpenGL ES (hi-res)
    int  scale = 2;                // internal resolution multiplier for the OpenGL renderer
    bool betterPolygons = true;
    int  ffSpeed = 3;              // fast-forward multiplier (0 = unlimited)
    int  frameskip = -1;           // software renderer: -1 auto (only when behind), 0 off, N = draw 1 of N+1
    int  speedHack = 0;            // 0 accurate, 1 fast, 2 fastest: longer ARM9/ARM7 run slices (less accurate timing)

    // firmware
    std::u16string nickname = u"Player";
    int  language = 1;             // 0 JP 1 EN 2 FR 3 DE 4 IT 5 ES
    int  favColor = 0;
    int  birthMonth = 1;
    int  birthDay = 1;
    u8   mac[6] = {0x00, 0x09, 0xBF, 0x11, 0x22, 0x33};
    u8   dns[4] = {0, 0, 0, 0};    // custom DNS for Nintendo WFC replacements, 0 = auto
};

class EmuCore
{
public:
    static EmuCore& Get();

    void Init(const std::string& baseDir);
    void SetConfig(const EmuConfig& cfg);
    EmuConfig GetConfig();

    // ROM / lifecycle (all thread-safe; executed on the emu thread)
    bool LoadROM(int fd, const std::string& romName);
    void Start();
    void Pause(bool paused);
    void Stop();
    void Reset();
    bool IsRunning() const { return running.load(); }
    bool IsPaused() const { return paused.load(); }

    bool SaveState(const std::string& path);
    bool LoadState(const std::string& path);

    // input (lock-free, any thread)
    void SetKeys(u32 mask) { keyMask.store(mask); }
    void Touch(int x, int y, bool down);
    void SetFastForward(bool ff) { fastForward.store(ff); }
    void SetMicBlow(bool on) { micBlow.store(on); }
    void SetLidClosed(bool closed) { lidClosed.store(closed); }

    float GetFPS() const { return fps.load(); }
    // average milliseconds per frame over the last FPS window:
    // emu = RunFrame (CPU emulation + GL command submission), present = drawing + eglSwapBuffers
    float GetEmuMs() const { return emuMs.load(); }
    float GetPresentMs() const { return presentMs.load(); }

    // multiplayer (LAN / direct IP)
    bool MPHost(const std::string& name, int maxPlayers);
    bool MPJoin(const std::string& name, const std::string& host);
    void MPStartDiscovery();
    void MPEndDiscovery();
    void MPEnd();
    std::string MPDiscoveryJson();
    std::string MPPlayersJson();
    int  MPState() const { return mpState.load(); } // 0 none 1 host 2 client

    // called from melonDS Platform callbacks (emu thread)
    void OnSaveWrite(const u8* data, u32 len, u32 offset, u32 writelen);
    void OnFirmwareWrite(u32 offset, u32 len);
    void OnStop();
    int  ReadMic(s16* data, int maxlen);
    int  NetSend(u8* data, int len);
    int  NetRecv(u8* data);

    const std::string& BaseDir() const { return baseDir; }

    // video: the window is rendered to from the emulation thread
    void SetWindow(ANativeWindow* win);
    void SetLayout(const int top[4], const int bottom[4], int filter);
    // 0 software, 1 OpenGL; may differ from the config if GLES 3.2 is unavailable
    int ActiveRenderer() const { return activeRenderer.load(); }

private:
    EmuCore() = default;
    void ThreadMain();
    void RunOnEmu(std::function<void()> fn, bool wait = true);
    void DrainCommands();
    bool CreateNDS();
    void DestroyNDS();
    void FlushSave(bool force);
    void BuildFirmware(void* fwptr);
    void PresentFrame();
    void ApplyRenderer(bool force);
    void LimitFrame(double frameTime);
    bool DecideFrameskip();

    std::string baseDir;
    std::string romName;
    std::string savePath;

    EmuConfig cfg;
    std::mutex cfgLock;

    std::unique_ptr<melonDS::NDS> nds;
    std::unique_ptr<melonDS::Net> net;

    std::thread thread;
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> fastForward{false};
    std::atomic<bool> micBlow{false};
    std::atomic<bool> lidClosed{false};
    std::atomic<u32>  keyMask{0xFFF};
    std::atomic<u32>  touchState{0};   // bit31 = down, x<<8 | y
    std::atomic<float> fps{0.f};
    std::atomic<float> emuMs{0.f};
    std::atomic<float> presentMs{0.f};
    std::atomic<int>  mpState{0};

    // command queue
    std::mutex cmdLock;
    std::condition_variable cmdCv;
    std::deque<std::function<void()>> cmds;

    // save handling
    std::vector<u8> saveBuf;
    bool saveDirty = false;
    u64  saveDirtyTime = 0;
    bool fwDirty = false;

    std::atomic<int> activeRenderer{0};
    int rendererType = -1;

    // mic
    int micPos = 0;

    double frameDeadline = 0;

    std::atomic<int> frameskipCfg{-1};
    std::atomic<int> speedHackCfg{0};
    int skipRun = 0;              // consecutive skipped frames
};
