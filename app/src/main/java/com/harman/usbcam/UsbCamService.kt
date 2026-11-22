package com.harman.usbcam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import org.json.JSONObject

/**
 * Owns the camera pipeline and the JSON control channel. Runs as a camera foreground service, so streaming
 * continues with the screen off / app in background. The activity only starts it and shows status.
 *
 * Control commands (one JSON object per line, port 5002):
 *   hello | caps | status | time | keyframe | stop | quit
 *   start {cam,w,h,fps,br,port,hdr}   (omitted fields = default mode from caps)
 *   bitrate {bps}
 */
class UsbCamService : Service() {
    companion object {
        const val VERSION = "0.2-m2"
        const val ACTION_START = "com.harman.usbcam.START"
        const val ACTION_QUIT = "com.harman.usbcam.QUIT"
        const val CONTROL_PORT = 5002
        private const val CHANNEL = "usbcam"
        @Volatile var status = "Idle"
        @Volatile var listener: ((String) -> Unit)? = null
    }

    private val lock = Any()
    private var streamer: Streamer? = null
    private var control: ControlServer? = null
    private var wake: PowerManager.WakeLock? = null
    private val caps: JSONObject by lazy { Caps.build(this) }
    private val ui = Handler(Looper.getMainLooper())

    override fun onBind(i: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_QUIT) { shutdown(); return START_NOT_STICKY }
        goForeground()
        if (control == null) {
            Log.i("UsbCam", "UsbCam $VERSION\n" + Caps.text(caps))
            control = ControlServer(CONTROL_PORT) { handle(it) }.also { it.start() }
            if (streamer == null) setStatus("Idle | control on port $CONTROL_PORT")
        }
        if (intent != null && listOf("w", "h", "fps", "br", "cam").any { intent.hasExtra(it) }) {
            val d = caps.getJSONObject("default")
            val w = intent.getIntExtra("w", d.getInt("w")); val h = intent.getIntExtra("h", d.getInt("h"))
            val fps = intent.getIntExtra("fps", d.getInt("fps"))
            val cfg = Streamer.Config(
                w = w, h = h, fps = fps,
                bitrate = intent.getIntExtra("br", Caps.bitrate(w, h, fps)),
                port = intent.getIntExtra("port", 5000),
                facing = intent.getIntExtra("facing", 1),
                cam = intent.getStringExtra("cam") ?: d.optString("cam").ifEmpty { null },
                zoom = intent.getFloatExtra("zoom", 0f),
                gl = intent.getBooleanExtra("gl", false),
                tpl = intent.getIntExtra("tpl", 0),
                nostab = intent.getBooleanExtra("nostab", false),
                sw = intent.getIntExtra("sw", 0), sh = intent.getIntExtra("sh", 0),
                usemtx = intent.getBooleanExtra("usemtx", false),
                hdr = intent.getBooleanExtra("hdr", false),
                ll = intent.getIntExtra("ll", 1)
            )
            synchronized(lock) { startStream(cfg) }
        }
        return START_NOT_STICKY
    }

    private fun goForeground() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "UsbCam streaming", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val quit = PendingIntent.getService(this, 1, Intent(this, UsbCamService::class.java).setAction(ACTION_QUIT), PendingIntent.FLAG_IMMUTABLE)
        @Suppress("DEPRECATION")
        val n = Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("UsbCam $VERSION")
            .setContentText("USB webcam service running")
            .setContentIntent(open)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", quit)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 30) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        else startForeground(1, n)
    }

    private fun setStatus(msg: String) {
        status = msg
        Log.i("UsbCam", msg)
        listener?.invoke(msg)
    }

    /** Caller holds [lock]. Replaces any running stream. */
    private fun startStream(cfg: Streamer.Config) {
        stopStream()
        try {
            val s = Streamer(this) { setStatus(it) }
            streamer = s
            s.start(cfg)
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "usbcam:stream").apply {
                setReferenceCounted(false); acquire(12 * 60 * 60 * 1000L)
            }
        } catch (e: Exception) {
            setStatus("Start failed: ${e.message}")
            stopStream()
        }
    }

    private fun stopStream() {
        try { streamer?.stop() } catch (_: Exception) {}
        streamer = null
        try { wake?.let { if (it.isHeld) it.release() } } catch (_: Exception) {}
        wake = null
        setStatus("Idle | control on port $CONTROL_PORT")
    }

    private fun shutdown() {
        synchronized(lock) { stopStream() }
        control?.stop(); control = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        synchronized(lock) { stopStream() }
        control?.stop(); control = null
        super.onDestroy()
    }

    // ---- control channel ----

    private fun state(s: Streamer?): JSONObject {
        val o = JSONObject().put("state", if (s == null) "idle" else "streaming").put("msg", status)
        if (s != null) {
            val c = s.cfgNow
            o.put("client", s.clientConnected).put("encFps", s.encFpsNow.toDouble()).put("encMbps", s.encMbpsNow.toDouble())
                .put("drops", s.dropCount).put("info", s.infoText)
            if (c != null) o.put("actual", JSONObject().put("cam", c.cam ?: "").put("w", c.w).put("h", c.h).put("fps", c.fps)
                .put("br", c.bitrate).put("port", c.port).put("hdr", c.hdr))
        }
        return o
    }

    private fun handle(req: JSONObject): JSONObject = synchronized(lock) {
        val ok = JSONObject().put("ok", true)
        when (val cmd = req.optString("cmd")) {
            "hello" -> ok.put("app", "UsbCam").put("ver", VERSION).put("proto", 1).put("hdrBytes", Streamer.HDR)
                .put("state", if (streamer == null) "idle" else "streaming")
            "caps" -> ok.put("caps", caps)
            "status" -> merge(ok, state(streamer))
            "time" -> ok.put("t_us", SystemClock.elapsedRealtimeNanos() / 1000).put("mono_us", System.nanoTime() / 1000) // frame pts is one of these two clocks (PC picks the one that fits)
            "keyframe" -> { streamer?.keyframe(); ok }
            "bitrate" -> { streamer?.setBitrate(req.getInt("bps")); ok }
            "stop" -> { stopStream(); ok }
            "quit" -> { ui.postDelayed({ shutdown() }, 300); ok }
            "start" -> {
                val d = caps.getJSONObject("default")
                val w = req.optInt("w", d.getInt("w")); val h = req.optInt("h", d.getInt("h"))
                val fps = req.optInt("fps", d.getInt("fps"))
                val cfg = Streamer.Config(
                    w = w, h = h, fps = fps,
                    bitrate = req.optInt("br", Caps.bitrate(w, h, fps)),
                    port = req.optInt("port", 5000),
                    facing = 1,
                    cam = if (req.has("cam")) req.getString("cam") else d.optString("cam").ifEmpty { null },
                    hdr = req.optBoolean("hdr", false),
                    ll = req.optInt("ll", 1)
                )
                startStream(cfg)
                if (streamer == null) JSONObject().put("ok", false).put("err", status) else merge(ok, state(streamer))
            }
            else -> JSONObject().put("ok", false).put("err", "unknown cmd '$cmd'")
        }
    }

    private fun merge(a: JSONObject, b: JSONObject): JSONObject {
        for (k in b.keys()) a.put(k, b.get(k))
        return a
    }
}
