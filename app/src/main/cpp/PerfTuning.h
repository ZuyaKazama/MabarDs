// EmuNDS - CPU scheduling help for the emulation threads (all Android phones, any SoC).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace PerfTuning
{
// Pins the calling thread to the fast cores ("big"/"prime" cluster) when the SoC is
// heterogeneous. Detected from cpu_capacity (or max frequency). No-op on uniform CPUs.
void PinToBigCores();

// Android Dynamic Performance Framework (hint sessions, Android 13+, loaded at runtime):
// tells the scheduler how long each frame's work took versus the 16.7 ms target, so the CPU
// governor ramps up before frames are dropped instead of after.
void StartHintSession(int64_t targetNs); // call from the emulation thread
void ReportWork(int64_t actualNs);
void UpdateTarget(int64_t targetNs);
void StopHintSession();
}
