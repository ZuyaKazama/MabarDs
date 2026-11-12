package com.emunds.nds

import android.annotation.SuppressLint
import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.DateFormat
import java.util.Date

class EmulatorActivity : AppCompatActivity(), ControlsView.Listener {
    companion object {
        const val EXTRA_NAME = "rom_name"
        private const val SLOTS = 5
    }

    private lateinit var screen: SurfaceView
    private lateinit var controls: ControlsView
    private lateinit var fpsText: TextView

    private var romName = ""
    private var loaded = false
    private var menuOpen = false
    private var ffToggled = false
    private var ffHeld = false
    private var ffTrigger = false

    private var touchKeys = 0
    private var padKeys = 0
    private var stickKeys = 0
    private var hatKeys = 0

    private var layoutMode = 0
    private var swap = false
    private var surfaceW = 0
    private var surfaceH = 0

    private val ui = Handler(Looper.getMainLooper())
    private val fpsTick = object : Runnable {
        override fun run() {
            val f = NativeLib.getFps()
            val ff = if (ffToggled || ffHeld) " »" else ""
            fpsText.text = when {
                f <= 0 -> ""
                // emulation + GPU command time vs. presentation, to tell CPU and GPU limits apart
                Prefs.showFrameTime(this@EmulatorActivity) ->
                    "%.0f FPS%s\nemu %.1f ms · tampil %.1f ms".format(f, ff, NativeLib.getEmuMs(), NativeLib.getPresentMs())
                else -> "%.0f FPS%s".format(f, ff)
            }
            ui.postDelayed(this, 500)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        layoutMode = Prefs.layout(this)
        swap = Prefs.swapScreens(this)

        val root = FrameLayout(this)
        // Rendering happens on the native emulation thread (EGL), straight into this surface.
        screen = SurfaceView(this).apply {
            holder.addCallback(object : SurfaceHolder.Callback {
                override fun surfaceCreated(h: SurfaceHolder) = NativeLib.setSurface(h.surface)
                override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hgt: Int) {
                    surfaceW = w; surfaceH = hgt
                    applyLayout() // also triggers a redraw on the emulation thread
                }
                override fun surfaceDestroyed(h: SurfaceHolder) = NativeLib.setSurface(null)
            })
        }
        controls = ControlsView(this).apply {
            listener = this@EmulatorActivity
            opacity = Prefs.controlsOpacity(this@EmulatorActivity) / 100f
            scale = Prefs.controlsScale(this@EmulatorActivity) / 100f
            haptics = Prefs.haptics(this@EmulatorActivity)
        }
        fpsText = TextView(this).apply {
            setTextColor(0xFFFFFFFF.toInt())
            setShadowLayer(4f, 0f, 0f, 0xFF000000.toInt())
            textSize = 12f
            setPadding(16, 8, 16, 8)
        }
        root.addView(screen, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(controls, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(fpsText, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
        setContentView(root)

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = showMenu()
        })

        loadFromIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        loadFromIntent(intent)
    }

    private fun loadFromIntent(intent: Intent) {
        val uri = intent.data ?: run { finish(); return }
        romName = intent.getStringExtra(EXTRA_NAME) ?: queryName(uri) ?: "game.nds"
        loaded = false
        Toast.makeText(this, R.string.loading_rom, Toast.LENGTH_SHORT).show()
        Prefs.applyToCore(this)

        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) { openAndLoad(uri, romName) }
            if (!ok) {
                Toast.makeText(this@EmulatorActivity, R.string.rom_load_failed, Toast.LENGTH_LONG).show()
                finish()
                return@launch
            }
            loaded = true
            NativeLib.start()
            if (Prefs.renderer(this@EmulatorActivity) == 1 && NativeLib.activeRenderer() == 0) {
                Toast.makeText(this@EmulatorActivity, R.string.gl_unavailable, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun openAndLoad(uri: Uri, name: String): Boolean {
        return try {
            contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                if (pfd.statSize > 0) return NativeLib.loadRom(pfd.fd, name)
                // non-seekable provider (pipe): copy to cache first
                val tmp = File(cacheDir, "rom.tmp")
                contentResolver.openInputStream(uri)?.use { inp -> tmp.outputStream().use { inp.copyTo(it, 1 shl 20) } }
                android.os.ParcelFileDescriptor.open(tmp, android.os.ParcelFileDescriptor.MODE_READ_ONLY).use {
                    NativeLib.loadRom(it.fd, name)
                }.also { tmp.delete() }
            } ?: false
        } catch (e: Exception) {
            false
        }
    }

    private fun queryName(uri: Uri): String? = try {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        }
    } catch (e: Exception) { uri.lastPathSegment }

    private fun applyLayout() {
        if (surfaceW == 0) return
        val r = ScreenLayout.compute(surfaceW, surfaceH, layoutMode, swap)
        NativeLib.setLayout(r.toArray(), Prefs.filter(this))
        controls.bottomScreen = r.bottom
        controls.relayout()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        hideSystemBars()
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
        if (loaded && !menuOpen) NativeLib.pause(false)
        fpsText.visibility = if (Prefs.showFps(this)) android.view.View.VISIBLE else android.view.View.GONE
        ui.post(fpsTick)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(fpsTick)
        controls.releaseAll()
        // In multiplayer the other consoles stall (and finally drop us) while we are paused,
        // so keep running when only focus is lost (dialogs, split screen); pause in onStop.
        if (loaded && !inMultiplayer()) NativeLib.pause(true)
    }

    override fun onStop() {
        super.onStop()
        if (loaded) NativeLib.pause(true)
    }

    private fun inMultiplayer() = NativeLib.mpState() != 0

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) NativeLib.stop()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    // ------------------------------------------------------------------ input plumbing

