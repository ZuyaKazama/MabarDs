// EmuNDS - Android frontend for the melonDS core
// SPDX-License-Identifier: GPL-3.0-or-later
#include "EmuCore.h"

#include <android/log.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "GPU.h"
#include "GPU_Soft.h"
#ifdef OGLRENDERER_ENABLED
#include "GPU_OpenGL.h"
#endif
#include "SPU.h"
#include "SPI_Firmware.h"
#include "Savestate.h"
#include "Platform.h"
#include "MPInterface.h"
#include "LAN.h"
#include "Net.h"
#include "Net_Slirp.h"
#include "mic_blow.h"

#include "AudioOut.h"
#include "VideoOut.h"
#include "PerfTuning.h"
#include <android/native_window.h>

using namespace melonDS;

#define LOG_TAG "EmuNDS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr double kFrameRate = 59.8260982880808;
static constexpr unsigned kWfcSize =
    3 * (sizeof(Firmware::WifiAccessPoint) + sizeof(Firmware::ExtendedWifiAccessPoint));

static double NowSec()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static u64 NowMs() { return (u64)(NowSec() * 1000.0); }

static bool WriteFileAtomic(const std::string& path, const void* data, size_t len)
{
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok &= fflush(f) == 0;
    fsync(fileno(f));
    fclose(f);
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

static bool ReadWholeFile(const std::string& path, std::vector<u8>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return false; }
    out.resize(len);
    bool ok = fread(out.data(), 1, len, f) == (size_t)len;
    fclose(f);
    return ok;
}

static LAN& Lan() { return static_cast<LAN&>(MPInterface::Get()); }

static std::string JsonEscape(const char* s)
{
    std::string r;
    for (; *s; s++)
    {
        char c = *s;
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if ((unsigned char)c < 0x20) r += ' ';
        else r += c;
    }
    return r;
}

static std::string IpToString(u32 addr)
{
    // melonDS stores addresses in network byte order inside a u32
    char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, addr >> 24);
    return buf;
}

// ---------------------------------------------------------------------------

EmuCore& EmuCore::Get()
{
    static EmuCore core;
    return core;
}

void EmuCore::Init(const std::string& dir)
{
    if (thread.joinable()) return;

    baseDir = dir;
    mkdir((baseDir + "/saves").c_str(), 0775);
    mkdir((baseDir + "/states").c_str(), 0775);

    net = std::make_unique<Net>();
    net->RegisterInstance(0);
    net->SetDriver(std::make_unique<Net_Slirp>([this](const u8* data, int len) {
        net->RXEnqueue(data, len);
    }));

    AudioOut::Get().Init();

    quit = false;
    thread = std::thread(&EmuCore::ThreadMain, this);
}

void EmuCore::SetConfig(const EmuConfig& c)
{
    {
        std::lock_guard<std::mutex> lk(cfgLock);
        cfg = c;
    }
    AudioOut::Get().SetVolume(c.volume);
    AudioOut::Get().SetEnabled(c.audioEnabled);
    frameskipCfg.store(c.frameskip);
    speedHackCfg.store(c.speedHack);

    RunOnEmu([this]() {
        if (!nds) return;
        EmuConfig c2 = GetConfig();
        nds->SPU.SetInterpolation((AudioInterpolation)c2.audioInterpolation);
        ApplyRenderer(false);
    }, true); // synchronous so ActiveRenderer() is up to date for the UI
}

EmuConfig EmuCore::GetConfig()
{
    std::lock_guard<std::mutex> lk(cfgLock);
    return cfg;
}

void EmuCore::RunOnEmu(std::function<void()> fn, bool wait)
{
    if (std::this_thread::get_id() == thread.get_id())
    {
        fn();
        return;
    }

    if (!wait)
    {
        std::lock_guard<std::mutex> lk(cmdLock);
        cmds.emplace_back(std::move(fn));
        cmdCv.notify_all();
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    {
        std::lock_guard<std::mutex> lk(cmdLock);
        cmds.emplace_back([fn = std::move(fn), done]() { fn(); done->set_value(); });
        cmdCv.notify_all();
    }
    fut.wait();
}

void EmuCore::DrainCommands()
{
    std::deque<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lk(cmdLock);
        local.swap(cmds);
    }
    for (auto& fn : local) fn();
}

