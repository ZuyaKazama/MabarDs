// EmuNDS - melonDS Platform layer for Android
// SPDX-License-Identifier: GPL-3.0-or-later
#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "Platform.h"
#include "SPI_Firmware.h"
#include "MPInterface.h"

#include "EmuCore.h"
#include "PerfTuning.h"

namespace melonDS::Platform
{

static EmuCore& Core() { return EmuCore::Get(); }

void SignalStop(StopReason reason, void*)
{
    Log(LogLevel::Info, "core stop requested (reason %d)\n", (int)reason);
    Core().OnStop();
}

// ---------------------------------------------------------------------------
// Files

struct FileHandle { FILE* f; };

static std::string ModeString(FileMode mode, bool exists)
{
    bool r = mode & FileMode::Read;
    bool w = mode & FileMode::Write;
    bool app = mode & FileMode::Append;
    bool preserve = mode & FileMode::Preserve;

    std::string s;
    if (app)                s = r ? "a+" : "a";
    else if (r && w)        s = (preserve && exists) ? "r+" : "w+";
    else if (w)             s = (preserve && exists) ? "r+" : "w";
    else                    s = "r";
    if (!(mode & FileMode::Text)) s += "b";
    return s;
}

std::string GetLocalFilePath(const std::string& filename)
{
    if (!filename.empty() && filename[0] == '/') return filename;
    return Core().BaseDir() + "/" + filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    struct stat st{};
    bool exists = stat(path.c_str(), &st) == 0;
    if ((mode & FileMode::NoCreate) && !exists) return nullptr;
    if (!(mode & FileMode::Write) && !exists) return nullptr;

    FILE* f = fopen(path.c_str(), ModeString(mode, exists).c_str());
    if (!f) return nullptr;
    return new FileHandle{f};
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    struct stat st{};
    return stat(name.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool LocalFileExists(const std::string& name) { return FileExists(GetLocalFilePath(name)); }

bool CheckFileWritable(const std::string& filepath)
{
    FileHandle* f = OpenFile(filepath, FileMode::ReadWriteExisting);
    if (f) { CloseFile(f); return true; }
    if (FileExists(filepath)) return false;
    return access(filepath.substr(0, filepath.find_last_of('/')).c_str(), W_OK) == 0;
}

bool CheckLocalFileWritable(const std::string& filepath) { return CheckFileWritable(GetLocalFilePath(filepath)); }

bool CloseFile(FileHandle* file)
{
    if (!file) return false;
    bool ok = fclose(file->f) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle* file) { return feof(file->f) != 0; }
bool FileReadLine(char* str, int count, FileHandle* file) { return fgets(str, count, file->f) != nullptr; }
u64 FilePosition(FileHandle* file) { return (u64)ftello(file->f); }

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int o = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseeko(file->f, offset, o) == 0;
}

void FileRewind(FileHandle* file) { rewind(file->f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return fread(data, size, count, file->f); }
bool FileFlush(FileHandle* file) { return fflush(file->f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) { return fwrite(data, size, count, file->f); }

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int r = vfprintf(file->f, fmt, args);
    va_end(args);
    return r < 0 ? 0 : (u64)r;
}

u64 FileLength(FileHandle* file)
{
    off_t cur = ftello(file->f);
    fseeko(file->f, 0, SEEK_END);
    off_t len = ftello(file->f);
    fseeko(file->f, cur, SEEK_SET);
    return (u64)len;
}

// ---------------------------------------------------------------------------
// Logging

void Log(LogLevel level, const char* fmt, ...)
{
    int prio = ANDROID_LOG_DEBUG;
    switch (level)
    {
        case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:  prio = ANDROID_LOG_INFO; break;
        case LogLevel::Warn:  prio = ANDROID_LOG_WARN; break;
        case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
    }
#ifdef NDEBUG
    if (prio < ANDROID_LOG_WARN) return;
#endif
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(prio, "melonDS", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Threading

struct Thread { std::thread t; };

Thread* Thread_Create(std::function<void()> func)
{
    // core threads (threaded 3D renderer) belong on the fast cores like the emulation thread
    return new Thread{std::thread([f = std::move(func)]() {
        PerfTuning::PinToBigCores();
        f();
    })};
}

void Thread_Free(Thread* thread)
{
    if (!thread) return;
    if (thread->t.joinable()) thread->t.detach();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread && thread->t.joinable()) thread->t.join();
}

struct Semaphore
{
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore(); }
void Semaphore_Free(Semaphore* s) { delete s; }

void Semaphore_Reset(Semaphore* s)
{
    std::lock_guard<std::mutex> lk(s->m);
    s->count = 0;
}

void Semaphore_Wait(Semaphore* s)
{
    std::unique_lock<std::mutex> lk(s->m);
    s->cv.wait(lk, [s] { return s->count > 0; });
    s->count--;
}

bool Semaphore_TryWait(Semaphore* s, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(s->m);
    if (timeout_ms <= 0)
    {
        if (s->count <= 0) return false;
    }
    else if (!s->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [s] { return s->count > 0; }))
    {
        return false;
    }
    s->count--;
    return true;
}

void Semaphore_Post(Semaphore* s, int count)
{
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->count += count;
    }
    if (count == 1) s->cv.notify_one();
    else s->cv.notify_all();
}

struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }

void Sleep(u64 usecs) { usleep((useconds_t)usecs); }

static u64 MonoUS()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

u64 GetMSCount() { return MonoUS() / 1000; }
u64 GetUSCount() { return MonoUS(); }

// ---------------------------------------------------------------------------
// Saves / firmware / RTC

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void*)
{
    Core().OnSaveWrite(savedata, savelen, writeoffset, writelen);
}

