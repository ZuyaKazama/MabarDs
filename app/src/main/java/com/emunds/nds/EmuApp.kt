package com.emunds.nds

import android.app.Application

class EmuApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Diagnostics.install(this)
        val dir = getExternalFilesDir(null) ?: filesDir
        NativeLib.init(dir.absolutePath)
        Prefs.applyToCore(this)
    }
}