// ---------------------------------------------------------------------------
// Firmware

void EmuCore::BuildFirmware(void* fwptr)
{
    Firmware& fw = *static_cast<Firmware*>(fwptr);
    EmuConfig c = GetConfig();

    // persistent Nintendo WFC settings (access points, WFC user ID)
    std::vector<u8> wfc;
    if (ReadWholeFile(baseDir + "/wfcsettings.bin", wfc) && wfc.size() >= kWfcSize)
        memcpy(fw.GetExtendedAccessPointPosition(), wfc.data(), kWfcSize);

    // custom DNS on the first access point (e.g. for Wiimmfi / AltWFC)
    if (c.dns[0] | c.dns[1] | c.dns[2] | c.dns[3])
    {
        auto& ap = fw.GetAccessPoints()[0];
        memcpy(&ap.PrimaryDns, c.dns, 4);
        memcpy(&ap.SecondaryDns, c.dns, 4);
    }

    auto& user = fw.GetEffectiveUserData();
    size_t nl = std::min<size_t>(c.nickname.size(), 10);
    if (nl > 0)
    {
        memset(user.Nickname, 0, sizeof(user.Nickname));
        memcpy(user.Nickname, c.nickname.data(), nl * sizeof(char16_t));
        user.NameLength = (u16)nl;
    }
    if (c.language >= 0 && c.language <= 5)
    {
        user.Settings &= ~Firmware::Language::Reserved;
        user.Settings |= (Firmware::Language)c.language;
    }
    user.FavoriteColor = (u8)(c.favColor & 0xF);
    if (c.birthMonth >= 1 && c.birthMonth <= 12) user.BirthdayMonth = (u8)c.birthMonth;
    if (c.birthDay >= 1 && c.birthDay <= 31) user.BirthdayDay = (u8)c.birthDay;

    // unique MAC per device: required for local/online multiplayer
    auto& header = fw.GetHeader();
    MacAddress mac;
    memcpy(mac.data(), c.mac, 6);
    mac[0] &= 0xFC;
    header.MacAddr = mac;
    header.UpdateChecksum();

    fw.UpdateChecksums();
}

void EmuCore::OnFirmwareWrite(u32 offset, u32 len)
{
    // Only the WFC area is worth persisting for a generated firmware.
    fwDirty = true;
}

// ---------------------------------------------------------------------------
// NDS lifetime (emu thread only)

bool EmuCore::CreateNDS()
{
    EmuConfig c = GetConfig();

    NDSArgs args;
    args.Firmware = Firmware(0);
    BuildFirmware(&args.Firmware);
    if (c.jit)
        args.JIT = JITArgs{(unsigned)c.jitBlockSize, true, true, c.jitFastMem};
    else
        args.JIT = std::nullopt;
    args.Interpolation = (AudioInterpolation)c.audioInterpolation;
    args.OutputSampleRate = AudioOut::Get().SampleRate();

    nds = std::make_unique<NDS>(std::move(args), this);
    rendererType = -1;
    ApplyRenderer(true);
    nds->Reset();
    return true;
}

void EmuCore::DestroyNDS()
{
    if (!nds) return;
    AudioOut::Get().SetSource(nullptr);
    FlushSave(true);
    if (running) nds->Stop();
    running = false;
    MPInterface::Get().End(0);
    nds.reset();
}

bool EmuCore::LoadROM(int fd, const std::string& name)
{
    bool ok = false;
    RunOnEmu([&]() {
        DestroyNDS();

        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) { LOGE("fstat failed"); return; }
        u32 len = (u32)st.st_size;
        auto data = std::make_unique<u8[]>(len);
        size_t got = 0;
        while (got < len)
        {
            ssize_t r = pread(fd, data.get() + got, len - got, got);
            if (r <= 0) break;
            got += r;
        }
        if (got != len) { LOGE("short ROM read %zu/%u", got, len); return; }

        romName = name;
        savePath = baseDir + "/saves/" + name + ".sav";

        std::vector<u8> save;
        ReadWholeFile(savePath, save);

        NDSCart::NDSCartArgs cargs;
        if (!save.empty())
        {
            cargs.SRAM = std::make_unique<u8[]>(save.size());
            memcpy(cargs.SRAM.get(), save.data(), save.size());
            cargs.SRAMLength = (u32)save.size();
        }

        auto cart = NDSCart::ParseROM(std::move(data), len, this, std::move(cargs));
        if (!cart) { LOGE("ParseROM failed"); return; }

        if (!CreateNDS()) return;
        nds->SetNDSCart(std::move(cart));
        nds->Reset();
        // FreeBIOS / generated firmware always needs direct boot
        nds->SetupDirectBoot(name);

        saveBuf.clear();
        saveDirty = false;
        ok = true;
    });
    return ok;
}

