package com.emunds.nds

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.provider.DocumentsContract
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

data class RomEntry(
    val uri: String,
    val fileName: String,
    val size: Long,
    val modified: Long,
    val title: String,
    val gameCode: String,
    val iconPath: String?,
)

/** Scans a SAF folder for .nds ROMs and extracts title + icon from the cartridge banner. */
object RomLibrary {
    private const val CACHE = "library.json"

    fun loadCached(ctx: Context): List<RomEntry> {
        val f = File(ctx.filesDir, CACHE)
        if (!f.exists()) return emptyList()
        return try {
            val arr = JSONArray(f.readText())
            (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                RomEntry(
                    o.getString("uri"), o.getString("file"), o.getLong("size"), o.getLong("mod"),
                    o.getString("title"), o.getString("code"), o.optString("icon").ifEmpty { null },
                )
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    private fun saveCache(ctx: Context, list: List<RomEntry>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(JSONObject().apply {
                put("uri", it.uri); put("file", it.fileName); put("size", it.size); put("mod", it.modified)
                put("title", it.title); put("code", it.gameCode); put("icon", it.iconPath ?: "")
            })
        }
        File(ctx.filesDir, CACHE).writeText(arr.toString())
    }

    /** Recursive scan (blocking: call from a background dispatcher). */
    fun scan(ctx: Context, tree: Uri): List<RomEntry> {
        val old = loadCached(ctx).associateBy { it.uri }
        val out = ArrayList<RomEntry>()
        val rootId = DocumentsContract.getTreeDocumentId(tree)
        walk(ctx, tree, rootId, 0, old, out)
        out.sortBy { it.title.lowercase() }
        saveCache(ctx, out)
        return out
    }

    private fun walk(ctx: Context, tree: Uri, docId: String, depth: Int, old: Map<String, RomEntry>, out: MutableList<RomEntry>) {
        if (depth > 4) return
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val cols = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        ctx.contentResolver.query(children, cols, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getString(0)
                val name = c.getString(1) ?: continue
                val mime = c.getString(2) ?: ""
                if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                    walk(ctx, tree, id, depth + 1, old, out)
                    continue
                }
                if (!name.endsWith(".nds", ignoreCase = true) && !name.endsWith(".dsi", ignoreCase = true)) continue
                val size = c.getLong(3)
                val mod = c.getLong(4)
                val uri = DocumentsContract.buildDocumentUriUsingTree(tree, id).toString()
                val cached = old[uri]
                if (cached != null && cached.size == size && cached.modified == mod) {
                    out += cached
                } else {
                    out += parse(ctx, Uri.parse(uri), name, size, mod)
                }
            }
        }
    }

    fun parse(ctx: Context, uri: Uri, name: String, size: Long, mod: Long): RomEntry {
        var title = name.substringBeforeLast('.')
        var code = ""
        var icon: String? = null
        try {
            ctx.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                FileInputStream(pfd.fileDescriptor).channel.use { ch ->
                    val hdr = ByteBuffer.allocate(0x200).order(ByteOrder.LITTLE_ENDIAN)
                    ch.read(hdr, 0)
                    code = String(hdr.array(), 0x0C, 4, Charsets.US_ASCII).trim { it <= ' ' }
                    val bannerOff = hdr.getInt(0x68).toLong() and 0xFFFFFFFFL
                    if (bannerOff in 0x200 until size - 0x840) {
                        val b = ByteBuffer.allocate(0x840).order(ByteOrder.LITTLE_ENDIAN)
                        ch.read(b, bannerOff)
                        // English title (falls back to Japanese)
                        val en = utf16(b, 0x340).ifBlank { utf16(b, 0x240) }
                        if (en.isNotBlank()) title = en.lines().filter { it.isNotBlank() }.take(2).joinToString(" — ")
                        icon = saveIcon(ctx, b, "${code}_${Integer.toHexString(name.hashCode())}")
                    }
                }
            }
        } catch (_: Exception) {
        }
        return RomEntry(uri.toString(), name, size, mod, title, code, icon)
    }

    private fun utf16(b: ByteBuffer, off: Int): String {
        val sb = StringBuilder()
        for (i in 0 until 128) {
            val ch = b.getShort(off + i * 2).toInt().toChar()
            if (ch == '\u0000') break
            sb.append(ch)
        }
        return sb.toString().trim()
    }

    private fun saveIcon(ctx: Context, b: ByteBuffer, key: String): String? {
        val pal = IntArray(16) { i ->
            val c = b.getShort(0x220 + i * 2).toInt() and 0x7FFF
            val r = (c and 0x1F) * 255 / 31
            val g = ((c shr 5) and 0x1F) * 255 / 31
            val bl = ((c shr 10) and 0x1F) * 255 / 31
            if (i == 0) 0 else (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
        }
        val px = IntArray(32 * 32)
        // 4x4 tiles of 8x8 pixels, 4bpp, low nibble first
        for (tile in 0 until 16) {
            val tx = (tile % 4) * 8
            val ty = (tile / 4) * 8
            for (i in 0 until 32) {
                val v = b.get(0x20 + tile * 32 + i).toInt() and 0xFF
                val y = ty + i / 4
                val x = tx + (i % 4) * 2
                px[y * 32 + x] = pal[v and 0xF]
                px[y * 32 + x + 1] = pal[v shr 4]
            }
        }
        val bmp = Bitmap.createBitmap(px, 32, 32, Bitmap.Config.ARGB_8888)
        val dir = File(ctx.filesDir, "icons").apply { mkdirs() }
        val f = File(dir, "$key.png")
        return try {
            f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            f.absolutePath
        } catch (e: Exception) {
            null
        }
    }
}
