package com.harman.usbcam

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.graphics.PointF
import android.graphics.Rect
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * Camera2 -> MediaCodec (hardware H.264, Surface input, zero CPU copies)
 * -> Annex-B bytes over a loopback TCP server. The PC reaches it via `adb forward`.
 * Loopback-only bind: nothing on Wi-Fi can connect.
 */
class Streamer(private val ctx: Context, private val onStatus: (String) -> Unit) {

    class Config(var w: Int, var h: Int, var fps: Int, val bitrate: Int, val port: Int, val facing: Int, val cam: String? = null, val zoom: Float = 0f,
                 val gl: Boolean = false, val tpl: Int = 0, val nostab: Boolean = false, val sw: Int = 0, val sh: Int = 0, val usemtx: Boolean = false,
                 val hdr: Boolean = false, val ll: Int = 1)

    /** Pooled frame buffer: [HDR bytes reserved for the wire header][payload]. Reused, so no per-frame allocation. */
    private class Pkt(var buf: ByteArray) { var len = 0; var pts = 0L; var key = false; var tq = 0L }
    companion object {
        const val HDR = 13        // [4B payload length][8B pts_us][1B flags], big-endian
        const val F_KEY = 1; const val F_CFG = 2
        private const val POOL = 10 // queue(6) + one in drain + one in pump + spare
    }

    @Volatile private var running = false
    @Volatile private var info = ""
    @Volatile private var drops = 0
    private var encFrames = 0
    private var encBytes = 0L
    private var encT = System.nanoTime()
    private var latSum = 0.0; private var latN = 0          // camera timestamp -> encoder output (ms)
    private val sendUs = java.util.concurrent.atomic.AtomicLong(); private val sendN = java.util.concurrent.atomic.AtomicLong() // dequeue -> socket write done
    private var camFrames = 0
    private var camT = System.nanoTime()
    private fun log(m: String) { Log.i("UsbCam", m) }
    @Volatile private var resync = true
    private var aeRange: Range<Int>? = null
    @Volatile private var codecConfig: ByteArray? = null
    private var codec: MediaCodec? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var gl: GlBridge? = null
    private var camThread: HandlerThread? = null
    private var server: ServerSocket? = null
    private val queue = LinkedBlockingQueue<Pkt>(6) // small on purpose: latency over completeness
    private val pool = ArrayBlockingQueue<Pkt>(POOL).also { for (i in 0 until POOL) it.add(Pkt(ByteArray(HDR + 262_144))) }
    @Volatile var encFpsNow = 0f; @Volatile var encMbpsNow = 0f; @Volatile var clientConnected = false
    @Volatile var cfgNow: Config? = null
    @Volatile var preview: Surface? = null          // on-screen preview (CameraActivity); camera gets it as a second output
    var ctl: CamControls? = null
    @Volatile var activeId = ""
    private var camHandler: Handler? = null
    private var camChars: CameraCharacteristics? = null
    private var encSurf: Surface? = null
    private var camPhys: String? = null
    private var curOuts: List<Surface> = emptyList()
    private var sessGen = 0
    val infoText: String get() = info
    val dropCount: Int get() = drops
    private val threads = mutableListOf<Thread>()