void WriteGBASave(const u8*, u32, u32, u32, void*) {}

void WriteFirmware(const Firmware&, u32 writeoffset, u32 writelen, void*)
{
    Core().OnFirmwareWrite(writeoffset, writelen);
}

void WriteDateTime(int, int, int, int, int, int, void*) {}

// ---------------------------------------------------------------------------
// Local wireless (forwarded to the active MPInterface: LAN / direct IP)

void MP_Begin(void*) { MPInterface::Get().Begin(0); }
void MP_End(void*) { MPInterface::Get().End(0); }
int MP_SendPacket(u8* data, int len, u64 ts, void*) { return MPInterface::Get().SendPacket(0, data, len, ts); }
int MP_RecvPacket(u8* data, u64* ts, void*) { return MPInterface::Get().RecvPacket(0, data, ts); }
int MP_SendCmd(u8* data, int len, u64 ts, void*) { return MPInterface::Get().SendCmd(0, data, len, ts); }
int MP_SendReply(u8* data, int len, u64 ts, u16 aid, void*) { return MPInterface::Get().SendReply(0, data, len, ts, aid); }
int MP_SendAck(u8* data, int len, u64 ts, void*) { return MPInterface::Get().SendAck(0, data, len, ts); }
int MP_RecvHostPacket(u8* data, u64* ts, void*) { return MPInterface::Get().RecvHostPacket(0, data, ts); }
u16 MP_RecvReplies(u8* data, u64 ts, u16 aidmask, void*) { return MPInterface::Get().RecvReplies(0, data, ts, aidmask); }

// Internet access point (Nintendo WFC replacements) via slirp
int Net_SendPacket(u8* data, int len, void*) { Core().NetSend(data, len); return 0; }
int Net_RecvPacket(u8* data, void*) { return Core().NetRecv(data); }

// ---------------------------------------------------------------------------
// Camera / mic / addons

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool yuv, void*)
{
    // grey image
    int n = yuv ? (width * height / 2) : (width * height);
    for (int i = 0; i < n; i++) frame[i] = yuv ? 0x80808080 : 0xFF808080;
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* data, int maxlength, void*) { return Core().ReadMic(data, maxlength); }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType type, void*)
{
    return type == MotionAccelerationZ ? 9.80665f : 0.f;
}

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return (DynamicLibrary*)dlopen(lib, RTLD_NOW | RTLD_LOCAL); }
void DynamicLibrary_Unload(DynamicLibrary* lib) { if (lib) dlclose(lib); }
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return dlsym(lib, name); }

} // namespace melonDS::Platform