void EmuCore::Start()
{
    RunOnEmu([this]() {
        if (!nds || running) return;
        nds->Start();
        running = true;
        paused = false;
        frameDeadline = 0;
        AudioOut::Get().SetSource(&nds->SPU);
        AudioOut::Get().Start();
    });
}

void EmuCore::Pause(bool p)
{
    RunOnEmu([this, p]() {
        paused = p;
        if (p)
        {
            FlushSave(true);
            AudioOut::Get().Stop();
        }
        else
        {
            frameDeadline = 0;
            AudioOut::Get().Start();
        }
    });
}

void EmuCore::Stop()
{
    RunOnEmu([this]() {
        AudioOut::Get().Stop();
        DestroyNDS();
    });
}

void EmuCore::Reset()
{
    RunOnEmu([this]() {
        if (!nds) return;
        FlushSave(true);
        nds->Reset();
        nds->SetupDirectBoot(romName);
        if (running) nds->Start();
    });
}

void EmuCore::OnStop()
{
    // called by the core (e.g. power-off from the game)
    running = false;
}

// ---------------------------------------------------------------------------
// Savestates

bool EmuCore::SaveState(const std::string& path)
{
    bool ok = false;
    RunOnEmu([&]() {
        if (!nds) return;
        Savestate st;
        if (st.Error) return;
        nds->DoSavestate(&st);
        st.Finish();
        if (st.Error) return;
        ok = WriteFileAtomic(path, st.Buffer(), st.Length());
    });
    return ok;
}

bool EmuCore::LoadState(const std::string& path)
{
    bool ok = false;
    RunOnEmu([&]() {
        if (!nds) return;
        std::vector<u8> data;
        if (!ReadWholeFile(path, data)) return;

        // keep a backup so a corrupt state cannot kill the session
        Savestate backup;
        nds->DoSavestate(&backup);
        backup.Finish();

        Savestate st(data.data(), (u32)data.size(), false);
        if (st.Error) return;
        ok = nds->DoSavestate(&st) && !st.Error;
        if (!ok && !backup.Error)
        {
            Savestate restore(backup.Buffer(), backup.Length(), false);
            nds->DoSavestate(&restore);
        }
        frameDeadline = 0;
    });
    return ok;
}

// ---------------------------------------------------------------------------
// Saves

void EmuCore::OnSaveWrite(const u8* data, u32 len, u32 offset, u32 writelen)
{
    if (saveBuf.size() != len) saveBuf.assign(data, data + len);
    else memcpy(saveBuf.data() + offset, data + offset, writelen);
    saveDirty = true;
    saveDirtyTime = NowMs();
}

void EmuCore::FlushSave(bool force)
{
    u64 now = NowMs();
    if (saveDirty && (force || now - saveDirtyTime > 1500))
    {
        if (WriteFileAtomic(savePath, saveBuf.data(), saveBuf.size()))
            saveDirty = false;
        else
            LOGE("failed to write save %s", savePath.c_str());
    }
    if (fwDirty && nds)
    {
        Firmware& fw = nds->GetFirmware();
        WriteFileAtomic(baseDir + "/wfcsettings.bin", fw.GetExtendedAccessPointPosition(), kWfcSize);
        fwDirty = false;
    }
}

// ---------------------------------------------------------------------------
// Input / mic / net

void EmuCore::Touch(int x, int y, bool down)
{
    if (!down) { touchState.store(0); return; }
    x = std::clamp(x, 0, 255);
    y = std::clamp(y, 0, 191);
    touchState.store(0x80000000u | ((u32)x << 8) | (u32)y);
}

