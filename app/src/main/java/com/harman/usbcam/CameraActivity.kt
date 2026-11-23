package com.harman.usbcam

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PointF
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.accessibility.AccessibilityManager
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.TooltipCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import android.hardware.camera2.CameraMetadata
import com.google.android.material.chip.Chip
import com.google.android.material.slider.Slider
import com.google.android.material.snackbar.Snackbar
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Live camera preview with Material 3 controls in the style of the Samsung / Pixel camera:
 * tap = focus + meter there, long-press = AE/AF LOCK, PRO panel with ISO / SPEED / EV / FOCUS / WB.
 * The preview is a second output of the running camera session; it exists only while this screen is open.
 * Controls fade out after a few seconds without touch and come back on any touch (kept visible with TalkBack).
 */
class CameraActivity : AppCompatActivity() {
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var sv: SurfaceView
    private lateinit var holderLayout: AspectLayout
    private lateinit var ring: View
    private lateinit var statusTv: TextView
    private lateinit var lockBadge: MaterialButton
    private lateinit var lockBtn: MaterialButton
    private lateinit var proBtn: MaterialButton
    private lateinit var backBtn: MaterialButton
    private lateinit var rightCol: View
    private lateinit var panel: View
    private lateinit var slider: Slider
    private lateinit var valueTv: TextView
    private lateinit var autoBtn: MaterialButton
    private lateinit var focusBox: View
    private lateinit var focusHint: TextView
    private lateinit var modeGroup: MaterialButtonToggleGroup
    private val chips = LinkedHashMap<String, Chip>()
    private val modeBtns = LinkedHashMap<Int, MaterialButton>()
    private var sel = "ISO"
    private lateinit var wbBox: View
    private val wbChips = LinkedHashMap<Int, Chip>() // camera white-balance mode -> its preset chip
    private var attached = false
    private var dragging = false
    private var controlsShown = true
    private val hideRun = Runnable { hideControls() }
    private val tick = object : Runnable { override fun run() { update(); ui.postDelayed(this, 250) } }

    private val HIDE_MS = 4000L

