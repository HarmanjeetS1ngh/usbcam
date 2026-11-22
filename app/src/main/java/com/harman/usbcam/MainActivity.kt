package com.harman.usbcam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.util.Log
import android.widget.ScrollView
import android.media.MediaCodecList
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import android.widget.TextView

/**
 * Extra: --es cam <id> picks a camera (normal id or physical id, see on-screen list). M1: no preview (saves GPU/CPU/heat). Configure from the PC via intent extras:
 *   adb shell am start -n com.harman.usbcam/.MainActivity --ei w 3840 --ei h 2160 --ei fps 30 --ei br 40000000
 */
class MainActivity : Activity() {
    private lateinit var tv: TextView
    private var streamer: Streamer? = null
    private var last = "Starting..."
    private val caps by lazy { capsText() }
    private val cams by lazy { camsText().also { Log.i("UsbCam", it) } }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        tv = TextView(this).apply {
            setPadding(32, 32, 32, 32)
            textSize = 11f
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.BLACK)
        }
        setContentView(ScrollView(this).apply { addView(tv) })
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
        Log.i("UsbCam", "device ${Build.MANUFACTURER} ${Build.MODEL} sdk=${Build.VERSION.SDK_INT}\ncameras:\n$cams\nencoders:\n$caps")
        val i = intent
        val cfg = Streamer.Config(
            w = i.getIntExtra("w", 1920),
            h = i.getIntExtra("h", 1080),
            fps = i.getIntExtra("fps", 30),
            bitrate = i.getIntExtra("br", 12_000_000),
            port = i.getIntExtra("port", 5000),
            facing = i.getIntExtra("facing", 1), // 1 = back, 0 = front
            cam = i.getStringExtra("cam"),
            zoom = i.getFloatExtra("zoom", 0f)
        )
        val s = Streamer(this) { msg -> runOnUiThread { status(msg) } }
        streamer = s
        try { s.start(cfg) } catch (e: Exception) { status("Start failed: ${e.message}") }
    }

    private fun status(msg: String) { last = msg; Log.i("UsbCam", msg); render() }
    private fun render() { tv.text = "UsbCam m1g\n$last\n\nCameras (use: --es cam <id>):\n$cams\nEncoders:\n$caps" }

    override fun onDestroy() {
        streamer?.stop()
        super.onDestroy()
    }

    private fun describe(c: CameraCharacteristics, label: String, indent: String): String {
        val face = when (c.get(CameraCharacteristics.LENS_FACING)) { 0 -> "front"; 1 -> "back"; else -> "ext" }
        val fl = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.joinToString("/") ?: "?"
        val all = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)?.getOutputSizes(MediaCodec::class.java)
        val best = all?.maxByOrNull { it.width.toLong() * it.height }
        val wide = all?.filter { it.width * 9 == it.height * 16 && it.width >= 1920 }
            ?.sortedByDescending { it.width }?.take(4)?.joinToString(",") { "${it.width}x${it.height}" } ?: ""
        val fps = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.map { it.upper }?.distinct()?.sorted()?.joinToString(",") ?: "?"
        val hs = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)?.highSpeedVideoFpsRanges
            ?.map { it.upper }?.distinct()?.sorted()?.joinToString(",").orEmpty()
        return "$indent$label ($face) f=${fl}mm max ${best?.width}x${best?.height} 16:9[$wide] fps[$fps] hs[$hs]\n"
    }

    private fun camsText(): String {
        val sb = StringBuilder()
        try {
            val mgr = getSystemService(CAMERA_SERVICE) as CameraManager
            for (id in mgr.cameraIdList) {
                val c = mgr.getCameraCharacteristics(id)
                sb.append(describe(c, "cam $id", ""))
                if (Build.VERSION.SDK_INT >= 28) for (pid in c.physicalCameraIds) {
                    try { sb.append(describe(mgr.getCameraCharacteristics(pid), "phys $pid", "   ")) }
                    catch (_: Exception) { sb.append("   phys $pid (no info)\n") }
                }
            }
        } catch (e: Exception) { sb.append("camera list failed: ${e.message}\n") }
        return sb.toString()
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
