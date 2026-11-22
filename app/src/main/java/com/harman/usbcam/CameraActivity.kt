package com.harman.usbcam

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PointF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Live camera preview with on-screen controls in the style of the Samsung camera (Pro mode):
 * tap = focus + meter there, long-press = AE/AF LOCK, PRO panel with ISO / SPEED / EV / FOCUS / WB.
 * The preview is a second output of the running camera session; it exists only while this screen is open.
 */
class CameraActivity : Activity() {
    private val Y = Color.rgb(255, 214, 0)
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var sv: SurfaceView
    private lateinit var holderLayout: AspectLayout
    private lateinit var ring: View
    private lateinit var statusTv: TextView
    private lateinit var lockPill: TextView
    private lateinit var lockBtn: TextView
    private lateinit var proBtn: TextView
    private lateinit var panel: LinearLayout
    private lateinit var seek: SeekBar
    private lateinit var valueTv: TextView
    private lateinit var autoBtn: TextView
    private val chips = LinkedHashMap<String, TextView>()
    private lateinit var modeRow: LinearLayout
    private val modeBtns = LinkedHashMap<Int, TextView>()
    private var sel = "ISO"
    private var attached = false
    private val tick = object : Runnable { override fun run() { update(); ui.postDelayed(this, 250) } }