    private fun ctl() = UsbCamService.instance?.controls
    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun ringBg(color: Int) = GradientDrawable().apply {
        setColor(Color.TRANSPARENT); cornerRadius = dp(6).toFloat(); setStroke(dp(2), color)
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        UiPrefs.saveCamOn(this, true) // opening the preview turns the camera back on (service does the same)
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_PREVIEW))
        setContentView(R.layout.activity_camera)
        bind()
        wire()
        sync()
        if (!UiPrefs.isHintSeen(this)) {
            UiPrefs.markHintSeen(this)
            Snackbar.make(findViewById(R.id.cam_root), R.string.hint_focus, 8000).setAction(R.string.got_it) { }.show()
        }
    }

    private fun bind() {
        holderLayout = findViewById(R.id.preview_frame)
        sv = findViewById(R.id.preview_surface)
        ring = findViewById(R.id.focus_ring)
        statusTv = findViewById(R.id.status_pill)
        lockBadge = findViewById(R.id.lock_badge)
        lockBtn = findViewById(R.id.btn_lock)
        proBtn = findViewById(R.id.btn_pro)
        backBtn = findViewById(R.id.btn_back)
        rightCol = findViewById(R.id.right_col)
        panel = findViewById(R.id.pro_panel)
        slider = findViewById(R.id.slider)
        valueTv = findViewById(R.id.value_tv)
        autoBtn = findViewById(R.id.btn_auto)
        focusBox = findViewById(R.id.focus_box)
        focusHint = findViewById(R.id.focus_hint)
        modeGroup = findViewById(R.id.focus_modes)
        chips["ISO"] = findViewById(R.id.chip_iso); chips["SPEED"] = findViewById(R.id.chip_speed); chips["EV"] = findViewById(R.id.chip_ev)
        chips["FOCUS"] = findViewById(R.id.chip_focus); chips["WB"] = findViewById(R.id.chip_wb)
        modeBtns[CamControls.FM_AUTO] = findViewById(R.id.fm_auto); modeBtns[CamControls.FM_SINGLE] = findViewById(R.id.fm_single)
        modeBtns[CamControls.FM_INF] = findViewById(R.id.fm_inf); modeBtns[CamControls.FM_LOCK] = findViewById(R.id.fm_lock)
        modeBtns[CamControls.FM_MANUAL] = findViewById(R.id.fm_manual)
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun wire() {
        sv.holder.setFixedSize(1280, 720)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { attached = UsbCamService.instance?.let { it.setPreview(h.surface); true } ?: false }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { attached = false; UsbCamService.instance?.setPreview(null) }
        })
        val gd = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onSingleTapUp(e: MotionEvent): Boolean { focusAt(e.x, e.y); return true }
            override fun onLongPress(e: MotionEvent) { lockAt(e.x, e.y) }
        })
        holderLayout.setOnTouchListener { _, e -> gd.onTouchEvent(e) }

        backBtn.setOnClickListener { finish() }
        findViewById<MaterialButton>(R.id.btn_flip).setOnClickListener { UsbCamService.instance?.flipCamera() }
        lockBtn.setOnClickListener { toggleLock() }
        proBtn.setOnClickListener { panel.visibility = if (panel.visibility == View.VISIBLE) View.GONE else View.VISIBLE; update() }
        lockBadge.setOnClickListener { ctl()?.unlockAeAf(); push(); update() }
        for (id in intArrayOf(R.id.btn_back, R.id.btn_flip, R.id.btn_lock, R.id.btn_pro)) {
            val v = findViewById<View>(id); TooltipCompat.setTooltipText(v, v.contentDescription)
        }

        for ((n, c) in chips) c.setOnClickListener { sel = n; sync(); update() }
        wbBox = findViewById(R.id.wb_box)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_AUTO] = findViewById(R.id.wb_auto)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT] = findViewById(R.id.wb_daylight)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT] = findViewById(R.id.wb_cloudy)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_SHADE] = findViewById(R.id.wb_shade)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT] = findViewById(R.id.wb_tungsten)
        wbChips[CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT] = findViewById(R.id.wb_fluor)
        for ((mode, chip) in wbChips) chip.setOnClickListener {
            val cc = ctl() ?: return@setOnClickListener
            cc.awbMode = mode; cc.awbLock = false // picking a preset releases the white-balance lock
            push(); sync(); update()
        }
        autoBtn.setOnClickListener { auto() }
        slider.addOnChangeListener { _, v, fromUser -> if (fromUser) slide(v.roundToInt()) }
        slider.addOnSliderTouchListener(object : Slider.OnSliderTouchListener {
            override fun onStartTrackingTouch(s: Slider) { dragging = true }
            override fun onStopTrackingTouch(s: Slider) { dragging = false }
        })
        slider.setLabelFormatter { _ -> valueTv.text.toString() }
        for ((m, btn) in modeBtns) btn.setOnClickListener {
            val c = ctl() ?: return@setOnClickListener
            c.chooseFocus(m); if (m == CamControls.FM_MANUAL) c.focusDist = c.liveFocus
            push(); if (m == CamControls.FM_SINGLE) UsbCamService.instance?.triggerAf()
            sync(); update()
        }
    }

    override fun onStart() { super.onStart(); ui.post(tick); showControls() }
    override fun onStop() {
        ui.removeCallbacks(tick); ui.removeCallbacks(hideRun)
        UsbCamService.instance?.setPreview(null); attached = false; super.onStop()
    }
    override fun onResume() {
        super.onResume()
        // the service may start a moment after this screen: attach once it exists
        if (!attached && sv.holder.surface.isValid) attached = UsbCamService.instance?.let { it.setPreview(sv.holder.surface); true } ?: false
    }

    // ---- auto-hide ----

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (ev.actionMasked == MotionEvent.ACTION_DOWN) showControls()
        return super.dispatchTouchEvent(ev)
    }

    private fun touchExploration(): Boolean =
        (getSystemService(Context.ACCESSIBILITY_SERVICE) as? AccessibilityManager)?.isTouchExplorationEnabled == true

    private fun showControls() {
        if (!controlsShown) {
            controlsShown = true
            for (v in listOf(backBtn, rightCol)) { v.animate().cancel(); v.visibility = View.VISIBLE; v.animate().alpha(1f).setDuration(150).start() }
        }
        ui.removeCallbacks(hideRun)
        if (!touchExploration()) ui.postDelayed(hideRun, HIDE_MS)
    }

    private fun hideControls() {
        if (panel.visibility == View.VISIBLE || dragging) { ui.postDelayed(hideRun, HIDE_MS); return }
        controlsShown = false
        for (v in listOf(backBtn, rightCol)) v.animate().alpha(0f).setDuration(150).withEndAction { if (!controlsShown) v.visibility = View.INVISIBLE }.start()
    }

    private fun push() { UsbCamService.instance?.applyControls() }

    // ---- gestures ----
    private fun showRing(x: Float, y: Float, color: Int) {
        ring.background = ringBg(color)
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
        showRing(x, y, getColor(R.color.cam_lock)); push()
        ui.postDelayed({ c.lockAeAf(); push(); update() }, 700)   // let AF settle on the point first
    }

    private fun toggleLock() {
        val c = ctl() ?: return
        if (c.locked) { c.unlockAeAf(); ring.visibility = View.GONE } else c.lockAeAf()
        push(); update()
    }

    // ---- pro panel (logic unchanged) ----
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
            "EV" -> {
                // Brightness only works with automatic exposure: leave manual ISO/shutter and release the exposure lock
                c.manualExp = false; c.aeLock = false
                c.ev = c.evMin + p
            }
            "FOCUS" -> { if (c.minFocus <= 0f) { Toast.makeText(this, R.string.toast_fixed_focus, Toast.LENGTH_SHORT).show(); return }
                c.chooseFocus(CamControls.FM_MANUAL); c.focusDist = c.minFocus * (1f - p / 100f) }
        }
        push(); update()
    }

    private fun noManual() = Toast.makeText(this, R.string.toast_no_manual, Toast.LENGTH_SHORT).show()

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

    /** Puts the slider and the chip / focus-mode selection on the current value of the selected control. */
    private fun sync() {
        val c = ctl()
        for ((n, v) in chips) v.isChecked = (n == sel)
        slider.visibility = if (sel == "WB") View.INVISIBLE else View.VISIBLE
        wbBox.visibility = if (sel == "WB") View.VISIBLE else View.GONE
        focusBox.visibility = if (sel == "FOCUS") View.VISIBLE else View.GONE
        if (c == null) return
        modeBtns[c.fm]?.let { modeGroup.check(it.id) }
        for ((m, ch) in wbChips) { ch.visibility = if (m == CameraMetadata.CONTROL_AWB_MODE_AUTO || c.awbModes.contains(m)) View.VISIBLE else View.GONE; ch.isChecked = (m == c.awbMode) }
        focusHint.setText(when (c.fm) {
            CamControls.FM_SINGLE -> R.string.fm_single_hint; CamControls.FM_INF -> R.string.fm_inf_hint
            CamControls.FM_LOCK -> R.string.fm_lock_hint; CamControls.FM_MANUAL -> R.string.fm_manual_hint; else -> R.string.fm_auto_hint })
        when (sel) {
            "ISO" -> putSlider(100, toLog((if (c.manualExp) c.iso else c.liveIso).toDouble(), c.isoMin.toDouble(), c.isoMax.toDouble()))
            "SPEED" -> putSlider(100, toLog((if (c.manualExp) c.expNs else c.liveExpNs).toDouble(), expLo(c), expHi(c)))
            "EV" -> putSlider(Math.max(1, c.evMax - c.evMin), c.ev - c.evMin)
            "FOCUS" -> putSlider(100, if (c.minFocus > 0f) ((1f - (when (c.fm) { CamControls.FM_INF -> 0f; CamControls.FM_MANUAL, CamControls.FM_LOCK -> c.focusDist; else -> c.liveFocus }) / c.minFocus) * 100).roundToInt().coerceIn(0, 100) else 100)
        }
    }

    private fun putSlider(max: Int, progress: Int) {
        val to = Math.max(1, max).toFloat()
        if (slider.valueTo != to) slider.valueTo = to
        slider.value = progress.coerceIn(0, to.toInt()).toFloat()
    }

    private fun autoWord() = getString(R.string.val_auto)

    private fun wbName(c: CamControls): String = if (c.awbLock) getString(R.string.val_locked) else when (c.awbMode) {
        CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT -> getString(R.string.wb_daylight)
        CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT -> getString(R.string.wb_cloudy)
        CameraMetadata.CONTROL_AWB_MODE_SHADE -> getString(R.string.wb_shade)
        CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT -> getString(R.string.wb_tungsten)
        CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT -> getString(R.string.wb_fluor)
        else -> autoWord()
    }

    private fun focusName(c: CamControls) = when (c.fm) {
        CamControls.FM_SINGLE -> getString(R.string.val_single); CamControls.FM_INF -> "\u221E"
        CamControls.FM_LOCK -> getString(R.string.val_locked_at, fmtFocus(c.focusDist))
        CamControls.FM_MANUAL -> fmtFocus(c.focusDist); else -> autoWord()
    }

    private fun fmtExp(ns: Long): String { if (ns <= 0) return "-"; val d = 1e9 / ns; return if (d >= 2) "1/${d.roundToInt()}" else "%.1fs".format(ns / 1e9) }
    private fun fmtFocus(d: Float) = if (d < 0.05f) "\u221E" else "%.2f m".format(1f / d)
    private fun fmtEv(c: CamControls): String { val v = c.ev * c.evStep; return (if (v > 0) "+" else "") + "%.1f".format(v) }

    private fun putText(tv: TextView, t: String) { if (tv.text.toString() != t) tv.text = t }

    /** Chip shows icon + value only; the full "Brightness +0.3" text is the spoken description. */
    private fun putChip(c: Chip, value: String, descRes: Int) {
        putText(c, value)
        val d = getString(descRes, value)
        if (c.contentDescription?.toString() != d) c.contentDescription = d
    }

    private fun update() {
        val s = UsbCamService.instance
        val c = s?.controls
        val live = s?.clientOn() == true
        val info = if (s == null) getString(R.string.pill_starting) else s.infoNow().ifEmpty { getString(R.string.pill_starting_info) }
        putText(statusTv, if (s == null) info else info + "  ·  " + getString(if (live) R.string.pill_live else R.string.pill_waiting))
        statusTv.setBackgroundResource(if (live) R.drawable.bg_pill_live else R.drawable.bg_pill_idle)
        if (c == null) return
        lockBadge.visibility = if (c.locked) View.VISIBLE else View.GONE
        lockBadge.setText(if (c.aeLock && c.afLock) R.string.badge_lock else if (c.aeLock) R.string.badge_ae else R.string.badge_af)
        lockBtn.isChecked = c.locked
        proBtn.isChecked = panel.visibility == View.VISIBLE
        if (!c.locked && c.point == null && ring.visibility == View.VISIBLE) ring.visibility = View.GONE
        if (panel.visibility != View.VISIBLE) return
        val isoV = if (c.manualExp) "${c.iso}" else autoWord() + " ${c.liveIso}"
        putChip(chips["ISO"]!!, isoV, R.string.pro_iso)
        putChip(chips["SPEED"]!!, fmtExp(if (c.manualExp) c.expNs else c.liveExpNs), R.string.pro_speed)
        putChip(chips["EV"]!!, fmtEv(c), R.string.pro_ev)
        putChip(chips["FOCUS"]!!, focusName(c), R.string.pro_focus)
        putChip(chips["WB"]!!, wbName(c), R.string.pro_wb)
        val manual = when (sel) { "ISO", "SPEED" -> c.manualExp; "EV" -> c.ev != 0; "FOCUS" -> c.fm != CamControls.FM_AUTO; else -> c.awbLock }
        autoBtn.setText(if (sel == "WB") (if (c.awbLock) R.string.btn_unlock_wb else R.string.btn_lock_wb) else R.string.btn_auto)
        autoBtn.isChecked = if (sel == "WB") c.awbLock else !manual
        putText(valueTv, when (sel) {
            "ISO" -> "${if (c.manualExp) c.iso else c.liveIso}"
            "SPEED" -> fmtExp(if (c.manualExp) c.expNs else c.liveExpNs)
            "EV" -> fmtEv(c)
            "FOCUS" -> focusName(c)
            else -> wbName(c)
        })
        // follow auto values while the user is not dragging
        if (!dragging && !manual && sel != "WB") sync()
    }
}
