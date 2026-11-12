// EmuNDS - JNI bridge
// SPDX-License-Identifier: GPL-3.0-or-later
#include <jni.h>
#include <android/native_window_jni.h>
#include <string>

#include "EmuCore.h"
#include "VideoOut.h"

static std::string JStr(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

static std::u16string JStr16(JNIEnv* env, jstring s)
{
    if (!s) return {};
    jsize n = env->GetStringLength(s);
    const jchar* c = env->GetStringChars(s, nullptr);
    std::u16string r((const char16_t*)c, n);
    env->ReleaseStringChars(s, c);
    return r;
}

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_emunds_nds_NativeLib_##name

JNI(void, init)(JNIEnv* env, jclass, jstring baseDir)
{
    EmuCore::Get().Init(JStr(env, baseDir));
}

// ints: jit, jitFastMem, jitBlockSize, threaded3D, audioInterp, audioEnabled, volume,
//       ffSpeed, language, favColor, birthMonth, birthDay, renderer, scale, betterPolygons
JNI(void, setConfig)(JNIEnv* env, jclass, jintArray ints, jstring nickname, jbyteArray mac, jbyteArray dns)
{
    EmuConfig c = EmuCore::Get().GetConfig();
    jint v[17] = {0, 0, 32, 1, 0, 1, 256, 3, 1, 0, 1, 1, 0, 2, 1, -1, 0};
    jsize n = env->GetArrayLength(ints);
    env->GetIntArrayRegion(ints, 0, n < 17 ? n : 17, v);
    c.jit = v[0] != 0;
    c.jitFastMem = v[1] != 0;
    c.jitBlockSize = v[2] > 0 ? v[2] : 32;
    c.threaded3D = v[3] != 0;
    c.audioInterpolation = v[4];
    c.audioEnabled = v[5] != 0;
    c.volume = v[6];
    c.ffSpeed = v[7];
    c.language = v[8];
    c.favColor = v[9];
    c.birthMonth = v[10];
    c.birthDay = v[11];
    c.renderer = v[12];
    c.scale = v[13];
    c.betterPolygons = v[14] != 0;
    c.frameskip = v[15];
    c.speedHack = v[16];
    c.nickname = JStr16(env, nickname);
    if (mac && env->GetArrayLength(mac) >= 6) env->GetByteArrayRegion(mac, 0, 6, (jbyte*)c.mac);
    if (dns && env->GetArrayLength(dns) >= 4) env->GetByteArrayRegion(dns, 0, 4, (jbyte*)c.dns);
    EmuCore::Get().SetConfig(c);
}

JNI(jboolean, loadRom)(JNIEnv* env, jclass, jint fd, jstring name)
{
    return EmuCore::Get().LoadROM(fd, JStr(env, name));
}

JNI(void, start)(JNIEnv*, jclass) { EmuCore::Get().Start(); }
JNI(void, pause)(JNIEnv*, jclass, jboolean p) { EmuCore::Get().Pause(p); }
JNI(void, stop)(JNIEnv*, jclass) { EmuCore::Get().Stop(); }
JNI(void, reset)(JNIEnv*, jclass) { EmuCore::Get().Reset(); }
JNI(jboolean, isRunning)(JNIEnv*, jclass) { return EmuCore::Get().IsRunning(); }

JNI(jboolean, saveState)(JNIEnv* env, jclass, jstring path) { return EmuCore::Get().SaveState(JStr(env, path)); }
JNI(jboolean, loadState)(JNIEnv* env, jclass, jstring path) { return EmuCore::Get().LoadState(JStr(env, path)); }

JNI(void, setKeys)(JNIEnv*, jclass, jint mask) { EmuCore::Get().SetKeys((u32)mask); }
JNI(void, touch)(JNIEnv*, jclass, jint x, jint y, jboolean down) { EmuCore::Get().Touch(x, y, down); }
JNI(void, setFastForward)(JNIEnv*, jclass, jboolean ff) { EmuCore::Get().SetFastForward(ff); }
JNI(void, setMicBlow)(JNIEnv*, jclass, jboolean on) { EmuCore::Get().SetMicBlow(on); }
JNI(void, setLidClosed)(JNIEnv*, jclass, jboolean c) { EmuCore::Get().SetLidClosed(c); }
JNI(jfloat, getFps)(JNIEnv*, jclass) { return EmuCore::Get().GetFPS(); }
JNI(jfloat, getEmuMs)(JNIEnv*, jclass) { return EmuCore::Get().GetEmuMs(); }
JNI(jfloat, getPresentMs)(JNIEnv*, jclass) { return EmuCore::Get().GetPresentMs(); }

// video: Surface from a SurfaceView (null when destroyed). Blocks until the
// emulation thread has switched, as required by SurfaceHolder.Callback.
JNI(void, setSurface)(JNIEnv* env, jclass, jobject surface)
{
    ANativeWindow* win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    EmuCore::Get().SetWindow(win);
}

JNI(void, setLayout)(JNIEnv* env, jclass, jintArray rects, jint filter)
{
    jint r[8] = {0};
    env->GetIntArrayRegion(rects, 0, 8, r);
    int t[4] = {r[0], r[1], r[2], r[3]};
    int b[4] = {r[4], r[5], r[6], r[7]};
    EmuCore::Get().SetLayout(t, b, filter);
}

JNI(jint, activeRenderer)(JNIEnv*, jclass) { return EmuCore::Get().ActiveRenderer(); }

// multiplayer
JNI(jboolean, mpHost)(JNIEnv* env, jclass, jstring name, jint maxPlayers)
{
    return EmuCore::Get().MPHost(JStr(env, name), maxPlayers);
}

JNI(jboolean, mpJoin)(JNIEnv* env, jclass, jstring name, jstring host)
{
    return EmuCore::Get().MPJoin(JStr(env, name), JStr(env, host));
}

JNI(void, mpStartDiscovery)(JNIEnv*, jclass) { EmuCore::Get().MPStartDiscovery(); }
JNI(void, mpEndDiscovery)(JNIEnv*, jclass) { EmuCore::Get().MPEndDiscovery(); }
JNI(void, mpEnd)(JNIEnv*, jclass) { EmuCore::Get().MPEnd(); }
JNI(jint, mpState)(JNIEnv*, jclass) { return EmuCore::Get().MPState(); }

JNI(jstring, mpDiscovery)(JNIEnv* env, jclass)
{
    return env->NewStringUTF(EmuCore::Get().MPDiscoveryJson().c_str());
}

JNI(jstring, mpPlayers)(JNIEnv* env, jclass)
{
    return env->NewStringUTF(EmuCore::Get().MPPlayersJson().c_str());
}