int EmuCore::ReadMic(s16* data, int maxlen)
{
    if (!micBlow)
    {
        micPos = 0;
        memset(data, 0, maxlen * sizeof(s16));
        return maxlen;
    }
    const int total = sizeof(mic_blow) / sizeof(s16);
    for (int i = 0; i < maxlen; i++)
    {
        data[i] = mic_blow[micPos++];
        if (micPos >= total) micPos = 0;
    }
    return maxlen;
}

int EmuCore::NetSend(u8* data, int len) { return net ? net->SendPacket(data, len, 0) : 0; }
int EmuCore::NetRecv(u8* data) { return net ? net->RecvPacket(data, 0) : 0; }

// ---------------------------------------------------------------------------
// Multiplayer

bool EmuCore::MPHost(const std::string& name, int maxPlayers)
{
    bool ok = false;
    RunOnEmu([&]() {
        if (MPInterface::GetType() != MPInterface_LAN)
            MPInterface::Set(MPInterface_LAN);
        Lan().EndSession();
        ok = Lan().StartHost(name.c_str(), std::clamp(maxPlayers, 2, 16));
        mpState = ok ? 1 : 0;
    });
    return ok;
}

bool EmuCore::MPJoin(const std::string& name, const std::string& host)
{
    bool ok = false;
    RunOnEmu([&]() {
        if (MPInterface::GetType() != MPInterface_LAN)
            MPInterface::Set(MPInterface_LAN);
        Lan().EndDiscovery();
        ok = Lan().StartClient(name.c_str(), host.c_str());
        mpState = ok ? 2 : 0;
    });
    return ok;
}

void EmuCore::MPStartDiscovery()
{
    RunOnEmu([&]() {
        if (MPInterface::GetType() != MPInterface_LAN)
            MPInterface::Set(MPInterface_LAN);
        Lan().StartDiscovery();
    });
}

void EmuCore::MPEndDiscovery()
{
    RunOnEmu([&]() {
        if (MPInterface::GetType() == MPInterface_LAN)
            Lan().EndDiscovery();
    });
}

void EmuCore::MPEnd()
{
    RunOnEmu([&]() {
        if (MPInterface::GetType() == MPInterface_LAN)
        {
            Lan().EndSession();
            Lan().EndDiscovery();
        }
        MPInterface::Set(MPInterface_Dummy);
        mpState = 0;
    });
}

std::string EmuCore::MPDiscoveryJson()
{
    std::string out = "[";
    RunOnEmu([&]() {
        if (MPInterface::GetType() != MPInterface_LAN) return;
        bool first = true;
        for (auto& [addr, d] : Lan().GetDiscoveryList())
        {
            char buf[256];
            char name[65] = {0};
            memcpy(name, d.SessionName, 64);
            snprintf(buf, sizeof(buf),
                     "%s{\"ip\":\"%s\",\"name\":\"%s\",\"players\":%d,\"max\":%d,\"status\":%d}",
                     first ? "" : ",", IpToString(addr).c_str(), JsonEscape(name).c_str(),
                     d.NumPlayers, d.MaxPlayers, d.Status);
            out += buf;
            first = false;
        }
    });
    return out + "]";
}

std::string EmuCore::MPPlayersJson()
{
    std::string out = "[";
    RunOnEmu([&]() {
        if (MPInterface::GetType() != MPInterface_LAN) return;
        bool first = true;
        for (auto& p : Lan().GetPlayerList())
        {
            char buf[256];
            char name[33] = {0};
            memcpy(name, p.Name, 32);
            snprintf(buf, sizeof(buf),
                     "%s{\"id\":%d,\"name\":\"%s\",\"status\":%d,\"ip\":\"%s\",\"local\":%s,\"ping\":%u}",
                     first ? "" : ",", p.ID, JsonEscape(name).c_str(), (int)p.Status,
                     IpToString(p.Address).c_str(), p.IsLocalPlayer ? "true" : "false", p.Ping);
            out += buf;
            first = false;
        }
    });
    return out + "]";
}

// ---------------------------------------------------------------------------
// Video handoff

