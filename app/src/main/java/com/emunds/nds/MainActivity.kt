package com.emunds.nds

import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.emunds.nds.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {
    companion object {
        /** When true, the user is picking the game for a multiplayer session. */
        const val EXTRA_PICK_FOR_MP = "pick_mp"
    }

    private lateinit var b: ActivityMainBinding
    private val adapter = RomAdapter { launch(it) }

    private val pickFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            Prefs.sp(this).edit().putString("rom_folder", uri.toString()).apply()
            rescan()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMainBinding.inflate(layoutInflater)
        setContentView(b.root)

        b.toolbar.inflateMenu(R.menu.main)
        b.toolbar.setOnMenuItemClickListener {
            when (it.itemId) {
                R.id.action_mp -> startActivity(Intent(this, MultiplayerActivity::class.java))
                R.id.action_folder -> pickFolder.launch(null)
                R.id.action_settings -> startActivity(Intent(this, SettingsActivity::class.java))
            }
            true
        }
        if (intent.getBooleanExtra(EXTRA_PICK_FOR_MP, false)) {
            b.toolbar.title = getString(R.string.pick_game_for_mp)
            b.toolbar.menu.findItem(R.id.action_mp)?.isVisible = false
        }

        // previous run crashed / was killed: offer the report (works without adb)
        lifecycleScope.launch {
            val report = withContext(Dispatchers.IO) { Diagnostics.pendingCrashReport(this@MainActivity) }
            if (report != null) Diagnostics.showReport(this@MainActivity, getString(R.string.diag_crash_title), report)
        }

        val span = (resources.configuration.screenWidthDp / 360).coerceAtLeast(1)
        b.list.layoutManager = GridLayoutManager(this, span)
        b.list.adapter = adapter
        b.pickFolder.setOnClickListener { pickFolder.launch(null) }
        b.toolbar.setOnClickListener { rescan() }

        val cached = RomLibrary.loadCached(this)
        show(cached)
        if (Prefs.romFolder(this) != null) rescan()
    }

    override fun onResume() {
        super.onResume()
        val st = NativeLib.mpState()
        if (st != 0) {
            val n = MultiplayerActivity.parsePlayers(NativeLib.mpPlayers()).size
            b.mpBanner.text = getString(R.string.mp_session_active, n)
            b.mpBanner.visibility = View.VISIBLE
            b.mpBanner.setOnClickListener { startActivity(Intent(this, MultiplayerActivity::class.java)) }
        } else {
            b.mpBanner.visibility = View.GONE
        }
    }

    private fun show(list: List<RomEntry>) {
        adapter.submit(list)
        b.empty.visibility = if (list.isEmpty()) View.VISIBLE else View.GONE
        b.toolbar.subtitle = if (list.isEmpty()) null else getString(R.string.rom_count, list.size)
    }

    private fun rescan() {
        val folder = Prefs.romFolder(this) ?: return
        b.progress.visibility = View.VISIBLE
        lifecycleScope.launch {
            val list = withContext(Dispatchers.IO) {
                try { RomLibrary.scan(this@MainActivity, Uri.parse(folder)) } catch (e: Exception) { emptyList() }
            }
            b.progress.visibility = View.GONE
            show(list)
        }
    }

    private fun launch(rom: RomEntry) {
        startActivity(Intent(this, EmulatorActivity::class.java).apply {
            data = Uri.parse(rom.uri)
            putExtra(EmulatorActivity.EXTRA_NAME, rom.fileName)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        })
        if (intent.getBooleanExtra(EXTRA_PICK_FOR_MP, false)) finish()
    }
}

private class RomAdapter(val onClick: (RomEntry) -> Unit) : RecyclerView.Adapter<RomAdapter.VH>() {
    private var items: List<RomEntry> = emptyList()

    fun submit(list: List<RomEntry>) {
        items = list
        notifyDataSetChanged()
    }

    class VH(v: View) : RecyclerView.ViewHolder(v) {
        val icon: ImageView = v.findViewById(R.id.icon)
        val title: TextView = v.findViewById(R.id.title)
        val subtitle: TextView = v.findViewById(R.id.subtitle)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        VH(LayoutInflater.from(parent.context).inflate(R.layout.item_rom, parent, false))

    override fun getItemCount() = items.size

    override fun onBindViewHolder(h: VH, pos: Int) {
        val r = items[pos]
        h.title.text = r.title
        h.subtitle.text = if (r.gameCode.isNotEmpty()) "${r.gameCode} · ${r.fileName}" else r.fileName
        val bmp = r.iconPath?.let { BitmapFactory.decodeFile(it) }
        if (bmp != null) {
            h.icon.setImageBitmap(bmp)
            h.icon.scaleType = ImageView.ScaleType.FIT_CENTER
            // keep pixel art crisp
            h.icon.drawable?.isFilterBitmap = false
        } else {
            h.icon.setImageDrawable(null)
        }
        h.itemView.setOnClickListener { onClick(r) }
    }
}
