package com.emunds.nds

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.min

/**
 * Multi-touch on-screen controller + DS touch screen input.
 * Every pointer is bound to exactly one target: a button, the d-pad, or the touch screen.
 */
class ControlsView(ctx: Context) : View(ctx) {

    interface Listener {
        fun onKeysChanged(mask: Int)
        fun onTouchScreen(x: Int, y: Int, down: Boolean)
        fun onMenu()
        fun onFastForward(pressed: Boolean)
        fun onMic(pressed: Boolean)
    }

    var listener: Listener? = null
    var bottomScreen = Rect()
    var controlsVisible = true
        set(v) { field = v; releaseAll(); invalidate() }
    var opacity = 0.55f
    var scale = 1f
    var haptics = true

    private enum class Kind { KEY, DPAD, MENU, FF, MIC }
    private class Btn(val kind: Kind, val key: Int, val label: String, val round: Boolean) {
        val r = RectF()
    }

    private val buttons = listOf(
        Btn(Kind.KEY, NativeLib.KEY_A, "A", true),
        Btn(Kind.KEY, NativeLib.KEY_B, "B", true),
        Btn(Kind.KEY, NativeLib.KEY_X, "X", true),
        Btn(Kind.KEY, NativeLib.KEY_Y, "Y", true),
        Btn(Kind.KEY, NativeLib.KEY_L, "L", false),
        Btn(Kind.KEY, NativeLib.KEY_R, "R", false),
        Btn(Kind.KEY, NativeLib.KEY_START, "START", false),
        Btn(Kind.KEY, NativeLib.KEY_SELECT, "SELECT", false),
        Btn(Kind.MENU, 0, "≡", false),
        Btn(Kind.FF, 0, "»", false),
        Btn(Kind.MIC, 0, "MIC", false),
    )
    private val dpad = RectF()