    private fun ctl() = UsbCamService.instance?.controls
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    /** Keeps the preview at the camera's 16:9 shape, centred. */
    private class AspectLayout(c: Context) : FrameLayout(c) {
        override fun onMeasure(w: Int, h: Int) {
            val mw = MeasureSpec.getSize(w); val mh = MeasureSpec.getSize(h)
            var cw = mw; var ch = (mw * 9f / 16f).toInt()
            if (ch > mh) { ch = mh; cw = (mh * 16f / 9f).toInt() }
            super.onMeasure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY))
        }
    }

    private fun bg(color: Int, r: Int, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color); cornerRadius = dp(r).toFloat(); if (stroke != 0) setStroke(dp(2), stroke)
    }

    private fun label(t: String, size: Float = 13f, color: Int = Color.WHITE, bold: Boolean = false) = TextView(this).apply {
        text = t; textSize = size; setTextColor(color); gravity = Gravity.CENTER; if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    private fun roundBtn(t: String, onClick: () -> Unit) = label(t, 12f, Color.WHITE, true).apply {
        background = bg(Color.argb(150, 0, 0, 0), 28); setOnClickListener { onClick() }
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_PREVIEW))
        setContentView(build())
    }

    private fun build(): View {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        // picture
        holderLayout = AspectLayout(this)
        sv = SurfaceView(this)
        sv.holder.setFixedSize(1280, 720)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { attached = UsbCamService.instance?.let { it.setPreview(h.surface); true } ?: false }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { attached = false; UsbCamService.instance?.setPreview(null) }
        })
        holderLayout.addView(sv, FrameLayout.LayoutParams(-1, -1))
        ring = View(this).apply { background = bg(Color.TRANSPARENT, 6, Color.WHITE); visibility = View.GONE }
        holderLayout.addView(ring, FrameLayout.LayoutParams(dp(76), dp(76)))
        val gd = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapUp(e: MotionEvent): Boolean { focusAt(e.x, e.y); return true }
            override fun onLongPress(e: MotionEvent) { lockAt(e.x, e.y) }
        })
        holderLayout.setOnTouchListener { _, e -> gd.onTouchEvent(e) }
        root.addView(holderLayout, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))

        // top bar
        val back = roundBtn("‹") { finish() }.apply { textSize = 26f }
        root.addView(back, FrameLayout.LayoutParams(dp(48), dp(48), Gravity.TOP or Gravity.START).apply { setMargins(dp(16), dp(12), 0, 0) })
        statusTv = label("", 12f).apply { background = bg(Color.argb(150, 0, 0, 0), 16); setPadding(dp(14), dp(6), dp(14), dp(6)) }
        root.addView(statusTv, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.CENTER_HORIZONTAL).apply { topMargin = dp(18) })
        lockPill = label("AE/AF LOCK", 12f, Color.BLACK, true).apply {
            background = bg(Y, 14); setPadding(dp(12), dp(6), dp(12), dp(6)); visibility = View.GONE
            setOnClickListener { ctl()?.unlockAeAf(); push(); update() }
        }
        root.addView(lockPill, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END).apply { setMargins(0, dp(18), dp(20), 0) })

        // right column
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER_HORIZONTAL }
        val flip = roundBtn("FLIP") { UsbCamService.instance?.flipCamera() }
        lockBtn = roundBtn("AE/AF\nLOCK") { toggleLock() }
        proBtn = roundBtn("PRO") { panel.visibility = if (panel.visibility == View.VISIBLE) View.GONE else View.VISIBLE; update() }
        for (v in listOf(flip, lockBtn, proBtn)) col.addView(v, LinearLayout.LayoutParams(dp(56), dp(56)).apply { setMargins(0, dp(8), 0, dp(8)) })
        root.addView(col, FrameLayout.LayoutParams(-2, -2, Gravity.END or Gravity.CENTER_VERTICAL).apply { rightMargin = dp(20) })

        // pro panel
        panel = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; background = bg(Color.argb(190, 0, 0, 0), 20); setPadding(dp(14), dp(8), dp(14), dp(10)); visibility = View.GONE }
        val chipRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for (n in listOf("ISO", "SPEED", "EV", "FOCUS", "WB")) {
            val c = label(n, 12f).apply { setPadding(dp(6), dp(6), dp(6), dp(6)); setOnClickListener { sel = n; sync(); update() } }
            chips[n] = c; chipRow.addView(c, LinearLayout.LayoutParams(dp(84), -2))
        }
        autoBtn = label("AUTO", 13f, Color.WHITE, true).apply { background = bg(Color.argb(90, 255, 255, 255), 14); setPadding(dp(14), dp(8), dp(14), dp(8)); setOnClickListener { auto() } }
        seek = SeekBar(this).apply { max = 100
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) { if (fromUser) slide(p) }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) {}
            }) }
        valueTv = label("", 14f, Y, true)
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(autoBtn, LinearLayout.LayoutParams(-2, -2).apply { rightMargin = dp(10) })
        row.addView(seek, LinearLayout.LayoutParams(0, dp(40), 1f))
        row.addView(valueTv, LinearLayout.LayoutParams(dp(84), -2))
        modeRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; visibility = View.GONE; setPadding(0, dp(4), 0, dp(6)) }
        for ((m, n) in listOf(CamControls.FM_AUTO to "AUTO\ncontinuous", CamControls.FM_SINGLE to "SINGLE\ntap to focus", CamControls.FM_INF to "\u221E\ninfinity",
                              CamControls.FM_LOCK to "LOCK\nhold focus", CamControls.FM_MANUAL to "MANUAL\nslider")) {
            val t = label(n, 11f).apply { setPadding(dp(4), dp(6), dp(4), dp(6)); setOnClickListener {
                val c = ctl() ?: return@setOnClickListener
                c.chooseFocus(m); if (m == CamControls.FM_MANUAL) c.focusDist = c.liveFocus
                push(); if (m == CamControls.FM_SINGLE) UsbCamService.instance?.triggerAf()
                sync(); update() } }
            modeBtns[m] = t; modeRow.addView(t, LinearLayout.LayoutParams(0, -2, 1f).apply { setMargins(dp(3), 0, dp(3), 0) })
        }
        panel.addView(chipRow); panel.addView(modeRow); panel.addView(row)
        root.addView(panel, FrameLayout.LayoutParams(dp(520), -2, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = dp(14) })
        sync()
        return root
    }

    override fun onStart() { super.onStart(); ui.post(tick) }
    override fun onStop() { ui.removeCallbacks(tick); UsbCamService.instance?.setPreview(null); attached = false; super.onStop() }
    override fun onResume() {
        super.onResume()
        // the service may start a moment after this screen: attach once it exists
        if (!attached && sv.holder.surface.isValid) attached = UsbCamService.instance?.let { it.setPreview(sv.holder.surface); true } ?: false
    }

    private fun push() { UsbCamService.instance?.applyControls() }

    // ---- gestures ----
    private fun showRing(x: Float, y: Float, color: Int) {
        ring.background = bg(Color.TRANSPARENT, 6, color)
        ring.translationX = x - dp(38); ring.translationY = y - dp(38)
        ring.visibility = View.VISIBLE
    }

    private fun focusAt(x: Float, y: Float) {
        val c = ctl() ?: return
        c.unlockAeAf(); if (c.focusMode == CamControls.FM_LOCK) c.focusMode = CamControls.FM_AUTO
        c.point = PointF(x / holderLayout.width, y / holderLayout.height)
        showRing(x, y, Color.WHITE); push()
        if (c.focusMode == CamControls.FM_SINGLE) UsbCamService.instance?.triggerAf()
        ui.postDelayed({ if (!c.locked) ring.visibility = View.GONE }, 1400)
    }

    private fun lockAt(x: Float, y: Float) {
        val c = ctl() ?: return
        c.point = PointF(x / holderLayout.width, y / holderLayout.height)
        showRing(x, y, Y); push()
        ui.postDelayed({ c.lockAeAf(); push(); update() }, 700)   // let AF settle on the point first
    }

    private fun toggleLock() {
        val c = ctl() ?: return
        if (c.locked) { c.unlockAeAf(); ring.visibility = View.GONE } else c.lockAeAf()
        push(); update()
    }

    // ---- pro panel ----
    private fun expLo(c: CamControls) = Math.max(c.expMin, 125_000L).toDouble()
    private fun expHi(c: CamControls) = Math.min(c.expMax, 1_000_000_000L / c.fps).toDouble()
    private fun toLog(v: Double, lo: Double, hi: Double) = (Math.log(v.coerceIn(lo, hi) / lo) / Math.log(hi / lo) * 100).roundToInt()
    private fun fromLog(p: Int, lo: Double, hi: Double) = lo * (hi / lo).pow(p / 100.0)

    private fun seedManual(c: CamControls) {
        if (c.manualExp) return
        c.iso = c.liveIso.coerceIn(c.isoMin, c.isoMax)
        c.expNs = c.liveExpNs.coerceIn(expLo(c).toLong(), expHi(c).toLong())
        c.manualExp = true
    }

    private fun slide(p: Int) {
        val c = ctl() ?: return
        when (sel) {
            "ISO" -> { if (!c.manualOk) { noManual(); return }; seedManual(c); c.iso = fromLog(p, c.isoMin.toDouble(), c.isoMax.toDouble()).roundToInt() }
            "SPEED" -> { if (!c.manualOk) { noManual(); return }; seedManual(c); c.expNs = fromLog(p, expLo(c), expHi(c)).toLong() }
            "EV" -> c.ev = c.evMin + p
            "FOCUS" -> { if (c.minFocus <= 0f) { Toast.makeText(this, "This camera has fixed focus", Toast.LENGTH_SHORT).show(); return }
                c.chooseFocus(CamControls.FM_MANUAL); c.focusDist = c.minFocus * (1f - p / 100f) }
        }
        push(); update()
    }

    private fun noManual() = Toast.makeText(this, "Manual ISO and speed are not supported on this camera", Toast.LENGTH_SHORT).show()

    private fun auto() {
        val c = ctl() ?: return
        when (sel) {
            "ISO", "SPEED" -> c.manualExp = false
            "EV" -> c.ev = 0
            "FOCUS" -> c.chooseFocus(CamControls.FM_AUTO)
            "WB" -> c.awbLock = !c.awbLock
        }
        push(); sync(); update()
    }

    /** Moves the slider to the current value of the selected chip. */
    private fun sync() {
        val c = ctl()
        for ((n, v) in chips) { v.setTextColor(if (n == sel) Y else Color.WHITE); v.background = if (n == sel) bg(Color.argb(60, 255, 214, 0), 12) else null }
        seek.visibility = if (sel == "WB") View.INVISIBLE else View.VISIBLE
        modeRow.visibility = if (sel == "FOCUS") View.VISIBLE else View.GONE
        ctl()?.let { c -> for ((m, t) in modeBtns) { t.setTextColor(if (c.fm == m) Color.BLACK else Color.WHITE); t.background = bg(if (c.fm == m) Y else Color.argb(70, 255, 255, 255), 12) } }
        if (c == null) return
        when (sel) {
            "ISO" -> { seek.max = 100; seek.progress = toLog((if (c.manualExp) c.iso else c.liveIso).toDouble(), c.isoMin.toDouble(), c.isoMax.toDouble()) }
            "SPEED" -> { seek.max = 100; seek.progress = toLog((if (c.manualExp) c.expNs else c.liveExpNs).toDouble(), expLo(c), expHi(c)) }
            "EV" -> { seek.max = Math.max(1, c.evMax - c.evMin); seek.progress = c.ev - c.evMin }
            "FOCUS" -> { seek.max = 100; seek.progress = if (c.minFocus > 0f) ((1f - (when (c.fm) { CamControls.FM_INF -> 0f; CamControls.FM_MANUAL, CamControls.FM_LOCK -> c.focusDist; else -> c.liveFocus }) / c.minFocus) * 100).roundToInt().coerceIn(0, 100) else 100 }
        }
    }

    private fun focusName(c: CamControls) = when (c.fm) {
        CamControls.FM_SINGLE -> "SINGLE"; CamControls.FM_INF -> "\u221E"; CamControls.FM_LOCK -> "LOCKED " + fmtFocus(c.focusDist)
        CamControls.FM_MANUAL -> fmtFocus(c.focusDist); else -> "AUTO"
    }

    private fun fmtExp(ns: Long): String { if (ns <= 0) return "-"; val d = 1e9 / ns; return if (d >= 2) "1/${d.roundToInt()}" else "%.1fs".format(ns / 1e9) }
    private fun fmtFocus(d: Float) = if (d < 0.05f) "∞" else "%.2f m".format(1f / d)
    private fun fmtEv(c: CamControls): String { val v = c.ev * c.evStep; return (if (v > 0) "+" else "") + "%.1f".format(v) }

    private fun update() {
        val s = UsbCamService.instance
        val c = s?.controls
        statusTv.text = if (s == null) "Starting..." else (s.infoNow().ifEmpty { "Starting stream..." } + if (s.clientOn()) "   ● LIVE to PC" else "   waiting for PC")
        if (c == null) return
        lockPill.visibility = if (c.locked) View.VISIBLE else View.GONE
        lockPill.text = if (c.aeLock && c.afLock) "AE/AF LOCK" else if (c.aeLock) "AE LOCK" else "AF LOCK"
        lockBtn.background = bg(if (c.locked) Y else Color.argb(150, 0, 0, 0), 28); lockBtn.setTextColor(if (c.locked) Color.BLACK else Color.WHITE)
        proBtn.background = bg(if (panel.visibility == View.VISIBLE) Y else Color.argb(150, 0, 0, 0), 28); proBtn.setTextColor(if (panel.visibility == View.VISIBLE) Color.BLACK else Color.WHITE)
        if (!c.locked && c.point == null && ring.visibility == View.VISIBLE) ring.visibility = View.GONE
        if (panel.visibility != View.VISIBLE) return
        chips["ISO"]?.text = "ISO\n" + if (c.manualExp) "${c.iso}" else "AUTO ${c.liveIso}"
        chips["SPEED"]?.text = "SPEED\n" + fmtExp(if (c.manualExp) c.expNs else c.liveExpNs)
        chips["EV"]?.text = "EV\n" + fmtEv(c)
        chips["FOCUS"]?.text = "FOCUS\n" + focusName(c)
        chips["WB"]?.text = "WB\n" + if (c.awbLock) "LOCKED" else "AUTO"
        val manual = when (sel) { "ISO", "SPEED" -> c.manualExp; "EV" -> c.ev != 0; "FOCUS" -> c.fm != CamControls.FM_AUTO; else -> c.awbLock }
        autoBtn.text = if (sel == "WB") (if (c.awbLock) "UNLOCK" else "LOCK WB") else "AUTO"
        autoBtn.background = bg(if (sel != "WB" && !manual) Y else Color.argb(90, 255, 255, 255), 14)
        autoBtn.setTextColor(if (sel != "WB" && !manual) Color.BLACK else Color.WHITE)
        valueTv.text = when (sel) {
            "ISO" -> "${if (c.manualExp) c.iso else c.liveIso}"
            "SPEED" -> fmtExp(if (c.manualExp) c.expNs else c.liveExpNs)
            "EV" -> fmtEv(c)
            "FOCUS" -> focusName(c)
            else -> if (c.awbLock) "LOCKED" else "AUTO"
        }
        // follow auto values while the user is not dragging
        if (!seek.isPressed && !manual && sel != "WB") sync()
    }
}