    @SuppressLint("MissingPermission")
    fun start(cfg: Config) {
        if (running) return
        running = true
        resync = true
        codecConfig = null
        drainQueue()
        cfgNow = cfg
        drops = 0; encFrames = 0; encBytes = 0; camFrames = 0
        encT = System.nanoTime(); camT = encT

        val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val target = resolve(mgr, cfg)
        val note = snap(mgr, target, cfg) + pickFps(mgr, target.first, cfg)
        activeId = target.second ?: target.first
        info = "cam ${target.second ?: target.first} ${cfg.w}x${cfg.h}@${cfg.fps}$note"
        log("START $info bitrate=${cfg.bitrate} requestedCam=${cfg.cam} logicalId=${target.first} physicalId=${target.second} aeRange=$aeRange zoom=${cfg.zoom}")

        fun buildFmt(ll: Int) = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, cfg.w, cfg.h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, cfg.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, cfg.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LATENCY, 1)
            setInteger("prepend-sps-pps-to-idr-frames", 1)
            if (ll >= 1) {  // low-latency tuning (cfg.ll = 0 turns it off for A/B tests)
                if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                setInteger(MediaFormat.KEY_OPERATING_RATE, cfg.fps)
                setInteger("vendor.qti-ext-enc-low-latency.enable", 1)   // Qualcomm (ignored elsewhere)
                setInteger("vendor.rtc-ext-enc-low-latency.enable", 1)   // Exynos (ignored elsewhere)
            }
        }
        var c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        var llUsed = cfg.ll
        try { c.configure(buildFmt(cfg.ll), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE) }
        catch (e: Exception) {
            log("configure with low-latency options failed (${e.message}), retrying plain")
            try { c.release() } catch (_: Exception) {}
            c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            llUsed = 0
            c.configure(buildFmt(0), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        log("encoder ${c.name} ll=$llUsed")
        val input = c.createInputSurface()
        c.start()
        codec = c

        spawn("drain") { drain(c) }
        spawn("net") { serve(cfg.port, c, cfg.hdr) }
        var camSurface: Surface = input
        if (cfg.gl) {
            val src = pickSrc(mgr, target, cfg)
            if (src != null) {
                val g = GlBridge(input, cfg.w, cfg.h, src.width, src.height, cfg.usemtx)
                val cs = g.start()
                if (cs != null) { gl = g; camSurface = cs; log("GL path: camera ${src.width}x${src.height} -> encoder ${cfg.w}x${cfg.h}") }
                else { g.release(); log("GL init failed, using direct encoder surface") }
            } else log("no SurfaceTexture size >= ${cfg.w}x${cfg.h}, using direct encoder surface")
        }
        openCamera(mgr, target, cfg, camSurface)
    }

    fun stop() {
        running = false
        clientConnected = false
        try { server?.close() } catch (_: Exception) {}
        try { session?.stopRepeating(); session?.close() } catch (_: Exception) {}
        try { camera?.close() } catch (_: Exception) {}
        try { gl?.release() } catch (_: Exception) {}
        gl = null
        threads.forEach { try { it.join(500) } catch (_: Exception) {} }
        threads.clear()
        try { codec?.stop(); codec?.release() } catch (_: Exception) {}
        camThread?.quitSafely()
        codec = null; camera = null; session = null; camThread = null; server = null; camHandler = null; encSurf = null
    }

    private fun spawn(name: String, r: () -> Unit) {
        val t = Thread(r, name); threads.add(t); t.start()
    }

    fun keyframe() { codec?.let { requestKey(it) } }

    fun setBitrate(bps: Int) {
        try { codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps) }) } catch (_: Exception) {}
    }

    private fun recycle(p: Pkt) { pool.offer(p) }
    private fun drainQueue() { while (true) { recycle(queue.poll() ?: break) } }
    private fun dropAll(c: MediaCodec) { drops++; drainQueue(); resync = true; requestKey(c) }

    private fun requestKey(c: MediaCodec) {
        try {
            c.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: Exception) {}
    }

    private fun drain(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val i = try { c.dequeueOutputBuffer(info, 100_000) } catch (_: Exception) { break }
            if (i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) { log("encoder format: ${c.outputFormat}"); continue }
            if (i < 0) continue
            val buf = c.getOutputBuffer(i)
            if (buf != null && info.size > 0) {
                encFrames++; encBytes += info.size
                val now = System.nanoTime()
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) { latSum += (now / 1000 - info.presentationTimeUs) / 1000.0; latN++ }
                if (now - encT > 2_000_000_000L) {
                    val sec = (now - encT) / 1e9
                    encFpsNow = (encFrames / sec).toFloat(); encMbpsNow = (encBytes * 8 / sec / 1e6).toFloat()
                    val sn = sendN.getAndSet(0); val su = sendUs.getAndSet(0)
                    log("enc fps=${"%.1f".format(encFpsNow)} Mbps=${"%.1f".format(encMbpsNow)} drops=$drops camToEnc=${"%.1f".format(if (latN > 0) latSum / latN else 0.0)}ms queueSend=${"%.2f".format(if (sn > 0) su / 1000.0 / sn else 0.0)}ms")
                    encFrames = 0; encBytes = 0; encT = now; latSum = 0.0; latN = 0
                }
                buf.position(info.offset); buf.limit(info.offset + info.size)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    codecConfig = ByteArray(info.size).also { buf.get(it) }
                } else if (clientConnected) { // nobody listening: don't copy/queue (avoids useless drops + keyframe requests)
                    val p = pool.poll()
                    if (p == null) dropAll(c) // all buffers in flight: consumer too slow
                    else {
                        if (p.buf.size < HDR + info.size) p.buf = ByteArray((HDR + info.size) * 3 / 2)
                        buf.get(p.buf, HDR, info.size)
                        p.len = info.size; p.pts = info.presentationTimeUs; p.tq = now
                        p.key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                        // Consumer too slow (or nobody connected): drop everything, resume at next keyframe.
                        if (!queue.offer(p)) { recycle(p); dropAll(c) }
                    }
                }
            }
            try { c.releaseOutputBuffer(i, false) } catch (_: Exception) { break }
        }
    }

    private fun serve(port: Int, c: MediaCodec, hdr: Boolean) {
        try {
            val ss = ServerSocket(port, 1, InetAddress.getByName("127.0.0.1"))
            server = ss
            onStatus("Waiting for PC on port $port | $info")
            while (running) {
                val s = try { ss.accept() } catch (_: Exception) { break }
                onStatus("PC connected, streaming | $info")
                clientConnected = true
                try { pump(s, c, hdr) } catch (_: Exception) {}
                clientConnected = false
                try { s.close() } catch (_: Exception) {}
                if (running) onStatus("PC disconnected, waiting on port $port | $info")
            }
        } catch (e: Exception) {
            onStatus("Server error: ${e.message}")
        }
    }

    private fun putHeader(b: ByteArray, len: Int, pts: Long, flags: Int) {
        b[0] = (len ushr 24).toByte(); b[1] = (len ushr 16).toByte(); b[2] = (len ushr 8).toByte(); b[3] = len.toByte()
        for (i in 0 until 8) b[4 + i] = (pts ushr (56 - 8 * i)).toByte()
        b[12] = flags.toByte()
    }

    /** hdr=false: raw Annex-B (ffplay / obs-bridge). hdr=true: [len][pts_us][flags] + payload per frame (M3 receiver). */
    private fun pump(s: Socket, c: MediaCodec, hdr: Boolean) {
        s.tcpNoDelay = true
        val out = s.getOutputStream()
        drainQueue()
        resync = true
        requestKey(c)
        codecConfig?.let {
            if (hdr) {
                val b = ByteArray(HDR + it.size); System.arraycopy(it, 0, b, HDR, it.size)
                putHeader(b, it.size, 0L, F_CFG); out.write(b)
            } else out.write(it)
        }
        while (running) {
            val p = queue.poll(200, TimeUnit.MILLISECONDS) ?: continue
            try {
                if (resync) { if (!p.key) continue; resync = false }
                if (hdr) {
                    putHeader(p.buf, p.len, p.pts, if (p.key) F_KEY else 0)
                    out.write(p.buf, 0, HDR + p.len)
                } else out.write(p.buf, HDR, p.len)
                sendUs.addAndGet((System.nanoTime() - p.tq) / 1000); sendN.incrementAndGet()
            } finally { recycle(p) }
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

    /** For the GL path: smallest camera SurfaceTexture size (4:3 preferred) that covers the output size. */
    private fun pickSrc(mgr: CameraManager, target: Pair<String, String?>, cfg: Config): Size? {
        try {
            val ch = try { mgr.getCameraCharacteristics(target.second ?: target.first) }
                     catch (_: Exception) { mgr.getCameraCharacteristics(target.first) }
            val sizes = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                ?.getOutputSizes(SurfaceTexture::class.java) ?: return null
            log("glSizes=" + sizes.sortedByDescending { it.width.toLong() * it.height }.take(8).joinToString(",") { "${it.width}x${it.height}" })
            if (cfg.sw > 0) return sizes.firstOrNull { it.width == cfg.sw && it.height == cfg.sh }
            val cover = sizes.filter { it.width >= cfg.w && it.height >= cfg.h }
            return cover.filter { Math.abs(it.width * 3 - it.height * 4) < 8 }.minByOrNull { it.width.toLong() * it.height }
                ?: cover.minByOrNull { it.width.toLong() * it.height }
        } catch (_: Exception) { return null }
    }

    /** Use an AE fps range the camera really offers; if cfg.fps is not offered, take the nearest higher one, else the maximum. */
    private fun pickFps(mgr: CameraManager, logicalId: String, cfg: Config): String {
        aeRange = null
        try {
            val r = mgr.getCameraCharacteristics(logicalId)
                .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return ""
            val up = r.map { it.upper }
            val tgt = up.filter { it >= cfg.fps }.minOrNull() ?: (up.maxOrNull() ?: cfg.fps)
            aeRange = r.filter { it.upper == tgt }.maxByOrNull { it.lower }
            if (tgt == cfg.fps) return ""
            val note = " (fps ${cfg.fps} not offered, using $tgt)"
            cfg.fps = tgt
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
    /** Ask for the cheaper (FAST) ISP modes, but only those the camera says it supports. */
    private fun fastIsp(b: CaptureRequest.Builder, ch: CameraCharacteristics?) {
        if (ch == null) return
        fun has(a: IntArray?, v: Int) = a?.contains(v) == true
        try {
            if (has(ch.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES), CaptureRequest.NOISE_REDUCTION_MODE_FAST))
                b.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)
            if (has(ch.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES), CaptureRequest.EDGE_MODE_FAST))
                b.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST)
            if (has(ch.get(CameraCharacteristics.HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES), CaptureRequest.HOT_PIXEL_MODE_FAST))
                b.set(CaptureRequest.HOT_PIXEL_MODE, CaptureRequest.HOT_PIXEL_MODE_FAST)
            if (has(ch.get(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES), CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE_FAST))
                b.set(CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE, CaptureRequest.COLOR_CORRECTION_ABERRATION_MODE_FAST)
        } catch (_: Exception) {}
    }

    /** Called from the UI when the preview surface appears (non-null) or goes away (null). Re-creates only the camera session; the encoder keeps running. */
    fun setPreview(sf: Surface?) {
        preview = sf
        camHandler?.post { if (running && camera != null) startSession() }
    }

    /** Re-reads the manual controls and pushes them to the running camera. */
    fun applyControls() {
        camHandler?.post { if (running) pushRequest() }
    }

    private var builderCfg: Config? = null

    private fun openCamera(mgr: CameraManager, target: Pair<String, String?>, cfg: Config, surface: Surface) {
        val (id, phys) = target
        val chars: CameraCharacteristics? = try { mgr.getCameraCharacteristics(id) } catch (_: Exception) { null }
        val t = HandlerThread("cam").also { it.start() }
        camThread = t
        val h = Handler(t.looper)
        camHandler = h; camChars = chars; encSurf = surface; camPhys = phys; builderCfg = cfg
        ctl?.load(chars, cfg.fps)
        mgr.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(cam: CameraDevice) { camera = cam; startSession() }
            override fun onDisconnected(cam: CameraDevice) { cam.close() }
            override fun onError(cam: CameraDevice, error: Int) { cam.close(); onStatus("Camera error $error") }
        }, h)
    }

    /** Runs on the camera thread. (Re)creates the capture session: encoder surface, plus the preview surface when there is one. */
    private fun startSession() {
        val cam = camera ?: return
        val h = camHandler ?: return
        val enc = encSurf ?: return
        val pv = preview?.takeIf { it.isValid }
        val outs = if (pv != null) listOf(enc, pv) else listOf(enc)
        val gen = ++sessGen
        val cb = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (gen != sessGen || !running) { try { s.close() } catch (_: Exception) {}; return }
                session = s; curOuts = outs
                pushRequest()
                codec?.let { requestKey(it) }   // the PC decoder resyncs after the short pause
            }
            override fun onConfigureFailed(s: CameraCaptureSession) {
                if (gen != sessGen) return
                if (pv != null) { log("session with preview failed, continuing without preview"); preview = null; startSession() }
                else onStatus("Camera session failed: $info unsupported on this camera")
            }
        }
        try {
            if (camPhys != null && Build.VERSION.SDK_INT >= 28) {
                val ocs = outs.map { OutputConfiguration(it).apply { setPhysicalCameraId(camPhys) } }
                cam.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR, ocs, { r -> h.post(r) }, cb))
            } else {
                @Suppress("DEPRECATION")
                cam.createCaptureSession(outs, cb, h)
            }
        } catch (e: Exception) { log("createCaptureSession: ${e.message}") }
    }

    private fun pushRequest() {
        val cam = camera ?: return; val s = session ?: return; val cfg = builderCfg ?: return
        try {
            val b = cam.createCaptureRequest(if (cfg.tpl > 0) cfg.tpl else CameraDevice.TEMPLATE_RECORD).apply {
                for (o in curOuts) addTarget(o)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeRange ?: Range(cfg.fps, cfg.fps))
                if (cfg.nostab) set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                if (cfg.zoom > 0f && Build.VERSION.SDK_INT >= 30) set(CaptureRequest.CONTROL_ZOOM_RATIO, cfg.zoom)
                set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                if (cfg.ll >= 1) fastIsp(this, camChars)  // less ISP work per frame, only modes the camera lists
                ctl?.let { applyCtl(this, it, cfg.fps) }
            }.build()
            s.setRepeatingRequest(b, captureCb, camHandler)
        } catch (e: Exception) { log("setRepeatingRequest: ${e.message}") }
    }

    private fun applyCtl(b: CaptureRequest.Builder, c: CamControls, fps: Int) {
        try {
            if (c.manualExp && c.manualOk) {
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                b.set(CaptureRequest.SENSOR_SENSITIVITY, c.iso)
                b.set(CaptureRequest.SENSOR_EXPOSURE_TIME, c.expNs)
                b.set(CaptureRequest.SENSOR_FRAME_DURATION, Math.max(1_000_000_000L / fps, c.expNs))
            } else {
                b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, c.ev)
                if (c.aeLock && c.aeLockOk) b.set(CaptureRequest.CONTROL_AE_LOCK, true)
            }
            if ((c.manualFocus || c.afLock) && c.minFocus > 0f) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, c.focusDist.coerceIn(0f, c.minFocus))
            }
            if (c.awbLock) b.set(CaptureRequest.CONTROL_AWB_LOCK, true)
            c.point?.let { p -> region(p, c)?.let { r -> b.set(CaptureRequest.CONTROL_AF_REGIONS, r); b.set(CaptureRequest.CONTROL_AE_REGIONS, r) } }
        } catch (e: Exception) { log("applyCtl: ${e.message}") }
    }

    private fun region(p: PointF, c: CamControls): Array<MeteringRectangle>? {
        val base = c.crop ?: c.activeArray ?: return null
        val sz = base.width() / 8
        val cx = base.left + (p.x * base.width()).toInt(); val cy = base.top + (p.y * base.height()).toInt()
        val r = Rect(cx - sz / 2, cy - sz / 2, cx + sz / 2, cy + sz / 2)
        if (!r.intersect(base)) return null
        return arrayOf(MeteringRectangle(r, 1000))
    }

    private val captureCb = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(sess: CameraCaptureSession, r: CaptureRequest, res: TotalCaptureResult) {
            camFrames++
            ctl?.let { c ->
                res.get(CaptureResult.SENSOR_SENSITIVITY)?.let { c.liveIso = it }
                res.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { c.liveExpNs = it }
                res.get(CaptureResult.LENS_FOCUS_DISTANCE)?.let { c.liveFocus = it }
                c.crop = res.get(CaptureResult.SCALER_CROP_REGION)
            }
            val now = System.nanoTime()
            if (now - camT > 2_000_000_000L) {
                val sec = (now - camT) / 1e9
                val ph = if (Build.VERSION.SDK_INT >= 28) res.get(CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID) else null
                log("cam fps=${"%.1f".format(camFrames / sec)} activePhysical=$ph focalLength=${res.get(CaptureResult.LENS_FOCAL_LENGTH)}mm iso=${res.get(CaptureResult.SENSOR_SENSITIVITY)} exp=${res.get(CaptureResult.SENSOR_EXPOSURE_TIME)} preview=${curOuts.size > 1}")
                camFrames = 0; camT = now
            }
        }
    }
}
