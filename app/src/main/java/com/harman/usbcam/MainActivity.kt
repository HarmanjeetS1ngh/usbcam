package com.harman.usbcam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.media.MediaCodecList
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import android.widget.TextView

/**
 * M1: no preview (saves GPU/CPU/heat). Configure from the PC via intent extras:
 *   adb shell am start -n com.harman.usbcam/.MainActivity --ei w 3840 --ei h 2160 --ei fps 30 --ei br 40000000
 */
class MainActivity : Activity() {
    private lateinit var tv: TextView
    private var streamer: Streamer? = null
    private var last = "Starting..."
    private val caps by lazy { capsText() }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        tv = TextView(this).apply {
            setPadding(32, 32, 32, 32)
            textSize = 13f
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.BLACK)
        }
        setContentView(tv)
        render()
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
        else begin()
    }

    override fun onNewIntent(i: Intent) {
        super.onNewIntent(i)
        intent = i
        begin()
    }

    override fun onRequestPermissionsResult(rc: Int, p: Array<out String>, r: IntArray) {
        if (r.isNotEmpty() && r[0] == PackageManager.PERMISSION_GRANTED) begin()
        else status("Camera permission denied")
    }

    private fun begin() {
        streamer?.stop()
        val i = intent
        val cfg = Streamer.Config(
            w = i.getIntExtra("w", 1920),
            h = i.getIntExtra("h", 1080),
            fps = i.getIntExtra("fps", 30),
            bitrate = i.getIntExtra("br", 12_000_000),
            port = i.getIntExtra("port", 5000),
            facing = i.getIntExtra("facing", 1) // 1 = back, 0 = front
        )
        val s = Streamer(this) { msg -> runOnUiThread { status(msg) } }
        streamer = s
        try { s.start(cfg) } catch (e: Exception) { status("Start failed: ${e.message}") }
    }

    private fun status(msg: String) { last = msg; render() }
    private fun render() { tv.text = "UsbCam M1\n$last\n\nEncoders:\n$caps" }

    override fun onDestroy() {
        streamer?.stop()
        super.onDestroy()
    }

    private fun capsText(): String {
        val sb = StringBuilder()
        for (ci in MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos) {
            if (!ci.isEncoder) continue
            for (t in listOf("video/avc", "video/hevc")) {
                if (!ci.supportedTypes.contains(t)) continue
                val v = ci.getCapabilitiesForType(t).videoCapabilities
                val hw = if (Build.VERSION.SDK_INT >= 29) (if (ci.isHardwareAccelerated) "HW" else "SW") else "?"
                sb.append("$hw ${ci.name} $t: max ${v.supportedWidths.upper}x${v.supportedHeights.upper}, ")
                    .append("${v.bitrateRange.upper / 1_000_000} Mbps\n")
            }
        }
        return sb.toString().ifEmpty { "none found" }
    }
}