void EmuCore::ApplyRenderer(bool force)
{
    if (!nds) return;
    EmuConfig c = GetConfig();
    int want = (c.renderer == 1 && VideoOut::Get().HasGLES32()) ? 1 : 0;
#ifndef OGLRENDERER_ENABLED
    want = 0;
#endif
    if (force || want != rendererType)
    {
#ifdef OGLRENDERER_ENABLED
        if (want == 1)
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
        else
#endif
            nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
        rendererType = want;
    }

    int actual = 0;
#ifdef OGLRENDERER_ENABLED
    // the core silently falls back to software if GL init fails
    actual = dynamic_cast<GLRenderer*>(&nds->GetRenderer()) ? 1 : 0;
#endif
    activeRenderer = actual;
    RendererSettings rs{actual ? std::clamp(c.scale, 1, 8) : 1, c.threaded3D, false, c.betterPolygons};
    nds->GetRenderer().SetRenderSettings(rs);
    VideoOut::Get().RequestRedraw();
}

void EmuCore::PresentFrame()
{
    if (!VideoOut::Get().HasWindow()) return;
    if (!nds) { VideoOut::Get().PresentBlank(); return; }
    void* top = nullptr;
    void* bottom = nullptr;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        if (top && bottom) VideoOut::Get().PresentSoft((const u32*)top, (const u32*)bottom);
    }
    else if (top)
    {
        VideoOut::Get().PresentTexture(*(const unsigned*)top);
    }
}

void EmuCore::SetWindow(ANativeWindow* win)
{
    RunOnEmu([this, win]() {
        VideoOut::Get().SetWindow(win);
        PresentFrame();
    });
}

void EmuCore::SetLayout(const int top[4], const int bottom[4], int filter)
{
    VideoOut::Get().SetLayout(top, bottom);
    VideoOut::Get().SetFilter(filter);
}

// ---------------------------------------------------------------------------
// Main loop

void EmuCore::LimitFrame(double frameTime)
{
    double now = NowSec();
    if (frameDeadline == 0 || now - frameDeadline > 0.1)
        frameDeadline = now; // resync after a hitch instead of fast-forwarding to catch up

    frameDeadline += frameTime;
    double wait = frameDeadline - now;
    if (wait > 0.002)
    {
        // coarse sleep, then a short spin for accuracy
        double target = frameDeadline - 0.001;
        timespec ts;
        double sl = target - now;
        ts.tv_sec = (time_t)sl;
        ts.tv_nsec = (long)((sl - ts.tv_sec) * 1e9);
        nanosleep(&ts, nullptr);
    }
    while (NowSec() < frameDeadline) { /* spin < 1ms */ }
}

bool EmuCore::DecideFrameskip()
{
    int fs = frameskipCfg.load();
    // only the software renderer supports skipping; the GL renderer keeps per-frame GPU state
    if (fs == 0 || activeRenderer.load() != 0) { skipRun = 0; return false; }

    bool skip;
    if (fs > 0)
    {
        skip = skipRun < fs; // draw 1 frame, skip fs
    }
    else
    {
        // auto: skip drawing only while emulation is behind real time, at most 3 in a row
        bool behind;
        if (AudioOut::Get().IsActive() && !(fastForward && mpState == 0))
            behind = nds->SPU.GetOutputSize() < AudioOut::Get().SampleRate() / 80; // < ~12 ms queued
        else
            behind = frameDeadline != 0 && NowSec() > frameDeadline + 0.004;
        skip = behind && skipRun < 3;
    }
    skipRun = skip ? skipRun + 1 : 0;
    return skip;
}

