package com.harman.usbcam

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject

/**
 * Capability-driven modes: only sizes/fps that BOTH the camera and the HW H.264 encoder support are offered.
 * Camera2 cannot report whether a big size is a sensor crop, so >1080p is labelled from a measured-device table.
 */
object Caps {
    /** Models where >1080p was MEASURED as a ~2x centre crop (S22: HANDOFF m1j). Add models here when measured. */
    private val CROP_MODELS = listOf("SM-S901")
    private val FPS_OK = setOf(24, 25, 30, 50, 60)

    fun cropNote(w: Int): String = when {
        w <= 1920 -> ""
        CROP_MODELS.any { Build.MODEL.startsWith(it) } -> "cropped"
        else -> "unverified"
    }

    private fun encoderInfo(): MediaCodecInfo? {
        val all = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { it.isEncoder && it.supportedTypes.contains(MediaFormat.MIMETYPE_VIDEO_AVC) }
        return all.firstOrNull { Build.VERSION.SDK_INT < 29 || it.isHardwareAccelerated } ?: all.firstOrNull()
    }

    private fun encVc(): MediaCodecInfo.VideoCapabilities? =
        try { encoderInfo()?.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)?.videoCapabilities } catch (_: Exception) { null }

    /** ~0.2 bits per pixel: 1080p30 = 12.4 Mbps, 4K30 = 49.8 Mbps; clamped to the encoder's limit. */
    fun bitrate(w: Int, h: Int, fps: Int): Int {
        val max = encVc()?.bitrateRange?.upper?.toLong() ?: 100_000_000L
        return (w.toLong() * h * fps / 5).coerceIn(2_000_000L, minOf(100_000_000L, max)).toInt()
    }

    private fun modes(ch: CameraCharacteristics, vc: MediaCodecInfo.VideoCapabilities?): JSONArray {
        val out = JSONArray()
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return out
        val ae = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.map { it.upper }?.toSet() ?: setOf(30)
        val sizes = map.getOutputSizes(MediaCodec::class.java) ?: return out
        for (s in sizes.filter { it.width * 9 == it.height * 16 && it.width >= 1280 }.sortedByDescending { it.width }) {
            try {
                if (vc != null && !vc.isSizeSupported(s.width, s.height)) continue
                val encMax = vc?.getSupportedFrameRatesFor(s.width, s.height)?.upper?.toInt() ?: 60
                val dur = map.getOutputMinFrameDuration(MediaCodec::class.java, s)
                val camMax = if (dur > 0) (1_000_000_000.0 / dur + 0.5).toInt() else 60
                val fps = ae.filter { it in FPS_OK && it <= encMax && it <= camMax }.sortedDescending()
                if (fps.isEmpty()) continue
                out.put(JSONObject()
                    .put("w", s.width).put("h", s.height)
                    .put("label", if (s.height >= 2160) "4K" else "${s.height}p")
                    .put("fps", JSONArray(fps))
                    .put("br", bitrate(s.width, s.height, fps[0]))
                    .put("note", cropNote(s.width)))
            } catch (_: Exception) { /* size/fps not queryable: skip it */ }
        }
        return out
    }

    private fun camJson(id: String, ch: CameraCharacteristics, vc: MediaCodecInfo.VideoCapabilities?): JSONObject {
        val face = when (ch.get(CameraCharacteristics.LENS_FACING)) { 0 -> "front"; 1 -> "back"; else -> "ext" }
        val focal = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull()?.toDouble() ?: 0.0
        return JSONObject().put("id", id).put("facing", face).put("focal", focal).put("modes", modes(ch, vc))
    }

    fun build(ctx: Context): JSONObject {
        val root = JSONObject()
        root.put("device", JSONObject().put("model", Build.MODEL).put("sdk", Build.VERSION.SDK_INT))
        val ei = encoderInfo()
        val vc = encVc()
        root.put("encoder", JSONObject().put("name", ei?.name ?: "none")
            .put("hw", ei != null && (Build.VERSION.SDK_INT < 29 || ei.isHardwareAccelerated))
            .put("maxBitrate", vc?.bitrateRange?.upper ?: 0))
        val cams = JSONArray()
        var def: JSONObject? = null
        try {
            val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            for (id in mgr.cameraIdList) {
                val ch = mgr.getCameraCharacteristics(id)
                val cj = camJson(id, ch, vc)
                if (Build.VERSION.SDK_INT >= 28) {
                    val ph = JSONArray()
                    for (pid in ch.physicalCameraIds) {
                        try { ph.put(camJson(pid, mgr.getCameraCharacteristics(pid), vc)) } catch (_: Exception) {}
                    }
                    cj.put("phys", ph)
                }
                cams.put(cj)
                if (def == null && cj.getString("facing") == "back") def = defaultMode(id, cj.getJSONArray("modes"))
            }
            if (def == null && cams.length() > 0) def = defaultMode(cams.getJSONObject(0).getString("id"), cams.getJSONObject(0).getJSONArray("modes"))
        } catch (e: Exception) { root.put("error", e.message ?: "camera query failed") }
        root.put("cameras", cams)
        root.put("default", def ?: JSONObject().put("cam", "0").put("w", 1920).put("h", 1080).put("fps", 30).put("br", bitrate(1920, 1080, 30)))
        return root
    }

    /** Default = largest uncropped-by-label mode up to 1080p, 30 fps if offered (decision: 1080p30 main lens). */
    private fun defaultMode(camId: String, modes: JSONArray): JSONObject? {
        var best: JSONObject? = null
        for (i in 0 until modes.length()) {
            val m = modes.getJSONObject(i)
            if (m.getInt("h") <= 1080 && (best == null || m.getInt("w") > best.getInt("w"))) best = m
        }
        val m = best ?: (if (modes.length() > 0) modes.getJSONObject(modes.length() - 1) else return null)
        val f = m.getJSONArray("fps")
        var fps = f.getInt(0)
        for (i in 0 until f.length()) if (f.getInt(i) == 30) fps = 30
        return JSONObject().put("cam", camId).put("w", m.getInt("w")).put("h", m.getInt("h")).put("fps", fps).put("br", bitrate(m.getInt("w"), m.getInt("h"), fps))
    }

    /** Human-readable summary for the phone screen and the log. */
    fun text(j: JSONObject): String {
        val sb = StringBuilder()
        val d = j.getJSONObject("device"); val e = j.getJSONObject("encoder")
        sb.append("${d.getString("model")} sdk=${d.getInt("sdk")}  encoder ${e.getString("name")} hw=${e.getBoolean("hw")} max ${e.getInt("maxBitrate") / 1_000_000} Mbps\n")
        val cams = j.getJSONArray("cameras")
        for (i in 0 until cams.length()) {
            line(sb, "", cams.getJSONObject(i))
            val ph = cams.getJSONObject(i).optJSONArray("phys") ?: continue
            for (k in 0 until ph.length()) line(sb, "   phys ", ph.getJSONObject(k))
        }
        sb.append("default: ${j.getJSONObject("default")}\n")
        return sb.toString()
    }

    private fun line(sb: StringBuilder, pre: String, c: JSONObject) {
        sb.append("${pre}cam ${c.getString("id")} (${c.getString("facing")}) f=${c.getDouble("focal")}mm: ")
        val m = c.getJSONArray("modes")
        if (m.length() == 0) sb.append("no 16:9 modes")
        for (i in 0 until m.length()) {
            val x = m.getJSONObject(i)
            val note = x.getString("note")
            sb.append("${x.getString("label")}@${x.getJSONArray("fps").join("/")}${if (note.isEmpty()) "" else " [$note]"}  ")
        }
        sb.append("\n")
    }
}
