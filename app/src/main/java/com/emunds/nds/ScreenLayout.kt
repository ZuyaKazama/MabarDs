package com.emunds.nds

import android.graphics.Rect
import kotlin.math.min

/** Computes where the two 256x192 DS screens are drawn on the surface. */
object ScreenLayout {
    const val AUTO = 0
    const val VERTICAL = 1
    const val HORIZONTAL = 2
    const val HYBRID = 3
    const val TOP_ONLY = 4
    const val BOTTOM_ONLY = 5

    data class Result(val top: Rect, val bottom: Rect) {
        fun toArray() = intArrayOf(
            top.left, top.top, top.width(), top.height(),
            bottom.left, bottom.top, bottom.width(), bottom.height(),
        )
    }

    fun compute(w: Int, h: Int, mode: Int, swap: Boolean): Result {
        val portrait = h >= w
        val m = if (mode == AUTO) (if (portrait) VERTICAL else HORIZONTAL) else mode
        var a = Rect()   // "first" screen (top unless swapped)
        var b = Rect()
        when (m) {
            VERTICAL -> {
                val s = min(w / 256f, h / 384f)
                val sw = (256 * s).toInt(); val sh = (192 * s).toInt()
                val x = (w - sw) / 2
                // portrait: pin to the top so controls get the remaining space
                val y = if (portrait) 0 else (h - sh * 2) / 2
                a = Rect(x, y, x + sw, y + sh)
                b = Rect(x, y + sh, x + sw, y + sh * 2)
            }
            HORIZONTAL -> {
                val s = min(w / 512f, h / 192f)
                val sw = (256 * s).toInt(); val sh = (192 * s).toInt()
                val x = (w - sw * 2) / 2
                val y = if (portrait) 0 else (h - sh) / 2
                a = Rect(x, y, x + sw, y + sh)
                b = Rect(x + sw, y, x + sw * 2, y + sh)
            }
            HYBRID -> {
                // big screen + small one at a third of the size
                val s = min(w / (256f * 4f / 3f), h / 192f)
                val bw = (256 * s).toInt(); val bh = (192 * s).toInt()
                val smw = bw / 3; val smh = bh / 3
                val x = (w - bw - smw) / 2
                val y = (h - bh) / 2
                a = Rect(x, y, x + bw, y + bh)
                b = Rect(x + bw, y, x + bw + smw, y + smh)
            }
            TOP_ONLY, BOTTOM_ONLY -> {
                val s = min(w / 256f, h / 192f)
                val sw = (256 * s).toInt(); val sh = (192 * s).toInt()
                val x = (w - sw) / 2
                val y = if (portrait) 0 else (h - sh) / 2
                val full = Rect(x, y, x + sw, y + sh)
                return if ((m == TOP_ONLY) != swap) Result(full, Rect()) else Result(Rect(), full)
            }
        }
        return if (swap) Result(b, a) else Result(a, b)
    }
}