void EmuCore::ThreadMain()
{
    // close to THREAD_PRIORITY_URGENT_DISPLAY; ignored if the kernel refuses
    setpriority(PRIO_PROCESS, gettid(), -8);
    PerfTuning::PinToBigCores();
    PerfTuning::StartHintSession(16666667);

    // one GL context for the whole session: used by the OpenGL renderer and the presenter
    if (!VideoOut::Get().InitEGL()) LOGE("EGL init failed, no video output");

    double fpsTime = NowSec();
    int fpsFrames = 0;
    double emuAcc = 0, presentAcc = 0, perfLogTime = NowSec();

    while (!quit)
    {
        DrainCommands();

        // Multiplayer networking must be serviced even while idle in the lobby.
        MPInterface::Get().Process();

        if (!nds || !running || paused)
        {
            if (VideoOut::Get().ConsumeRedraw()) PresentFrame();
            std::unique_lock<std::mutex> lk(cmdLock);
            cmdCv.wait_for(lk, std::chrono::milliseconds(16), [this] { return !cmds.empty() || quit; });
            fpsTime = NowSec();
            fpsFrames = 0;
            fps = 0.f;
            continue;
        }

        // OpenGL renderer: compile shaders in small steps so the UI stays responsive
        if (nds->GetRenderer().NeedsShaderCompile())
        {
            int cur = 0, count = 0;
            double t0 = NowSec();
            do nds->GetRenderer().ShaderCompileStep(cur, count);
            while (nds->GetRenderer().NeedsShaderCompile() && NowSec() - t0 < 0.1);
            continue;
        }

        nds->SetKeyMask(keyMask.load());
        u32 t = touchState.load();
        if (t & 0x80000000u) nds->TouchScreen((t >> 8) & 0xFF, t & 0xFF);
        else nds->ReleaseScreen();
        bool lid = lidClosed.load();
        if (nds->IsLidClosed() != lid) nds->SetLidClosed(lid);

        bool skip = DecideFrameskip();
#ifdef EMUNDS_FRAMESKIP
        nds->GPU.SkipDraw = skip;
        // CPU resync interval (33 MHz cycles). Measured on LEGO Batman: 256 still renders correctly,
        // 384+ loses the 3D scene. Multiplayer always runs accurate so wireless timing stays intact.
        static const s32 kSlices[3] = {64, 128, 256};
        int sh = std::clamp(speedHackCfg.load(), 0, 2);
        melonDS::EmundsMaxIterationCycles = mpState.load() ? 64 : kSlices[sh];
#endif
        double tRun = NowSec();
        u32 nlines = nds->RunFrame();
#ifdef EMUNDS_FRAMESKIP
        nds->GPU.SkipDraw = false;
#endif
        if (!running) continue; // core requested stop

        double tPresent = NowSec();
        bool redraw = VideoOut::Get().ConsumeRedraw();
        if (!skip || redraw) PresentFrame();
        double tDone = NowSec();
        emuAcc += tPresent - tRun;
        presentAcc += tDone - tPresent;
        PerfTuning::ReportWork((int64_t)((tDone - tRun) * 1e9));
        FlushSave(false);

        fpsFrames++;
        double now = NowSec();
        if (now - fpsTime >= 0.5)
        {
            fps = (float)(fpsFrames / (now - fpsTime));
            emuMs = (float)(emuAcc * 1000.0 / fpsFrames);
            presentMs = (float)(presentAcc * 1000.0 / fpsFrames);
            if (now - perfLogTime >= 5.0)
            {
                EmuConfig c = GetConfig();
                LOGI("perf: %.1f fps, emu %.2f ms, present %.2f ms (renderer %d, scale %d)",
                     fps.load(), emuMs.load(), presentMs.load(), activeRenderer.load(), activeRenderer ? c.scale : 1);
                perfLogTime = now;
            }
            fpsTime = now;
            fpsFrames = 0;
            emuAcc = presentAcc = 0;
        }

        double frameTime = nlines / (kFrameRate * 263.0);
        if (frameTime < 0.001) frameTime = 0.001;

        bool ff = fastForward && mpState == 0; // multiplayer must stay in lockstep
        PerfTuning::UpdateTarget((int64_t)(frameTime * 1e9 / (ff ? std::max(GetConfig().ffSpeed, 1) : 1)));
        if (ff)
        {
            int mul = GetConfig().ffSpeed;
            if (mul <= 0) { frameDeadline = 0; continue; }
            LimitFrame(frameTime / mul);
        }
        else if (AudioOut::Get().IsActive())
        {
            // Audio is the master clock: run ahead until the SPU FIFO holds a
            // ~25 ms cushion, then wait for the device to drain it. This removes
            // the crackling caused by emulation and DAC drifting apart.
            const int target = AudioOut::Get().SampleRate() / 40;
            double t0 = NowSec();
            while (nds->SPU.GetOutputSize() > target && NowSec() - t0 < 0.05)
            {
                timespec ts{0, 500000};
                nanosleep(&ts, nullptr);
            }
            frameDeadline = 0;
        }
        else
        {
            LimitFrame(frameTime);
        }
    }

    DestroyNDS();
    VideoOut::Get().ShutdownEGL();
    PerfTuning::StopHintSession();
}
