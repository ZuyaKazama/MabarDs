package com.emunds.nds

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build

/**
 * Keeps Wi-Fi out of power-save while a multiplayer session is active.
 *
 * melonDS LAN multiplayer stalls the emulation until the other consoles' replies arrive,
 * several times per frame. Wi-Fi power save delays packets by tens of milliseconds, which
 * drops the frame rate on every device in the session (the PC too).
 */
object NetLock {
    private var lock: WifiManager.WifiLock? = null

    @Synchronized
    fun acquire(ctx: Context) {
        if (lock?.isHeld == true) return
        val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        lock = wifi.createWifiLock(mode, "emunds-mp").apply {
            setReferenceCounted(false)
            acquire()
        }
    }

    @Synchronized
    fun release() {
        lock?.let { if (it.isHeld) it.release() }
        lock = null
    }
}
