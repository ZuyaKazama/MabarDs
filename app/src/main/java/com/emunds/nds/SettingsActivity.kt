package com.emunds.nds

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.preference.PreferenceFragmentCompat
import com.google.android.material.appbar.MaterialToolbar

class SettingsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction().replace(R.id.content, Fragment()).commit()
        }
    }

    override fun onPause() {
        super.onPause()
        Prefs.applyToCore(this)
    }

    class Fragment : PreferenceFragmentCompat() {
        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.preferences, rootKey)
            findPreference<androidx.preference.Preference>("mac_info")?.summary =
                Prefs.macAddress(requireContext()).joinToString(":") { "%02X".format(it) }
            findPreference<androidx.preference.Preference>("share_log")?.setOnPreferenceClickListener {
                val act = requireActivity()
                Thread {
                    val report = Diagnostics.currentReport(act)
                    act.runOnUiThread {
                        if (!act.isFinishing) Diagnostics.showReport(act, act.getString(R.string.diag_log_title), report)
                    }
                }.start()
                true
            }
        }
    }
}
