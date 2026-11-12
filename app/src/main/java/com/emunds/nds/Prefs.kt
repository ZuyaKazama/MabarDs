package com.emunds.nds

import android.content.Context
import android.content.SharedPreferences
import androidx.preference.PreferenceManager
import java.security.SecureRandom

object Prefs {
    fun sp(ctx: Context): SharedPreferences = PreferenceManager.getDefaultSharedPreferences(ctx)

    /** Each device needs its own MAC, otherwise DS games refuse to see each other. */
    fun macAddress(ctx: Context): ByteArray {
        val p = sp(ctx)
        val stored = p.getString("mac", null)
        val hex: String = if (stored != null && stored.length == 12) stored else {
            val r = ByteArray(3).also { SecureRandom().nextBytes(it) }
            // Nintendo OUI 00:09:BF + random NIC part
            ("0009BF" + r.joinToString("") { "%02X".format(it) }).also {
                p.edit().putString("mac", it).apply()
            }
        }
        return ByteArray(6) { i -> hex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
    }

    private fun dns(p: SharedPreferences): ByteArray {
        val s = p.getString("dns", "")!!.trim()
        val parts = s.split('.')
        if (parts.size != 4) return ByteArray(4)
        return try {
            ByteArray(4) { i -> parts[i].toInt().also { require(it in 0..255) }.toByte() }
        } catch (e: Exception) {
            ByteArray(4)
        }
    }

    fun nickname(ctx: Context): String =
        sp(ctx).getString("nickname", "")!!.ifBlank { "Player" }.take(10)

    fun intOf(p: SharedPreferences, key: String, def: Int): Int =
        p.getString(key, null)?.toIntOrNull() ?: def

    fun applyToCore(ctx: Context) {
        val p = sp(ctx)
        val ints = intArrayOf(
            if (p.getBoolean("jit", true)) 1 else 0,
            if (p.getBoolean("jit_fastmem", true)) 1 else 0,
            intOf(p, "jit_block", 32),
            if (p.getBoolean("threaded3d", true)) 1 else 0,
            intOf(p, "audio_interp", 0),
            if (p.getBoolean("audio", true)) 1 else 0,
            (p.getInt("volume", 100).coerceIn(0, 100) * 256) / 100,
            intOf(p, "ff_speed", 3),
            intOf(p, "language", 1),
            intOf(p, "fav_color", 0),
            intOf(p, "birth_month", 1),
            intOf(p, "birth_day", 1),
            intOf(p, "renderer", 0),
            intOf(p, "scale", 2),
            if (p.getBoolean("better_polygons", true)) 1 else 0,
            intOf(p, "frameskip", -1),
            intOf(p, "speed_hack", 0),
        )
        NativeLib.setConfig(ints, nickname(ctx), macAddress(ctx), dns(p))
    }

    fun layout(ctx: Context) = intOf(sp(ctx), "layout", 0)
    fun renderer(ctx: Context) = intOf(sp(ctx), "renderer", 0)
    fun scale(ctx: Context) = intOf(sp(ctx), "scale", 2)
    fun swapScreens(ctx: Context) = sp(ctx).getBoolean("swap", false)
    fun filter(ctx: Context) = if (sp(ctx).getBoolean("linear_filter", false)) 1 else 0
    fun showFps(ctx: Context) = sp(ctx).getBoolean("show_fps", true)
    fun showFrameTime(ctx: Context) = sp(ctx).getBoolean("show_frametime", false)
    fun controlsOpacity(ctx: Context) = sp(ctx).getInt("controls_opacity", 55)
    fun controlsScale(ctx: Context) = sp(ctx).getInt("controls_scale", 100)
    fun haptics(ctx: Context) = sp(ctx).getBoolean("haptics", true)
    fun romFolder(ctx: Context) = sp(ctx).getString("rom_folder", null)
}
