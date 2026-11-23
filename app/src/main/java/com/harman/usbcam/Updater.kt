package com.harman.usbcam

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import androidx.core.content.pm.PackageInfoCompat
import org.json.JSONArray
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Self-update from GitHub Releases. The Actions workflow publishes every APK as release "build-<run number>"
 * (the run number is also the app's versionCode) and uses RELEASE_NOTES.md as the release text.
 * All network functions block: call them from a background thread.
 */
object Updater {
    const val REPO = "HarmanjeetS1ngh/usbcam"

    class Info(val code: Long, val name: String, val notes: String, val url: String, val size: Long)

    fun installedCode(c: Context): Long = PackageInfoCompat.getLongVersionCode(c.packageManager.getPackageInfo(c.packageName, 0))

    fun installedName(c: Context): String = c.packageManager.getPackageInfo(c.packageName, 0).versionName ?: ""

    fun canInstall(c: Context): Boolean = c.packageManager.canRequestPackageInstalls()

    /** Newest published build, or null when there is none. Throws on network problems. */
    fun fetchLatest(): Info? {
        val conn = URL("https://api.github.com/repos/$REPO/releases?per_page=10").openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 8000
        conn.setRequestProperty("Accept", "application/vnd.github+json")
        try {
            if (conn.responseCode != 200) throw IOException("HTTP " + conn.responseCode)
            val arr = JSONArray(conn.inputStream.bufferedReader().use { it.readText() })
            var best: Info? = null
            for (i in 0 until arr.length()) {
                val r = arr.getJSONObject(i)
                if (r.optBoolean("draft")) continue
                val tag = r.optString("tag_name")
                if (!tag.startsWith("build-")) continue
                val code = tag.removePrefix("build-").toLongOrNull() ?: continue
                val assets = r.optJSONArray("assets") ?: continue
                var url = ""
                var size = 0L
                for (k in 0 until assets.length()) {
                    val a = assets.getJSONObject(k)
                    if (a.optString("name").endsWith(".apk")) { url = a.optString("browser_download_url"); size = a.optLong("size"); break }
                }
                if (url.isEmpty()) continue
                val cur = best
                if (cur == null || code > cur.code) best = Info(code, r.optString("name", tag), r.optString("body"), url, size)
            }
            return best
        } finally {
            conn.disconnect()
        }
    }

    /** Release text without markdown marks, bullets as "•". */
    fun plainNotes(md: String): String = md.replace("\r", "").replace("**", "").lines().joinToString("\n") { l ->
        val t = l.trim().trimStart('#').trim()
        if (t.startsWith("- ") || t.startsWith("* ")) "•  " + t.substring(2) else t
    }.trim()

    /** Downloads the APK into the cache folder. [cancelled] is polled while downloading. */
    fun download(c: Context, info: Info, onProgress: (Int) -> Unit, cancelled: () -> Boolean): File {
        val dir = File(c.cacheDir, "updates")
        dir.mkdirs()
        dir.listFiles()?.forEach { it.delete() }
        val out = File(dir, "usbcam-" + info.code + ".apk")
        val conn = URL(info.url).openConnection() as HttpURLConnection
        conn.connectTimeout = 10000
        conn.readTimeout = 15000
        try {
            if (conn.responseCode != 200) throw IOException("HTTP " + conn.responseCode)
            val total = if (info.size > 0) info.size else conn.contentLengthLong
            var done = 0L
            var last = -1
            conn.inputStream.use { ins ->
                out.outputStream().use { os ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        if (cancelled()) throw IOException("cancelled")
                        val n = ins.read(buf)
                        if (n < 0) break
                        os.write(buf, 0, n)
                        done += n
                        if (total > 0) {
                            val p = (done * 100 / total).toInt()
                            if (p != last) { last = p; onProgress(p) }
                        }
                    }
                }
            }
            if (info.size > 0 && done != info.size) throw IOException("incomplete download")
        } catch (e: Exception) {
            out.delete()
            throw e
        } finally {
            conn.disconnect()
        }
        return out
    }

    /** Hands the APK to Android's installer (it asks the user to confirm). */
    fun install(c: Context, apk: File) {
        val uri = FileProvider.getUriForFile(c, c.packageName + ".updates", apk)
        val i = Intent(Intent.ACTION_VIEW)
        i.setDataAndType(uri, "application/vnd.android.package-archive")
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        c.startActivity(i)
    }
}
