package com.harman.usbcam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.widget.Switch
import android.widget.Toast
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject

/**
 * Phone screen: choose camera, resolution, frame rate and bitrate. The choice is saved and is what the PC viewer / OBS source
 * get when they ask for the default (width/height 0). Starts UsbCamService (camera, encoder, control channel).
 * Intent extras are still forwarded to the service, so `run.bat` keeps working:
 *   adb shell am start -n com.harman.usbcam/.MainActivity --ei w 1920 --ei h 1080 --ei fps 30 --ei br 12000000 [--es cam <id>] [--ez hdr true]
 */
class MainActivity : Activity() {
    private class CamEntry(val id: String, val label: String, val modes: JSONArray)

    private var capsJson: JSONObject? = null
    private val cams = ArrayList<CamEntry>()
    private var camIdx = 0
    private var modeIdx = 0
    private var fpsIdx = 0
    private var brIdx = 0
    private val brOptions = intArrayOf(0, 4, 6, 8, 12, 16, 20, 30, 40, 60) // Mbit/s, 0 = automatic

    private lateinit var statusTv: TextView
    private lateinit var guardTv: TextView
    private lateinit var hintTv: TextView
    private lateinit var dot: View
    private lateinit var titleTv: TextView
    private lateinit var summaryTv: TextView
    private lateinit var applyCard: LinearLayout
    private val presetBtns = ArrayList<Button>()
    private var dirty = false
    private lateinit var spCam: Spinner
    private lateinit var spMode: Spinner
    private lateinit var spFps: Spinner
    private lateinit var spBr: Spinner
    private lateinit var cbLl: Switch
    private lateinit var cbGuard: Switch

