package com.harman.usbcam

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.view.Surface
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Camera2 -> MediaCodec (hardware H.264, Surface input, zero CPU copies)
 * -> Annex-B bytes over a loopback TCP server. The PC reaches it via `adb forward`.
 * Loopback-only bind: nothing on Wi-Fi can connect.
 */
class Streamer(private val ctx: Context, private val onStatus: (String) -> Unit) {

    class Config(var w: Int, var h: Int, val fps: Int, val bitrate: Int, val port: Int, val facing: Int, val cam: String? = null)
    private class Pkt(val data: ByteArray, val key: Boolean)

    @Volatile private var running = false
    @Volatile private var info = ""
    @Volatile private var resync = true
    @Volatile private var codecConfig: ByteArray? = null
    private var codec: MediaCodec? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var camThread: HandlerThread? = null
    private var server: ServerSocket? = null
    private val queue = LinkedBlockingQueue<Pkt>(6) // small on purpose: latency over completeness
    private val threads = mutableListOf<Thread>()

    @SuppressLint("MissingPermission")
    fun start(cfg: Config) {
        if (running) return
        running = true
        resync = true
        codecConfig = null
        queue.clear()

        val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val target = resolve(mgr, cfg)
        val note = snap(mgr, target, cfg)
        info = "cam ${target.second ?: target.first} ${cfg.w}x${cfg.h}@${cfg.fps}$note"

        val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, cfg.w, cfg.h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
            setInteger("prepend-sps-pps-to-idr-frames", 1)
        }
        val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val input = c.createInputSurface()
        c.start()
        codec = c

        spawn("drain") { drain(c) }
        spawn("net") { serve(cfg.port, c) }
        openCamera(mgr, target, cfg, input)
    }

    fun stop() {
        running = false
        try { server?.close() } catch (_: Exception) {}
        try { session?.stopRepeating(); session?.close() } catch (_: Exception) {}
        try { camera?.close() } catch (_: Exception) {}
        threads.forEach { try { it.join(500) } catch (_: Exception) {} }
        threads.clear()
        try { codec?.stop(); codec?.release() } catch (_: Exception) {}
        camThread?.quitSafely()
        codec = null; camera = null; session = null; camThread = null; server = null
    }

    private fun spawn(name: String, r: () -> Unit) {
        val t = Thread(r, name); threads.add(t); t.start()
    }

    private fun requestKey(c: MediaCodec) {
        try {
            c.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: Exception) {}
    }

    private fun drain(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val i = try { c.dequeueOutputBuffer(info, 100_000) } catch (_: Exception) { break }
            if (i < 0) continue
            val buf = c.getOutputBuffer(i)
            if (buf != null && info.size > 0) {
                val bytes = ByteArray(info.size) // M2: replace with a buffer pool to cut GC churn
                buf.position(info.offset); buf.limit(info.offset + info.size); buf.get(bytes)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    codecConfig = bytes
                } else {
                    val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                    if (!queue.offer(Pkt(bytes, key))) {
                        // Consumer too slow (or nobody connected): drop everything, resume at next keyframe.
                        queue.clear(); resync = true; requestKey(c)
                    }
                }
            }
            try { c.releaseOutputBuffer(i, false) } catch (_: Exception) { break }
        }
    }

    private fun serve(port: Int, c: MediaCodec) {
        try {
            val ss = ServerSocket(port, 1, InetAddress.getByName("127.0.0.1"))
            server = ss
            onStatus("Waiting for PC on port $port | $info")
            while (running) {
                val s = try { ss.accept() } catch (_: Exception) { break }
                onStatus("PC connected, streaming | $info")
                try { pump(s, c) } catch (_: Exception) {}
                try { s.close() } catch (_: Exception) {}
                if (running) onStatus("PC disconnected, waiting on port $port | $info")
            }
        } catch (e: Exception) {
            onStatus("Server error: ${e.message}")
        }
    }

    private fun pump(s: Socket, c: MediaCodec) {
        s.tcpNoDelay = true
        val out = s.getOutputStream()
        queue.clear()
        resync = true
        requestKey(c)
        codecConfig?.let { out.write(it) }
        while (running) {
            val p = queue.poll(200, TimeUnit.MILLISECONDS) ?: continue
            if (resync) { if (!p.key) continue; resync = false }
            out.write(p.data)
        }
    }

    /** If the camera cannot output the requested size, use the largest supported size with the same aspect ratio. */
    private fun snap(mgr: CameraManager, target: Pair<String, String?>, cfg: Config): String {
        try {
            val ch = try { mgr.getCameraCharacteristics(target.second ?: target.first) }
                     catch (_: Exception) { mgr.getCameraCharacteristics(target.first) }
            val sizes = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(MediaCodec::class.java) ?: return ""
            if (sizes.any { it.width == cfg.w && it.height == cfg.h }) return ""
            val ar = cfg.w.toDouble() / cfg.h
            val best = sizes.filter {
                Math.abs(it.width.toDouble() / it.height - ar) < 0.02 && it.width <= cfg.w && it.height <= cfg.h
            }.maxByOrNull { it.width.toLong() * it.height } ?: return ""
            val note = " (snapped from ${cfg.w}x${cfg.h})"
            cfg.w = best.width; cfg.h = best.height
            return note
        } catch (_: Exception) { return "" }
    }

    /** Pick camera: explicit id (normal or physical sub-camera of a logical one), else first with requested facing. */
    private fun resolve(mgr: CameraManager, cfg: Config): Pair<String, String?> {
        val ids = mgr.cameraIdList
        val want = cfg.cam
        if (want != null) {
            if (want in ids) return want to null
            if (Build.VERSION.SDK_INT >= 28) {
                for (id in ids) {
                    if (mgr.getCameraCharacteristics(id).physicalCameraIds.contains(want)) return id to want
                }
            }
        }
        val id = ids.firstOrNull {
            mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == cfg.facing
        } ?: ids[0]
        return id to null
    }

    @SuppressLint("MissingPermission")
    private fun openCamera(mgr: CameraManager, target: Pair<String, String?>, cfg: Config, surface: Surface) {
        val (id, phys) = target
        val t = HandlerThread("cam").also { it.start() }
        camThread = t
        val h = Handler(t.looper)

        mgr.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(cam: CameraDevice) {
                camera = cam
                val cb = object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {
                        session = s
                        val req = cam.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(surface)
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, Range(cfg.fps, cfg.fps))
                            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                        }.build()
                        s.setRepeatingRequest(req, null, h)
                    }
                    override fun onConfigureFailed(s: CameraCaptureSession) {
                        onStatus("Camera session failed: $info unsupported on this camera")
                    }
                }
                if (phys != null && Build.VERSION.SDK_INT >= 28) {
                    val oc = OutputConfiguration(surface).apply { setPhysicalCameraId(phys) }
                    val sc = SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(oc), { r -> h.post(r) }, cb)
                    cam.createCaptureSession(sc)
                } else {
                    @Suppress("DEPRECATION")
                    cam.createCaptureSession(listOf(surface), cb, h)
                }
            }
            override fun onDisconnected(cam: CameraDevice) { cam.close() }
            override fun onError(cam: CameraDevice, error: Int) { cam.close(); onStatus("Camera error $error") }
        }, h)
    }
}
