package com.emunds.nds

import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Process
import androidx.appcompat.app.AlertDialog
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Crash / disconnect diagnostics that work without adb:
 *  - Java crashes: stack trace saved by an uncaught-exception handler
 *  - native crashes / ANRs: Android's exit record of the previous process (Android 11+)
 *  - logcat of this app (the app may always read its own log), incl. the native backtrace
 *    that the system prints under the "DEBUG" tag after a native crash
 */
object Diagnostics {
    private const val PREF_LAST_EXIT = "diag_last_exit_ts"
    private const val MAX_CHARS = 200_000 // keeps the share intent far below the binder limit

    private fun javaCrashFile(ctx: Context) = File(ctx.filesDir, "last-java-crash.txt")

    fun install(ctx: Context) {
        val app = ctx.applicationContext
        val prev = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            runCatching {
                javaCrashFile(app).writeText("thread ${t.name}\n" + android.util.Log.getStackTraceString(e))
            }
            prev?.uncaughtException(t, e)
        }
    }

    private fun header(ctx: Context): String {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        @Suppress("DEPRECATION")
        val ver = "${pi.versionName} (${pi.versionCode})"
        return "MabarDs $ver\n" +
            "Perangkat: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.HARDWARE}), Android ${Build.VERSION.RELEASE} / API ${Build.VERSION.SDK_INT}\n" +
            "ABI: ${Build.SUPPORTED_ABIS.joinToString()}\n" +
            "Waktu: ${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())}\n"
    }

    /** Logcat lines of this app (current and earlier processes still in the buffer). */
    private fun appLog(maxLines: Int): String = runCatching {
        val p = ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", maxLines.toString())
            .redirectErrorStream(true).start()
        val text = p.inputStream.bufferedReader().readText()
        p.waitFor()
        text
    }.getOrElse { "logcat tidak bisa dibaca: $it" }

    private fun reasonName(r: Int) = when (r) {
        ApplicationExitInfo.REASON_CRASH -> "crash (Java)"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "crash (native)"
        ApplicationExitInfo.REASON_ANR -> "tidak merespons (ANR)"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "dihentikan karena memori habis"
        ApplicationExitInfo.REASON_SIGNALED -> "dihentikan sistem (signal)"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "dihentikan: pemakaian sumber daya berlebih"
        ApplicationExitInfo.REASON_FREEZER -> "dibekukan sistem"
        else -> "alasan $r"
    }

    private fun isAbnormal(r: Int) = r == ApplicationExitInfo.REASON_CRASH ||
        r == ApplicationExitInfo.REASON_CRASH_NATIVE || r == ApplicationExitInfo.REASON_ANR ||
        r == ApplicationExitInfo.REASON_LOW_MEMORY || r == ApplicationExitInfo.REASON_SIGNALED ||
        r == ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE

    /** Report text for the previous abnormal exit, or null if the last run ended normally. */
    fun pendingCrashReport(ctx: Context): String? {
        val sp = Prefs.sp(ctx)
        val sb = StringBuilder()
        var found = false

        val jf = javaCrashFile(ctx)
        if (jf.exists()) {
            sb.append("=== Crash Java ===\n").append(jf.readText()).append('\n')
            jf.delete()
            found = true
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val am = ctx.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val last = sp.getLong(PREF_LAST_EXIT, 0L)
            val exits = runCatching { am.getHistoricalProcessExitReasons(null, 0, 5) }.getOrDefault(emptyList())
            val newest = exits.maxOfOrNull { it.timestamp } ?: 0L
            for (e in exits.filter { it.timestamp > last && it.pid != Process.myPid() }) {
                if (!isAbnormal(e.reason)) continue
                found = true
                sb.append("=== Proses sebelumnya berhenti: ${reasonName(e.reason)} ===\n")
                sb.append("waktu ${Date(e.timestamp)}, pid ${e.pid}, status ${e.status}, ")
                sb.append("memori ${e.pss / 1024} MB\n")
                e.description?.let { sb.append("keterangan: ").append(it).append('\n') }
                if (e.reason == ApplicationExitInfo.REASON_ANR) {
                    runCatching {
                        e.traceInputStream?.bufferedReader()?.useLines { lines ->
                            sb.append("--- trace ANR ---\n")
                            lines.take(300).forEach { sb.append(it).append('\n') }
                        }
                    }
                }
            }
            if (newest > last) sp.edit().putLong(PREF_LAST_EXIT, newest).apply()
        }
        if (!found) return null
        return header(ctx) + "\n" + sb + "\n=== Log aplikasi ===\n" + appLog(4000)
    }

    /** Full diagnostic log on demand (disconnects, glitches...). */
    fun currentReport(ctx: Context): String = header(ctx) +
        "Renderer aktif: ${if (NativeLib.activeRenderer() == 1) "OpenGL ES" else "Software"}, " +
        "multiplayer: ${NativeLib.mpState()}\n\n=== Log aplikasi ===\n" + appLog(4000)

    fun showReport(ctx: Context, title: String, report: String) {
        val text = if (report.length > MAX_CHARS) report.takeLast(MAX_CHARS) else report
        AlertDialog.Builder(ctx)
            .setTitle(title)
            .setMessage(R.string.diag_message)
            .setPositiveButton(R.string.diag_share) { _, _ ->
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_SUBJECT, "MabarDs log")
                    putExtra(Intent.EXTRA_TEXT, text)
                }
                ctx.startActivity(Intent.createChooser(send, ctx.getString(R.string.diag_share)))
            }
            .setNeutralButton(R.string.diag_copy) { _, _ ->
                val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                cm.setPrimaryClip(ClipData.newPlainText("MabarDs log", text))
                android.widget.Toast.makeText(ctx, R.string.diag_copied, android.widget.Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }
}