    private val ui = Handler(Looper.getMainLooper())
    private val tick = object : Runnable { override fun run() { refreshStatus(); ui.postDelayed(this, 1000) } }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        capsJson = try { Caps.build(this) } catch (_: Exception) { null }
        buildCams()
        restoreSelection()
        setContentView(buildUi())
        fillCam(); fillModes(); fillFps(); fillBr()
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            val p = if (Build.VERSION.SDK_INT >= 33) arrayOf(Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS)
                    else arrayOf(Manifest.permission.CAMERA)
            requestPermissions(p, 1)
        } else launch()
    }

    override fun onNewIntent(i: Intent) {
        super.onNewIntent(i)
        intent = i
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) launch()
    }

    override fun onRequestPermissionsResult(rc: Int, p: Array<out String>, r: IntArray) {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) launch()
        else { UsbCamService.status = "Camera permission denied"; refreshStatus() }
    }

    private fun launch() {
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_START).putExtras(intent))
    }

    override fun onResume() { super.onResume(); ui.post(tick) }
    override fun onPause() { ui.removeCallbacks(tick); super.onPause() }

    // ---- data ----

    private fun buildCams() {
        val arr = capsJson?.optJSONArray("cameras") ?: return
        for (i in 0 until arr.length()) {
            val c = arr.getJSONObject(i)
            val modes = c.getJSONArray("modes")
            if (modes.length() > 0) cams.add(CamEntry(c.getString("id"), "${c.getString("facing").replaceFirstChar { it.uppercase() }} camera (id ${c.getString("id")})", modes))
            val ph = c.optJSONArray("phys") ?: continue
            for (k in 0 until ph.length()) {
                val pc = ph.getJSONObject(k)
                val pm = pc.getJSONArray("modes")
                if (pm.length() > 0) cams.add(CamEntry(pc.getString("id"), "   ${c.getString("facing").replaceFirstChar { it.uppercase() }} lens ${"%.1f".format(pc.getDouble("focal"))} mm (id ${pc.getString("id")})", pm))
            }
        }
    }

    private fun curModes(): JSONArray = cams.getOrNull(camIdx)?.modes ?: JSONArray()

    private fun fpsList(): List<Int> {
        val m = curModes().optJSONObject(modeIdx) ?: return emptyList()
        val a = m.getJSONArray("fps")
        return (0 until a.length()).map { a.getInt(it) }
    }

    private fun restoreSelection() {
        val p = Prefs(this)
        val d = capsJson?.optJSONObject("default")
        val wantCam = if (p.hasChoice && p.cam.isNotEmpty()) p.cam else (d?.optString("cam") ?: "")
        camIdx = cams.indexOfFirst { it.id == wantCam }.let { if (it < 0) 0 else it }
        val ww = if (p.hasChoice) p.w else (d?.optInt("w") ?: 0)
        val hh = if (p.hasChoice) p.h else (d?.optInt("h") ?: 0)
        val modes = curModes()
        modeIdx = (0 until modes.length()).firstOrNull { modes.getJSONObject(it).getInt("w") == ww && modes.getJSONObject(it).getInt("h") == hh } ?: 0
        val wantFps = if (p.fps > 0) p.fps else (d?.optInt("fps") ?: 30)
        fpsIdx = fpsList().indexOf(wantFps).let { if (it < 0) 0 else it }
        brIdx = brOptions.indexOf(p.br / 1_000_000).let { if (it < 0) 0 else it }
    }

    private fun save() {
        val p = Prefs(this)
        val c = cams.getOrNull(camIdx) ?: return
        val m = curModes().optJSONObject(modeIdx) ?: return
        p.cam = c.id; p.w = m.getInt("w"); p.h = m.getInt("h")
        p.fps = fpsList().getOrElse(fpsIdx) { 30 }
        p.br = brOptions[brIdx] * 1_000_000
        p.ll = if (cbLl.isChecked) 1 else 0
        p.guard = cbGuard.isChecked
    }

    // ---- UI ----

    private val BG = Color.rgb(15, 17, 21); private val CARD = Color.rgb(26, 29, 36); private val TXT = Color.rgb(242, 244, 248)
    private val SUB = Color.rgb(154, 163, 178); private val ACC = Color.rgb(76, 141, 255); private val OKC = Color.rgb(61, 220, 132)
    private val WARN = Color.rgb(255, 183, 77); private val BAD = Color.rgb(255, 92, 92)

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun shape(fill: Int, r: Int = 16, stroke: Int = 0) = GradientDrawable().apply {
        setColor(fill); cornerRadius = dp(r).toFloat(); if (stroke != 0) setStroke(dp(1), stroke)
    }

    private fun tv(t: String, size: Float, color: Int = TXT, bold: Boolean = false) = TextView(this).apply {
        text = t; textSize = size; setTextColor(color); if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    private fun section(t: String) = tv(t.uppercase(), 12f, SUB, true).apply { letterSpacing = 0.08f; setPadding(dp(4), dp(20), 0, dp(8)) }

    private fun card(vararg v: View) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; background = shape(CARD); setPadding(dp(16), dp(14), dp(16), dp(14))
        for (x in v) addView(x)
    }

    private fun field(name: String, sp: Spinner) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; setPadding(0, dp(6), 0, dp(6))
        addView(tv(name, 13f, SUB))
        addView(sp, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(48)))
    }

    private fun switchRow(title: String, sub: String, sw: Switch) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; setPadding(0, dp(8), 0, dp(8))
        val txt = LinearLayout(this@MainActivity).apply { orientation = LinearLayout.VERTICAL; addView(tv(title, 15f)); addView(tv(sub, 12f, SUB)) }
        addView(txt, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)); addView(sw)
    }

    private fun btn(t: String, fill: Int, textColor: Int, stroke: Int = 0) = Button(this).apply {
        text = t; isAllCaps = false; textSize = 15f; setTextColor(textColor); background = shape(fill, 12, stroke); minimumHeight = dp(48)
    }

    private fun collapsible(title: String, content: View): View {
        content.visibility = View.GONE
        val h = tv("\u25B8  $title", 14f, ACC, true).apply { setPadding(dp(4), dp(14), 0, dp(10)); minimumHeight = dp(48); gravity = Gravity.CENTER_VERTICAL }
        h.setOnClickListener {
            val open = content.visibility != View.VISIBLE
            content.visibility = if (open) View.VISIBLE else View.GONE
            h.text = (if (open) "\u25BE  " else "\u25B8  ") + title
        }
        return LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; addView(h); addView(content) }
    }

    private fun pick(sp: Spinner, f: (Int) -> Unit) {
        sp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) { f(pos) }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
    }

    private fun changed() { save(); dirty = true; updateSummary() }

    private fun buildUi(): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(20), dp(16), dp(32)) }
        spCam = Spinner(this); spMode = Spinner(this); spFps = Spinner(this); spBr = Spinner(this)
        val p0 = Prefs(this)
        cbLl = Switch(this).apply { isChecked = p0.ll == 1; setOnCheckedChangeListener { _, _ -> changed() } }
        cbGuard = Switch(this).apply { isChecked = p0.guard; setOnCheckedChangeListener { _, _ -> save() } }

        // header + live status
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.BOTTOM
            addView(tv("UsbCam", 24f, TXT, true)); addView(tv("   v${UsbCamService.VERSION}", 12f, SUB).apply { setPadding(0, 0, 0, dp(3)) }) }
        dot = View(this).apply { background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(SUB) } }
        titleTv = tv("Starting...", 18f, TXT, true)
        val top = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            addView(dot, LinearLayout.LayoutParams(dp(12), dp(12)).apply { rightMargin = dp(10) }); addView(titleTv) }
        statusTv = tv("", 13f, SUB).apply { setPadding(0, dp(4), 0, dp(10)) }
        guardTv = tv("", 13f, SUB)
        hintTv = tv("", 12f, WARN).apply { setPadding(0, dp(4), 0, 0) }
        col.addView(header)
        col.addView(LinearLayout(this).apply { setPadding(0, dp(14), 0, 0) })
        col.addView(card(top, statusTv, guardTv))

        // quality presets
        col.addView(section("Quality"))
        val presets = listOf(Triple("Fast", "720p, least delay", 720), Triple("Balanced", "1080p, recommended", 1080), Triple("Sharp", "1440p, best picture", 1440))
        val prow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for ((n, d, h) in presets) {
            val b = btn("$n\n$d", CARD, TXT, Color.rgb(52, 57, 68)).apply { textSize = 13f }
            b.setOnClickListener { applyPreset(h) }
            presetBtns.add(b)
            prow.addView(b, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(3), 0, dp(3), 0) })
        }
        summaryTv = tv("", 13f, SUB).apply { setPadding(dp(4), dp(10), 0, 0) }
        col.addView(prow); col.addView(summaryTv)

        // fine tuning
        col.addView(section("Camera and detail"))
        col.addView(card(field("Camera", spCam), field("Resolution", spMode), hintTv, field("Frame rate", spFps)))

        // pending-changes bar
        val btnApply = btn("Restart stream with these settings", ACC, Color.WHITE).apply {
            setOnClickListener {
                save(); dirty = false; updateSummary()
                startForegroundService(Intent(this@MainActivity, UsbCamService::class.java).setAction(UsbCamService.ACTION_APPLY))
                Toast.makeText(this@MainActivity, "Settings applied", Toast.LENGTH_SHORT).show()
            } }
        applyCard = card(tv("You changed the settings", 14f, TXT, true), tv("They are saved. A running stream keeps the old ones until you restart it.", 12f, SUB).apply { setPadding(0, dp(2), 0, dp(10)) }, btnApply)
        applyCard.visibility = View.GONE
        col.addView(LinearLayout(this).apply { setPadding(0, dp(12), 0, 0); addView(applyCard, LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT) })

        // protection
        col.addView(section("Protection"))
        col.addView(card(switchRow("Protect the phone", "Halves the bitrate when warm; stops when hot or the battery is low", cbGuard)))

        // advanced
        val reset = btn("Reset to recommended settings", BG, ACC, Color.rgb(52, 57, 68)).apply {
            setOnClickListener {
                val p = Prefs(this@MainActivity); p.cam = ""; p.w = 0; p.h = 0; p.fps = 0; p.br = 0
                restoreSelection(); fillCam(); fillModes(); fillFps(); fillBr(); dirty = true; updateSummary()
            } }
        col.addView(collapsible("Advanced", card(field("Bitrate (picture data per second)", spBr),
            switchRow("Low-latency tuning", "Asks the encoder for the shortest delay", cbLl), reset)))
        col.addView(collapsible("Device info", card(tv(capsJson?.let { Caps.text(it) } ?: "Camera information is unavailable.", 11f, SUB).apply { typeface = Typeface.MONOSPACE })))

        col.addView(tv("The PC viewer and the OBS source use these settings unless you choose custom values on the PC.", 12f, SUB).apply { setPadding(dp(4), dp(12), dp(4), dp(16)) })
        col.addView(btn("Quit UsbCam", BG, BAD, BAD).apply { setOnClickListener {
            startService(Intent(this@MainActivity, UsbCamService::class.java).setAction(UsbCamService.ACTION_QUIT)) } },
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)

        pick(spCam) { pos -> if (pos != camIdx) { camIdx = pos; modeIdx = 0; fpsIdx = 0; fillModes(); fillFps(); changed() } }
        pick(spMode) { pos -> if (pos != modeIdx) { modeIdx = pos; fpsIdx = 0; fillFps(); changed() }; updateHint() }
        pick(spFps) { pos -> if (pos != fpsIdx) { fpsIdx = pos; changed() } }
        pick(spBr) { pos -> if (pos != brIdx) { brIdx = pos; changed() } }
        return ScrollView(this).apply { setBackgroundColor(BG); isFillViewport = true; addView(col) }
    }

    private fun applyPreset(targetH: Int) {
        val m = curModes(); if (m.length() == 0) return
        var best = 0
        for (i in 0 until m.length()) if (Math.abs(m.getJSONObject(i).getInt("h") - targetH) < Math.abs(m.getJSONObject(best).getInt("h") - targetH)) best = i
        modeIdx = best; fillModes()
        val f = fpsList(); fpsIdx = f.indexOf(30).let { if (it < 0) 0 else it }; fillFps()
        brIdx = 0; fillBr(); changed()
    }

    private fun adapter(items: List<String>) = object : ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item, items) {
        override fun getView(pos: Int, v: View?, parent: android.view.ViewGroup): View = super.getView(pos, v, parent).also { styleItem(it) }
        override fun getDropDownView(pos: Int, v: View?, parent: android.view.ViewGroup): View = super.getDropDownView(pos, v, parent).also { styleItem(it); (it as? TextView)?.minHeight = dp(48) }
        private fun styleItem(it: View) { (it as? TextView)?.apply { setTextColor(Color.WHITE); textSize = 15f; gravity = Gravity.CENTER_VERTICAL } }
    }

    private fun fillCam() { spCam.adapter = adapter(cams.map { it.label }); spCam.setSelection(camIdx, false) }

    private fun fillModes() {
        val m = curModes()
        val items = (0 until m.length()).map {
            val x = m.getJSONObject(it)
            val n = when (x.getString("note")) { "cropped" -> "  (zoomed in)"; "unverified" -> "  (untested)"; else -> "" }
            "${x.getString("label")}  \u00B7  ${x.getInt("w")}\u00D7${x.getInt("h")}$n"
        }
        spMode.adapter = adapter(items); spMode.setSelection(modeIdx, false); updateHint()
    }

    private fun fillFps() { spFps.adapter = adapter(fpsList().map { "$it frames per second" }); spFps.setSelection(fpsIdx, false) }

    private fun fillBr() {
        spBr.adapter = adapter(brOptions.map { if (it == 0) "Automatic (recommended)" else "$it Mbit/s" }); spBr.setSelection(brIdx, false)
    }

    private fun updateHint() {
        val n = curModes().optJSONObject(modeIdx)?.optString("note") ?: ""
        hintTv.text = when (n) {
            "cropped" -> "Zoomed in: on this phone this size shows only the middle part of the sensor."
            "unverified" -> "Untested on this phone model. It may be zoomed in or add delay."
            else -> ""
        }
        hintTv.visibility = if (hintTv.text.isEmpty()) View.GONE else View.VISIBLE
        updateSummary()
    }

    private fun updateSummary() {
        if (!::summaryTv.isInitialized) return
        val m = curModes().optJSONObject(modeIdx)
        val fps = fpsList().getOrNull(fpsIdx) ?: 30
        if (m != null) {
            val w = m.getInt("w"); val h = m.getInt("h")
            val mb = if (brOptions[brIdx] > 0) brOptions[brIdx] else Caps.bitrate(w, h, fps) / 1_000_000
            summaryTv.text = "Sending ${w}\u00D7${h} at $fps fps, about $mb Mbit/s"
            val hs = intArrayOf(720, 1080, 1440)
            for (i in presetBtns.indices) {
                val closest = (0 until curModes().length()).minByOrNull { Math.abs(curModes().getJSONObject(it).getInt("h") - hs[i]) }
                val on = closest == modeIdx && fps == 30 && brIdx == 0
                presetBtns[i].background = shape(if (on) Color.rgb(32, 58, 105) else CARD, 12, if (on) ACC else Color.rgb(52, 57, 68))
            }
        }
        applyCard.visibility = if (dirty) View.VISIBLE else View.GONE
    }

    private fun refreshStatus() {
        val s = UsbCamService
        val raw = s.status
        val low = raw.lowercase()
        val (title, color) = when {
            s.guardStage == 2 -> "Stopped to protect the phone" to BAD
            low.contains("denied") || low.contains("fail") || low.contains("error") -> "Problem" to BAD
            low.contains("stream") -> "Streaming to PC" to OKC
            low.contains("wait") || low.contains("listen") || low.contains("ready") -> "Ready, waiting for the PC" to ACC
            else -> "Starting..." to SUB
        }
        titleTv.text = title
        (dot.background as GradientDrawable).setColor(color)
        statusTv.text = raw
        val batt = if (s.battLevel >= 0) "${s.battLevel}%${if (s.plugged) " (charging)" else ""}" else "?"
        guardTv.text = "Battery $batt   \u00B7   ${"%.1f".format(s.battTemp)} \u00B0C" +
            (if (s.guardMsg.isNotEmpty()) "\n${s.guardMsg}" else "")
        guardTv.setTextColor(if (s.guardStage > 0) WARN else SUB)
    }
}
