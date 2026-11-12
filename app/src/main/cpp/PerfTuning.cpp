// EmuNDS - CPU scheduling help for the emulation threads.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PerfTuning.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "EmuNDS-Perf", __VA_ARGS__)

namespace PerfTuning
{

static long ReadLong(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

// cpu indices of the fast cluster(s); empty if the CPU is uniform or unknown
static const std::vector<int>& BigCores()
{
    static std::vector<int> big;
    static std::once_flag once;
    std::call_once(once, []() {
        int n = (int)sysconf(_SC_NPROCESSORS_CONF);
        if (n <= 1 || n > 64) return;
        std::vector<long> score(n, -1);
        long best = 0;
        bool haveCapacity = true;
        for (int i = 0; i < n; i++)
        {
            char p[96];
            snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
            score[i] = ReadLong(p);
            if (score[i] <= 0) { haveCapacity = false; break; }
        }
        if (!haveCapacity)
        {
            for (int i = 0; i < n; i++)
            {
                char p[96];
                snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
                score[i] = ReadLong(p);
            }
        }
        for (long s : score) best = s > best ? s : best;
        if (best <= 0) return;
        // "big" = within 30% of the fastest core (prime + big on 1+3+4 designs, the A7x pair on 2+6),
        // but never fewer than 2 cores: the emulation thread and the 3D render thread run in parallel
        long threshold = best * 7 / 10;
        for (;;)
        {
            big.clear();
            long next = 0; // fastest score below the threshold, to widen the set if needed
            for (int i = 0; i < n; i++)
            {
                if (score[i] >= threshold) big.push_back(i);
                else if (score[i] > next) next = score[i];
            }
            if (big.size() >= 2 || next <= 0) break;
            threshold = next;
        }
        if ((int)big.size() == n || big.size() < 2) big.clear(); // uniform: let the scheduler decide
        LOGI("big cores: %zu of %d (%s)", big.size(), n, haveCapacity ? "capacity" : "frequency");
    });
    return big;
}

void PinToBigCores()
{
    const auto& big = BigCores();
    if (big.empty()) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : big) CPU_SET(c, &set);
    sched_setaffinity(gettid(), sizeof(set), &set);
}

// ---------------------------------------------------------------------------
// ADPF (APerformanceHint_*, libandroid.so, Android 13 / API 33+)

struct APerformanceHintManager;
struct APerformanceHintSession;
typedef APerformanceHintManager* (*FnGetManager)();
typedef APerformanceHintSession* (*FnCreateSession)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
typedef int (*FnReport)(APerformanceHintSession*, int64_t);
typedef int (*FnUpdateTarget)(APerformanceHintSession*, int64_t);
typedef void (*FnClose)(APerformanceHintSession*);

static FnReport sReport = nullptr;
static FnUpdateTarget sUpdate = nullptr;
static FnClose sClose = nullptr;
static APerformanceHintSession* sSession = nullptr;
static int64_t sTarget = 0;

void StartHintSession(int64_t targetNs)
{
    if (sSession) return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    auto getManager = (FnGetManager)dlsym(lib, "APerformanceHint_getManager");
    auto create = (FnCreateSession)dlsym(lib, "APerformanceHint_createSession");
    sReport = (FnReport)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    sUpdate = (FnUpdateTarget)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    sClose = (FnClose)dlsym(lib, "APerformanceHint_closeSession");
    if (!getManager || !create || !sReport || !sUpdate || !sClose) { LOGI("ADPF unavailable"); return; }
    APerformanceHintManager* mgr = getManager();
    if (!mgr) return;
    int32_t tid = gettid();
    sSession = create(mgr, &tid, 1, targetNs);
    sTarget = targetNs;
    LOGI("ADPF hint session %s", sSession ? "active" : "refused");
}

void ReportWork(int64_t actualNs)
{
    if (sSession && actualNs > 0) sReport(sSession, actualNs);
}

void UpdateTarget(int64_t targetNs)
{
    if (sSession && targetNs > 0 && targetNs != sTarget)
    {
        sUpdate(sSession, targetNs);
        sTarget = targetNs;
    }
}

void StopHintSession()
{
    if (sSession) sClose(sSession);
    sSession = nullptr;
}

}
