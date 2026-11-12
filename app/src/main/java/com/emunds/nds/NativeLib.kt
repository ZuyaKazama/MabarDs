package com.emunds.nds

/** JNI surface of libemunds.so (melonDS core + Android frontend). */
object NativeLib {
    init { System.loadLibrary("emunds") }

    @JvmStatic external fun init(baseDir: String)
    @JvmStatic external fun setConfig(ints: IntArray, nickname: String, mac: ByteArray, dns: ByteArray)

    @JvmStatic external fun loadRom(fd: Int, name: String): Boolean
    @JvmStatic external fun start()
    @JvmStatic external fun pause(paused: Boolean)
    @JvmStatic external fun stop()
    @JvmStatic external fun reset()
    @JvmStatic external fun isRunning(): Boolean

    @JvmStatic external fun saveState(path: String): Boolean
    @JvmStatic external fun loadState(path: String): Boolean

    @JvmStatic external fun setKeys(mask: Int)
    @JvmStatic external fun touch(x: Int, y: Int, down: Boolean)
    @JvmStatic external fun setFastForward(ff: Boolean)
    @JvmStatic external fun setMicBlow(on: Boolean)
    @JvmStatic external fun setLidClosed(closed: Boolean)
    @JvmStatic external fun getFps(): Float
    @JvmStatic external fun getEmuMs(): Float
    @JvmStatic external fun getPresentMs(): Float

    /** Hands the SurfaceView's Surface to the emulation thread (null = destroyed). Blocks until done. */
    @JvmStatic external fun setSurface(surface: android.view.Surface?)
    /** 0 = software, 1 = OpenGL ES (falls back to 0 if the GPU lacks GLES 3.2). */
    @JvmStatic external fun activeRenderer(): Int
    /** rects = top(x,y,w,h) + bottom(x,y,w,h) in surface pixels. */
    @JvmStatic external fun setLayout(rects: IntArray, filter: Int)

    @JvmStatic external fun mpHost(name: String, maxPlayers: Int): Boolean
    @JvmStatic external fun mpJoin(name: String, host: String): Boolean
    @JvmStatic external fun mpStartDiscovery()
    @JvmStatic external fun mpEndDiscovery()
    @JvmStatic external fun mpEnd()
    @JvmStatic external fun mpState(): Int
    @JvmStatic external fun mpDiscovery(): String
    @JvmStatic external fun mpPlayers(): String

    // DS key bits (active-low mask passed to the core)
    const val KEY_A = 1 shl 0
    const val KEY_B = 1 shl 1
    const val KEY_SELECT = 1 shl 2
    const val KEY_START = 1 shl 3
    const val KEY_RIGHT = 1 shl 4
    const val KEY_LEFT = 1 shl 5
    const val KEY_UP = 1 shl 6
    const val KEY_DOWN = 1 shl 7
    const val KEY_R = 1 shl 8
    const val KEY_L = 1 shl 9
    const val KEY_X = 1 shl 10
    const val KEY_Y = 1 shl 11
}