    // pointerId -> target
    private sealed class Target {
        data class Button(val b: Btn) : Target()
        data class DPad(var dirs: Int) : Target()
        object Screen : Target()
    }
    private val pointers = HashMap<Int, Target>()
    private var keyMask = 0
    private var ffDown = false

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 3f }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textAlign = Paint.Align.CENTER; isFakeBoldText = true }

    init {
        isHapticFeedbackEnabled = true
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) = relayout()

    fun relayout() {
        val w = width.toFloat(); val h = height.toFloat()
        if (w <= 0 || h <= 0) return
        val portrait = h >= w
        val u = min(w, h) * 0.085f * scale  // base unit
        fun place(b: Btn, cx: Float, cy: Float, bw: Float, bh: Float) =
            b.r.set(cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
        val byKey = { k: Kind, label: String -> buttons.first { it.kind == k && it.label == label } }

        // area for the main cluster: below the screens in portrait, full height in landscape
        val areaTop = if (portrait) bottomScreen.bottom.toFloat().coerceAtMost(h - u * 6.5f) else 0f
        val clusterY = if (portrait) (areaTop + h) / 2 + u * 0.3f else h - u * 3.4f
        val margin = u * 0.6f

        // d-pad
        val dpCx = margin + u * 2.1f
        dpad.set(dpCx - u * 2f, clusterY - u * 2f, dpCx + u * 2f, clusterY + u * 2f)

        // face buttons (diamond)
        val fCx = w - margin - u * 2.1f
        val d = u * 1.35f
        val fb = u * 1.25f
        place(byKey(Kind.KEY, "A"), fCx + d, clusterY, fb, fb)
        place(byKey(Kind.KEY, "B"), fCx, clusterY + d, fb, fb)
        place(byKey(Kind.KEY, "X"), fCx, clusterY - d, fb, fb)
        place(byKey(Kind.KEY, "Y"), fCx - d, clusterY, fb, fb)

        // shoulders
        val shY = if (portrait) areaTop + u * 0.9f else u * 0.9f
        place(byKey(Kind.KEY, "L"), margin + u * 1.3f, shY, u * 2.6f, u * 1.0f)
        place(byKey(Kind.KEY, "R"), w - margin - u * 1.3f, shY, u * 2.6f, u * 1.0f)

        // start / select
        val ssY = h - u * 0.8f
        place(byKey(Kind.KEY, "SELECT"), w / 2 - u * 1.5f, ssY, u * 2.4f, u * 0.8f)
        place(byKey(Kind.KEY, "START"), w / 2 + u * 1.5f, ssY, u * 2.4f, u * 0.8f)
        if (!portrait) {
            // screens occupy the middle in landscape: put start/select under the clusters
            place(byKey(Kind.KEY, "SELECT"), dpCx, h - u * 0.6f, u * 2.4f, u * 0.8f)
            place(byKey(Kind.KEY, "START"), fCx, h - u * 0.6f, u * 2.4f, u * 0.8f)
        }

        // utility row
        val utY = if (portrait) shY else u * 2.3f
        val utX = if (portrait) w / 2 else margin + u * 0.6f
        if (portrait) {
            place(byKey(Kind.MENU, "≡"), utX - u * 1.6f, utY, u * 1.2f, u * 0.9f)
            place(byKey(Kind.FF, "»"), utX, utY, u * 1.2f, u * 0.9f)
            place(byKey(Kind.MIC, "MIC"), utX + u * 1.6f, utY, u * 1.2f, u * 0.9f)
        } else {
            place(byKey(Kind.MENU, "≡"), utX, utY, u * 1.1f, u * 0.9f)
            place(byKey(Kind.FF, "»"), w - utX, utY, u * 1.1f, u * 0.9f)
            place(byKey(Kind.MIC, "MIC"), w - utX, utY + u * 1.2f, u * 1.1f, u * 0.9f)
        }
        text.textSize = u * 0.45f
        invalidate()
    }

    // ------------------------------------------------------------------ drawing

    override fun onDraw(c: Canvas) {
        if (!controlsVisible) {
            // keep a tiny menu button so the user can always get out
            drawBtn(c, buttons.first { it.kind == Kind.MENU }, false)
            return
        }
        val a = (opacity * 255).toInt()
        // d-pad
        val dirs = pointers.values.filterIsInstance<Target.DPad>().fold(0) { acc, t -> acc or t.dirs }
        val cx = dpad.centerX(); val cy = dpad.centerY(); val arm = dpad.width() / 6
        fill.color = Color.argb(a / 2, 40, 40, 48)
        c.drawRoundRect(cx - arm, dpad.top, cx + arm, dpad.bottom, arm / 2, arm / 2, fill)
        c.drawRoundRect(dpad.left, cy - arm, dpad.right, cy + arm, arm / 2, arm / 2, fill)
        fill.color = Color.argb(a, 229, 72, 77)
        if (dirs and NativeLib.KEY_UP != 0) c.drawRect(cx - arm, dpad.top, cx + arm, cy - arm, fill)
        if (dirs and NativeLib.KEY_DOWN != 0) c.drawRect(cx - arm, cy + arm, cx + arm, dpad.bottom, fill)
        if (dirs and NativeLib.KEY_LEFT != 0) c.drawRect(dpad.left, cy - arm, cx - arm, cy + arm, fill)
        if (dirs and NativeLib.KEY_RIGHT != 0) c.drawRect(cx + arm, cy - arm, dpad.right, cy + arm, fill)
        stroke.color = Color.argb(a, 255, 255, 255)
        c.drawRoundRect(cx - arm, dpad.top, cx + arm, dpad.bottom, arm / 2, arm / 2, stroke)
        c.drawRoundRect(dpad.left, cy - arm, dpad.right, cy + arm, arm / 2, arm / 2, stroke)

        for (b in buttons) {
            val pressed = when (b.kind) {
                Kind.KEY -> keyMask and b.key != 0
                else -> pointers.values.any { it is Target.Button && it.b === b }
            }
            drawBtn(c, b, pressed)
        }
    }

    private fun drawBtn(c: Canvas, b: Btn, pressed: Boolean) {
        val a = (opacity * 255).toInt()
        fill.color = if (pressed) Color.argb(a, 229, 72, 77) else Color.argb(a / 2, 40, 40, 48)
        stroke.color = Color.argb(a, 255, 255, 255)
        text.alpha = a
        if (b.round) {
            val rad = b.r.width() / 2
            c.drawCircle(b.r.centerX(), b.r.centerY(), rad, fill)
            c.drawCircle(b.r.centerX(), b.r.centerY(), rad, stroke)
        } else {
            val rad = b.r.height() / 2
            c.drawRoundRect(b.r, rad, rad, fill)
            c.drawRoundRect(b.r, rad, rad, stroke)
        }
        c.drawText(b.label, b.r.centerX(), b.r.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    // ------------------------------------------------------------------ input

    private fun hitButton(x: Float, y: Float): Btn? {
        for (b in buttons) {
            if (!controlsVisible && b.kind != Kind.MENU) continue
            // generous hit slop for round buttons
            val slop = if (b.round) b.r.width() * 0.15f else 0f
            if (x >= b.r.left - slop && x <= b.r.right + slop && y >= b.r.top - slop && y <= b.r.bottom + slop) return b
        }
        return null
    }

    private fun dpadDirs(x: Float, y: Float): Int {
        val dx = x - dpad.centerX(); val dy = y - dpad.centerY()
        if (hypot(dx, dy) < dpad.width() * 0.08f) return 0
        // 8-way with 22.5° diagonal zones
        val ang = Math.toDegrees(atan2(-dy.toDouble(), dx.toDouble())).let { if (it < 0) it + 360 else it }
        var m = 0
        if (ang < 67.5 || ang > 292.5) m = m or NativeLib.KEY_RIGHT
        if (ang in 22.5..157.5) m = m or NativeLib.KEY_UP
        if (ang in 112.5..247.5) m = m or NativeLib.KEY_LEFT
        if (ang in 202.5..337.5) m = m or NativeLib.KEY_DOWN
        return m
    }

    private fun inDpad(x: Float, y: Float): Boolean {
        val ex = dpad.width() * 0.25f
        return controlsVisible && x >= dpad.left - ex && x <= dpad.right + ex && y >= dpad.top - ex && y <= dpad.bottom + ex
    }

    private fun screenCoords(x: Float, y: Float): Pair<Int, Int> {
        val sx = ((x - bottomScreen.left) * 256f / bottomScreen.width()).toInt()
        val sy = ((y - bottomScreen.top) * 192f / bottomScreen.height()).toInt()
        return sx.coerceIn(0, 255) to sy.coerceIn(0, 191)
    }

    private fun inScreen(x: Float, y: Float) =
        !bottomScreen.isEmpty && x >= bottomScreen.left && x < bottomScreen.right && y >= bottomScreen.top && y < bottomScreen.bottom

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                bind(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until e.pointerCount) move(e.getPointerId(i), e.getX(i), e.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> release(e.getPointerId(e.actionIndex))
            MotionEvent.ACTION_CANCEL -> releaseAll()
        }
        recompute()
        return true
    }

    private fun bind(id: Int, x: Float, y: Float) {
        val b = hitButton(x, y)
        val t: Target? = when {
            b != null -> Target.Button(b)
            inDpad(x, y) -> Target.DPad(dpadDirs(x, y))
            inScreen(x, y) && pointers.values.none { it is Target.Screen } -> Target.Screen
            else -> null
        }
        t ?: return
        pointers[id] = t
        when (t) {
            is Target.Button -> {
                haptic()
                when (t.b.kind) {
                    Kind.MENU -> listener?.onMenu()
                    Kind.MIC -> listener?.onMic(true)
                    else -> {}
                }
            }
            is Target.DPad -> haptic()
            is Target.Screen -> {
                val (sx, sy) = screenCoords(x, y)
                listener?.onTouchScreen(sx, sy, true)
            }
        }
    }

    private fun move(id: Int, x: Float, y: Float) {
        when (val t = pointers[id] ?: return) {
            is Target.DPad -> {
                val nd = dpadDirs(x, y)
                if (nd != t.dirs) { t.dirs = nd; if (nd != 0) haptic() }
            }
            is Target.Screen -> {
                val (sx, sy) = screenCoords(x, y)
                listener?.onTouchScreen(sx, sy, true)
            }
            is Target.Button -> {
                // allow sliding between face buttons (A→B rolls etc.)
                if (t.b.kind == Kind.KEY) {
                    val nb = hitButton(x, y)
                    if (nb != null && nb !== t.b && nb.kind == Kind.KEY) {
                        pointers[id] = Target.Button(nb)
                        haptic()
                    }
                }
            }
        }
    }

    private fun release(id: Int) {
        val t = pointers.remove(id) ?: return
        if (t is Target.Screen) listener?.onTouchScreen(0, 0, false)
        if (t is Target.Button && t.b.kind == Kind.MIC) listener?.onMic(false)
    }

    fun releaseAll() {
        val had = pointers.isNotEmpty()
        for (id in pointers.keys.toList()) release(id)
        if (had) recompute()
    }

    private fun recompute() {
        var m = 0
        var ff = false
        for (t in pointers.values) {
            when (t) {
                is Target.Button -> when (t.b.kind) {
                    Kind.KEY -> m = m or t.b.key
                    Kind.FF -> ff = true
                    else -> {}
                }
                is Target.DPad -> m = m or t.dirs
                else -> {}
            }
        }
        if (m != keyMask) {
            keyMask = m
            listener?.onKeysChanged(m)
        }
        if (ff != ffDown) {
            ffDown = ff
            listener?.onFastForward(ff)
        }
        invalidate()
    }

    private fun haptic() {
        if (haptics) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY, HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING)
    }
}
