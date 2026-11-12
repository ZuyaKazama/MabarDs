package com.emunds.nds

import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.wifi.WifiManager
import android.os.Bundle
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.emunds.nds.databinding.ActivityMultiplayerBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.net.Inet4Address
import java.net.NetworkInterface

/**
 * Lobby for DS local wireless over IP (melonDS LAN protocol on UDP 7064, discovery on UDP 7063).
 * Offline: same Wi-Fi / hotspot, rooms are discovered by broadcast.
 * Online: direct IP (port-forward) or a mesh VPN such as Tailscale / ZeroTier.
 */
class MultiplayerActivity : AppCompatActivity() {

    data class Room(val ip: String, val name: String, val players: Int, val max: Int, val status: Int)
    data class Player(val id: Int, val name: String, val status: Int, val ip: String, val local: Boolean, val ping: Int)

    companion object {
        fun parsePlayers(json: String): List<Player> = try {
            val a = JSONArray(json)
            (0 until a.length()).map {
                val o = a.getJSONObject(it)
                Player(o.getInt("id"), o.getString("name"), o.getInt("status"), o.getString("ip"), o.getBoolean("local"), o.getInt("ping"))
            }
        } catch (e: Exception) { emptyList() }

        fun parseRooms(json: String): List<Room> = try {
            val a = JSONArray(json)
            (0 until a.length()).map {
                val o = a.getJSONObject(it)
                Room(o.getString("ip"), o.getString("name"), o.getInt("players"), o.getInt("max"), o.getInt("status"))
            }
        } catch (e: Exception) { emptyList() }
    }

    private lateinit var b: ActivityMultiplayerBinding
    private var multicastLock: WifiManager.MulticastLock? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        b = ActivityMultiplayerBinding.inflate(layoutInflater)
        setContentView(b.root)
        b.toolbar.setNavigationOnClickListener { finish() }

        b.name.setText(Prefs.sp(this).getString("mp_name", null) ?: Prefs.nickname(this))
        b.hostAddr.setText(Prefs.sp(this).getString("mp_last_host", ""))

        val updateMax = { b.maxLabel.text = "${getString(R.string.mp_max_players)}: ${b.maxPlayers.value.toInt()}" }
        b.maxPlayers.value = Prefs.sp(this).getInt("mp_max", 4).toFloat().coerceIn(2f, 16f)
        updateMax()
        b.maxPlayers.addOnChangeListener { _, _, _ -> updateMax() }

        b.host.setOnClickListener { host() }
        b.join.setOnClickListener { join(b.hostAddr.text.toString().trim()) }
        b.leave.setOnClickListener {
            lifecycleScope.launch {
                withContext(Dispatchers.IO) {
                    NativeLib.mpEnd()
                    NetLock.release()
                    NativeLib.mpStartDiscovery()
                }
                refresh()
            }
        }
        b.startGame.setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java).putExtra(MainActivity.EXTRA_PICK_FOR_MP, true))
        }

        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (true) {
                    refresh()
                    delay(1000)
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        multicastLock = wifi.createMulticastLock("emunds-lan").apply { setReferenceCounted(false); acquire() }
        if (NativeLib.mpState() == 0) NativeLib.mpStartDiscovery()
    }

    override fun onStop() {
        super.onStop()
        if (NativeLib.mpState() == 0) NativeLib.mpEndDiscovery()
        multicastLock?.release()
        multicastLock = null
    }

    private fun playerName(): String {
        val n = b.name.text.toString().trim().ifEmpty { "Player" }.take(10)
        Prefs.sp(this).edit().putString("mp_name", n).apply()
        return n
    }

    private fun host() {
        val max = b.maxPlayers.value.toInt()
        Prefs.sp(this).edit().putInt("mp_max", max).apply()
        val name = playerName()
        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) { NativeLib.mpHost(name, max) }
            if (ok) NetLock.acquire(this@MultiplayerActivity)
            if (!ok) Toast.makeText(this@MultiplayerActivity, R.string.mp_host_failed, Toast.LENGTH_LONG).show()
            refresh()
        }
    }

    private fun join(addr: String) {
        if (addr.isEmpty()) return
        Prefs.sp(this).edit().putString("mp_last_host", addr).apply()
        val name = playerName()
        b.join.isEnabled = false
        lifecycleScope.launch {
            // StartClient blocks while ENet connects / resolves DNS
            val ok = withContext(Dispatchers.IO) { NativeLib.mpJoin(name, addr) }
            if (ok) NetLock.acquire(this@MultiplayerActivity)
            b.join.isEnabled = true
            if (!ok) {
                Toast.makeText(this@MultiplayerActivity, getString(R.string.mp_join_failed, addr), Toast.LENGTH_LONG).show()
                NativeLib.mpStartDiscovery()
            }
            refresh()
        }
    }

    private suspend fun refresh() {
        val state = NativeLib.mpState()
        val (rooms, players) = withContext(Dispatchers.IO) {
            parseRooms(NativeLib.mpDiscovery()) to parsePlayers(NativeLib.mpPlayers())
        }
        val inSession = state != 0
        b.sessionPanel.visibility = if (inSession) View.VISIBLE else View.GONE
        b.lobbyPanel.visibility = if (inSession) View.GONE else View.VISIBLE
        b.name.isEnabled = !inSession

        if (inSession) {
            b.sessionStatus.text = getString(if (state == 1) R.string.mp_connected_as_host else R.string.mp_connected_as_client)
            b.myIp.text = getString(R.string.mp_your_ip, localAddresses().joinToString(", ").ifEmpty { "?" })
            b.players.removeAllViews()
            for (p in players.filter { it.status != 0 }) {
                val st = when (p.status) {
                    1 -> getString(R.string.mp_status_client)
                    2 -> getString(R.string.mp_status_host)
                    3 -> getString(R.string.mp_status_connecting)
                    else -> getString(R.string.mp_status_disconnected)
                }
                val ping = if (!p.local && p.ping > 0) " · ${p.ping} ms" else ""
                b.players.addView(row("${p.id + 1}. ${p.name}${if (p.local) " (kamu)" else ""}", "$st$ping"))
            }
        } else {
            b.rooms.removeAllViews()
            for (r in rooms) {
                val v = row(r.name.ifEmpty { r.ip }, "${r.ip} · ${r.players}/${r.max}${if (r.status == 1) " · sedang main" else ""}")
                v.setOnClickListener { join(r.ip) }
                b.rooms.addView(v)
            }
            b.noRooms.visibility = if (rooms.isEmpty()) View.VISIBLE else View.GONE
        }
    }

    private fun row(title: String, sub: String): View {
        val v = layoutInflater.inflate(R.layout.item_rom, b.root as android.view.ViewGroup, false)
        v.findViewById<View>(R.id.icon).visibility = View.GONE
        v.findViewById<TextView>(R.id.title).text = title
        v.findViewById<TextView>(R.id.subtitle).text = sub
        return v
    }

    /** IPv4 addresses of this device (Wi-Fi, hotspot, Tailscale/ZeroTier tunnels...). */
    private fun localAddresses(): List<String> {
        val out = ArrayList<String>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.inetAddresses) {
                    if (a is Inet4Address && !a.isLoopbackAddress) out += "${a.hostAddress} (${ni.name})"
                }
            }
        } catch (_: Exception) {
        }
        if (out.isEmpty()) {
            val cm = getSystemService(ConnectivityManager::class.java)
            cm.getLinkProperties(cm.activeNetwork)?.linkAddresses?.forEach {
                if (it.address is Inet4Address) out += it.address.hostAddress ?: ""
            }
        }
        return out
    }
}
