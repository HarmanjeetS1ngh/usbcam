package com.harman.usbcam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import org.json.JSONArray
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
        const val VERSION = "0.5-m5"
        const val ACTION_START = "com.harman.usbcam.START"
        const val ACTION_QUIT = "com.harman.usbcam.QUIT"
        const val ACTION_APPLY = "com.harman.usbcam.APPLY"   // restart the running stream with the saved phone choice
        // thermal / battery guard thresholds (battery temperature in deg C)
        private const val T_REDUCE = 42f; private const val T_STOP = 46f; private const val T_RECOVER = 39f; private const val T_RESUME = 40f
        private const val BATT_STOP = 10
        const val CONTROL_PORT = 5002
        private const val CHANNEL = "usbcam"
        @Volatile var status = "Idle"
        @Volatile var listener: ((String) -> Unit)? = null
        @Volatile var battLevel = -1; @Volatile var battTemp = 0f; @Volatile var plugged = false
        @Volatile var thermal = 0; @Volatile var guardMsg = ""; @Volatile var guardStage = 0   // 0 ok, 1 bitrate reduced, 2 stopped
    }

    private val lock = Any()
    private var streamer: Streamer? = null
    private var control: ControlServer? = null
    private var wake: PowerManager.WakeLock? = null
    private val caps: JSONObject by lazy { Caps.build(this) }
    private val ui = Handler(Looper.getMainLooper())
    private var baseBr = 0
    private var guardBlock: String? = null
    private val guardTick = object : Runnable { override fun run() { guardCheck(); ui.postDelayed(this, 5000) } }

    override fun onBind(i: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_QUIT) { shutdown(); return START_NOT_STICKY }
        goForeground()
        ui.removeCallbacks(guardTick); ui.post(guardTick)
        if (intent?.action == ACTION_APPLY) { synchronized(lock) { applySaved() }; return START_NOT_STICKY }
        if (control == null) {
            Log.i("UsbCam", "UsbCam $VERSION\n" + Caps.text(caps))
            control = ControlServer(CONTROL_PORT) { handle(it) }.also { it.start() }
            if (streamer == null) setStatus("Idle | control on port $CONTROL_PORT")
        }
        if (intent != null && listOf("w", "h", "fps", "br", "cam").any { intent.hasExtra(it) }) {
            val d = baseDefault()
            val w = intent.getIntExtra("w", d.getInt("w")); val h = intent.getIntExtra("h", d.getInt("h"))
            val fps = intent.getIntExtra("fps", d.getInt("fps"))
            val cfg = Streamer.Config(
                w = w, h = h, fps = fps,
                bitrate = intent.getIntExtra("br", if (intent.hasExtra("w") || intent.hasExtra("h")) Caps.bitrate(w, h, fps) else d.getInt("br")),
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
                ll = intent.getIntExtra("ll", Prefs(this).ll)
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
        guardCheck()
        guardBlock?.let { setStatus(it); return }
        try {
            baseBr = cfg.bitrate; guardStage = 0
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
        ui.removeCallbacks(guardTick)
        synchronized(lock) { stopStream() }
        control?.stop(); control = null
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        ui.removeCallbacks(guardTick)
        synchronized(lock) { stopStream() }
        control?.stop(); control = null
        super.onDestroy()
    }

    // ---- control channel ----

    private fun state(s: Streamer?): JSONObject {
        val o = JSONObject().put("state", if (s == null) "idle" else "streaming").put("msg", status)
            .put("batt", battLevel).put("tempC", battTemp.toDouble()).put("plugged", plugged).put("thermal", thermal)
            .put("guardStage", guardStage).put("guard", guardMsg).put("guardOn", Prefs(this).guard)
            .put("sel", selJson())
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
            "select" -> {   // save the phone choice (what the viewer / OBS get when they ask for the default); apply=true restarts a running stream
                val p = Prefs(this)
                if (req.has("cam")) p.cam = req.getString("cam")
                if (req.has("w")) p.w = req.getInt("w"); if (req.has("h")) p.h = req.getInt("h")
                if (req.has("fps")) p.fps = req.getInt("fps")
                if (req.has("br")) p.br = req.getInt("br")
                if (req.has("ll")) p.ll = req.getInt("ll")
                if (req.has("guard")) p.guard = req.getBoolean("guard")
                if (req.optBoolean("clear", false)) { p.cam = ""; p.w = 0; p.h = 0; p.fps = 0; p.br = 0 }
                if (req.optBoolean("apply", false)) applySaved()
                merge(ok, state(streamer))
            }
            "start" -> {
                val d = baseDefault()
                val w = req.optInt("w", d.getInt("w")); val h = req.optInt("h", d.getInt("h"))
                val fps = req.optInt("fps", d.getInt("fps"))
                val cfg = Streamer.Config(
                    w = w, h = h, fps = fps,
                    bitrate = req.optInt("br", if (req.has("w") || req.has("h")) Caps.bitrate(w, h, fps) else d.getInt("br")),
                    port = req.optInt("port", 5000),
                    facing = 1,
                    cam = if (req.has("cam")) req.getString("cam") else d.optString("cam").ifEmpty { null },
                    hdr = req.optBoolean("hdr", false),
                    ll = req.optInt("ll", Prefs(this).ll)
                )
                startStream(cfg)
                if (streamer == null) JSONObject().put("ok", false).put("err", status) else merge(ok, state(streamer))
            }
            else -> JSONObject().put("ok", false).put("err", "unknown cmd '$cmd'")
        }
    }

    /** Default mode = the choice saved on the phone if there is one, else the capability default. */
    private fun baseDefault(): JSONObject {
        val d = JSONObject(caps.getJSONObject("default").toString())
        val p = Prefs(this)
        if (p.hasChoice) {
            d.put("w", p.w).put("h", p.h)
            if (p.cam.isNotEmpty()) d.put("cam", p.cam)
            if (p.fps > 0) d.put("fps", p.fps)
            d.put("br", if (p.br > 0) p.br else Caps.bitrate(p.w, p.h, d.getInt("fps")))
        }
        return d
    }

    private fun selJson(): JSONObject {
        val p = Prefs(this); val d = baseDefault()
        return JSONObject().put("cam", d.optString("cam")).put("w", d.getInt("w")).put("h", d.getInt("h")).put("fps", d.getInt("fps"))
            .put("br", d.getInt("br")).put("auto", p.br == 0).put("saved", p.hasChoice).put("ll", p.ll)
    }

    /** Caller holds [lock]. Restarts the running stream (same port/hdr) with the saved phone choice. */
    private fun applySaved() {
        val old = streamer?.cfgNow ?: return
        val d = baseDefault()
        val cfg = Streamer.Config(w = d.getInt("w"), h = d.getInt("h"), fps = d.getInt("fps"), bitrate = d.getInt("br"),
            port = old.port, facing = 1, cam = d.optString("cam").ifEmpty { null }, hdr = old.hdr, ll = Prefs(this).ll)
        startStream(cfg)
    }

    // ---- thermal / battery guard ----

    private fun guardCheck() {
        try {
            val b = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            if (b != null) {
                val lvl = b.getIntExtra("level", -1); val scale = b.getIntExtra("scale", 100)
                battLevel = if (lvl >= 0 && scale > 0) lvl * 100 / scale else -1
                battTemp = b.getIntExtra("temperature", 0) / 10f
                plugged = b.getIntExtra("plugged", 0) != 0
            }
            thermal = if (Build.VERSION.SDK_INT >= 29) (getSystemService(Context.POWER_SERVICE) as PowerManager).currentThermalStatus else 0
        } catch (_: Exception) {}
        val on = Prefs(this).guard
        val hot = battTemp >= T_STOP || thermal >= 3 || (!plugged && battLevel in 0..BATT_STOP)
        val warm = battTemp >= T_REDUCE || thermal >= 2
        val cool = battTemp <= T_RECOVER && thermal <= 1
        // a stopped stream stays blocked until the phone has cooled down (otherwise the PC would restart it at once)
        guardBlock?.let { if (!on || (battTemp <= T_RESUME && thermal <= 1 && (plugged || battLevel > BATT_STOP + 5))) { guardBlock = null; guardStage = 0; guardMsg = "" } }
        val s = streamer
        if (!on) { if (guardStage == 1 && s != null) s.setBitrate(baseBr); guardStage = 0; guardMsg = ""; return }
        if (hot) {
            if (guardBlock == null) {
                val why = if (!plugged && battLevel in 0..BATT_STOP) "battery ${battLevel}%" else "phone hot (${"%.1f".format(battTemp)} C, thermal $thermal)"
                guardBlock = "Stopped by guard: $why. Waiting to cool down / charge."
                guardStage = 2; guardMsg = guardBlock!!
                if (s != null) synchronized(lock) { stopStream() }
                setStatus(guardBlock!!)
            }
        } else if (s != null && warm && guardStage == 0) {
            guardStage = 1; baseBr = s.cfgNow?.bitrate ?: baseBr
            s.setBitrate(baseBr / 2)
            guardMsg = "Warm (${"%.1f".format(battTemp)} C): bitrate halved"
            setStatus(guardMsg)
        } else if (s != null && cool && guardStage == 1) {
            s.setBitrate(baseBr); guardStage = 0; guardMsg = ""
            setStatus("Cooled down: bitrate restored")
        }
    }

    private fun merge(a: JSONObject, b: JSONObject): JSONObject {
        for (k in b.keys()) a.put(k, b.get(k))
        return a
    }
}