    private fun pushKeys() {
        val pressed = touchKeys or padKeys or stickKeys or hatKeys
        NativeLib.setKeys(0xFFF and pressed.inv())
    }

    private fun pushFastForward() {
        val want = ffToggled || ffHeld || ffTrigger
        if (want && NativeLib.mpState() != 0) {
            Toast.makeText(this, R.string.mp_ff_disabled, Toast.LENGTH_SHORT).show()
        }
        NativeLib.setFastForward(want)
    }

    override fun onKeysChanged(mask: Int) { touchKeys = mask; pushKeys() }
    override fun onTouchScreen(x: Int, y: Int, down: Boolean) = NativeLib.touch(x, y, down)
    override fun onMenu() = showMenu()
    override fun onFastForward(pressed: Boolean) { ffHeld = pressed; pushFastForward() }
    override fun onMic(pressed: Boolean) = NativeLib.setMicBlow(pressed)

    private fun mapKey(code: Int): Int = when (code) {
        KeyEvent.KEYCODE_BUTTON_A -> NativeLib.KEY_A
        KeyEvent.KEYCODE_BUTTON_B -> NativeLib.KEY_B
        KeyEvent.KEYCODE_BUTTON_X -> NativeLib.KEY_X
        KeyEvent.KEYCODE_BUTTON_Y -> NativeLib.KEY_Y
        KeyEvent.KEYCODE_BUTTON_L1 -> NativeLib.KEY_L
        KeyEvent.KEYCODE_BUTTON_R1 -> NativeLib.KEY_R
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_ENTER -> NativeLib.KEY_START
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_SPACE -> NativeLib.KEY_SELECT
        KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_W -> NativeLib.KEY_UP
        KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_S -> NativeLib.KEY_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_A -> NativeLib.KEY_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_D -> NativeLib.KEY_RIGHT
        // keyboard: Z/X = B/A, C/V = Y/X, Q/E = L/R
        KeyEvent.KEYCODE_X -> NativeLib.KEY_A
        KeyEvent.KEYCODE_Z -> NativeLib.KEY_B
        KeyEvent.KEYCODE_V -> NativeLib.KEY_X
        KeyEvent.KEYCODE_C -> NativeLib.KEY_Y
        KeyEvent.KEYCODE_Q -> NativeLib.KEY_L
        KeyEvent.KEYCODE_E -> NativeLib.KEY_R
        else -> 0
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val code = event.keyCode
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (event.action == KeyEvent.ACTION_UP) showMenu()
            return true
        }
        if (code == KeyEvent.KEYCODE_BUTTON_R2 || code == KeyEvent.KEYCODE_TAB) {
            ffHeld = event.action == KeyEvent.ACTION_DOWN
            pushFastForward()
            return true
        }
        if (code == KeyEvent.KEYCODE_BUTTON_L2) {
            NativeLib.setMicBlow(event.action == KeyEvent.ACTION_DOWN)
            return true
        }
        val k = mapKey(code)
        if (k != 0 && !menuOpen) {
            padKeys = if (event.action == KeyEvent.ACTION_DOWN) padKeys or k else padKeys and k.inv()
            pushKeys()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if (ev.isFromSource(InputDevice.SOURCE_JOYSTICK) && ev.action == MotionEvent.ACTION_MOVE) {
            fun dir(v: Float, neg: Int, pos: Int) = if (v < -0.5f) neg else if (v > 0.5f) pos else 0
            val x = ev.getAxisValue(MotionEvent.AXIS_X)
            val y = ev.getAxisValue(MotionEvent.AXIS_Y)
            stickKeys = dir(x, NativeLib.KEY_LEFT, NativeLib.KEY_RIGHT) or dir(y, NativeLib.KEY_UP, NativeLib.KEY_DOWN)
            val hx = ev.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y)
            hatKeys = dir(hx, NativeLib.KEY_LEFT, NativeLib.KEY_RIGHT) or dir(hy, NativeLib.KEY_UP, NativeLib.KEY_DOWN)
            val rt = maxOf(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS))
            val trig = rt > 0.5f
            if (trig != ffTrigger) { ffTrigger = trig; pushFastForward() }
            pushKeys()
            return true
        }
        return super.dispatchGenericMotionEvent(ev)
    }

    // ------------------------------------------------------------------ menu

    private fun statePath(slot: Int): String {
        val dir = File(getExternalFilesDir(null) ?: filesDir, "states")
        dir.mkdirs()
        return File(dir, "$romName.ss$slot").absolutePath
    }

    private fun showMenu() {
        if (menuOpen) return
        menuOpen = true
        controls.releaseAll()
        val mp = inMultiplayer()
        if (loaded && !mp) NativeLib.pause(true)
        if (mp) Toast.makeText(this, R.string.mp_menu_running, Toast.LENGTH_SHORT).show()
        val items = arrayOf(
            getString(R.string.menu_resume),
            getString(R.string.menu_save_state),
            getString(R.string.menu_load_state),
            getString(R.string.menu_fast_forward) + if (ffToggled) " ✓" else "",
            getString(R.string.menu_swap),
            getString(R.string.menu_layout),
            getString(R.string.menu_resolution) + " (${if (NativeLib.activeRenderer() == 1) Prefs.scale(this) else 1}×)",
            getString(R.string.menu_toggle_controls),
            getString(R.string.menu_reset),
            getString(R.string.menu_quit),
        )
        AlertDialog.Builder(this)
            .setTitle(romName)
            .setItems(items) { _, which ->
                // savestates, reset and renderer changes freeze or desync a wireless session
                if (mp && which in setOf(1, 2, 6, 8)) {
                    Toast.makeText(this, R.string.mp_not_available, Toast.LENGTH_LONG).show()
                    closeMenu()
                    return@setItems
                }
                when (which) {
                    0 -> closeMenu()
                    1 -> pickSlot(true)
                    2 -> pickSlot(false)
                    3 -> { ffToggled = !ffToggled; pushFastForward(); closeMenu() }
                    4 -> {
                        swap = !swap
                        Prefs.sp(this).edit().putBoolean("swap", swap).apply()
                        applyLayout(); closeMenu()
                    }
                    5 -> pickLayout()
                    6 -> pickResolution()
                    7 -> { controls.controlsVisible = !controls.controlsVisible; closeMenu() }
                    8 -> { NativeLib.reset(); closeMenu() }
                    9 -> { menuOpen = false; finish() }
                }
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun closeMenu() {
        menuOpen = false
        if (loaded) NativeLib.pause(false)
        hideSystemBars()
    }

    private fun pickSlot(save: Boolean) {
        val fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
        val labels = Array(SLOTS) { i ->
            val f = File(statePath(i + 1))
            if (f.exists()) getString(R.string.slot, i + 1) + " — " + fmt.format(Date(f.lastModified()))
            else getString(R.string.slot_empty, i + 1)
        }
        AlertDialog.Builder(this)
            .setTitle(if (save) R.string.menu_save_state else R.string.menu_load_state)
            .setItems(labels) { _, i ->
                val slot = i + 1
                lifecycleScope.launch {
                    val ok = withContext(Dispatchers.IO) {
                        if (save) NativeLib.saveState(statePath(slot)) else NativeLib.loadState(statePath(slot))
                    }
                    val msg = when {
                        !ok -> getString(R.string.state_failed)
                        save -> getString(R.string.state_saved, slot)
                        else -> getString(R.string.state_loaded, slot)
                    }
                    Toast.makeText(this@EmulatorActivity, msg, Toast.LENGTH_SHORT).show()
                    closeMenu()
                }
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun pickResolution() {
        val current = if (NativeLib.activeRenderer() == 1) Prefs.scale(this) else 1
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_resolution)
            .setSingleChoiceItems(R.array.scale_names, current - 1) { d, which ->
                val scale = which + 1
                // >1x needs the OpenGL renderer; 1x keeps whatever renderer the user chose
                val ed = Prefs.sp(this).edit().putString("scale", scale.toString())
                if (scale > 1) ed.putString("renderer", "1")
                ed.apply()
                Prefs.applyToCore(this)
                d.dismiss()
                if (scale > 1 && NativeLib.activeRenderer() == 0) {
                    Toast.makeText(this, R.string.gl_unavailable, Toast.LENGTH_LONG).show()
                } else {
                    Toast.makeText(this, getString(R.string.resolution_set, scale, 256 * scale, 192 * scale), Toast.LENGTH_SHORT).show()
                }
                closeMenu()
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }

    private fun pickLayout() {
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_layout)
            .setSingleChoiceItems(R.array.layouts, layoutMode) { d, which ->
                layoutMode = which
                Prefs.sp(this).edit().putString("layout", which.toString()).apply()
                applyLayout()
                d.dismiss()
                closeMenu()
            }
            .setOnCancelListener { closeMenu() }
            .show()
    }
}
