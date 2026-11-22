package com.harman.usbcam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
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
    private lateinit var spCam: Spinner
    private lateinit var spMode: Spinner
    private lateinit var spFps: Spinner
    private lateinit var spBr: Spinner
    private lateinit var cbLl: CheckBox
    private lateinit var cbGuard: CheckBox

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
            if (modes.length() > 0) cams.add(CamEntry(c.getString("id"), "cam ${c.getString("id")} (${c.getString("facing")}, ${"%.1f".format(c.getDouble("focal"))} mm)", modes))
            val ph = c.optJSONArray("phys") ?: continue
            for (k in 0 until ph.length()) {
                val pc = ph.getJSONObject(k)
                val pm = pc.getJSONArray("modes")
                if (pm.length() > 0) cams.add(CamEntry(pc.getString("id"), "   lens ${pc.getString("id")} (${"%.1f".format(pc.getDouble("focal"))} mm)", pm))
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

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun label(t: String) = TextView(this).apply { text = t; textSize = 12f; setTextColor(Color.LTGRAY); setPadding(0, dp(12), 0, 0) }

    private fun pick(sp: Spinner, f: (Int) -> Unit) {
        sp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) { f(pos) }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
    }

    private fun buildUi(): View {
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(16), dp(16), dp(24)) }
        statusTv = TextView(this).apply { textSize = 13f; setTextColor(Color.WHITE) }
        guardTv = TextView(this).apply { textSize = 12f; setTextColor(Color.rgb(255, 200, 100)) }
        hintTv = TextView(this).apply { textSize = 11f; setTextColor(Color.rgb(255, 160, 120)) }
        spCam = Spinner(this); spMode = Spinner(this); spFps = Spinner(this); spBr = Spinner(this)
        cbLl = CheckBox(this).apply { text = "Low-latency encoder tuning"; setTextColor(Color.WHITE); isChecked = Prefs(this@MainActivity).ll == 1
            setOnCheckedChangeListener { _, _ -> save() } }
        cbGuard = CheckBox(this).apply { text = "Protect phone (halve bitrate when warm, stop when hot or battery low)"; setTextColor(Color.WHITE); isChecked = Prefs(this@MainActivity).guard
            setOnCheckedChangeListener { _, _ -> save() } }

        pick(spCam) { pos -> if (pos != camIdx) { camIdx = pos; modeIdx = 0; fpsIdx = 0; fillModes(); fillFps(); save() } }
        pick(spMode) { pos -> if (pos != modeIdx) { modeIdx = pos; fpsIdx = 0; fillFps(); save() } ; updateHint() }
        pick(spFps) { pos -> if (pos != fpsIdx) { fpsIdx = pos; save() } }
        pick(spBr) { pos -> if (pos != brIdx) { brIdx = pos; save() } }

        val btnApply = Button(this).apply { text = "Apply to running stream"
            setOnClickListener { save(); startForegroundService(Intent(this@MainActivity, UsbCamService::class.java).setAction(UsbCamService.ACTION_APPLY)) } }
        val reset = Button(this).apply { text = "Reset to default"
            setOnClickListener {
                val p = Prefs(this@MainActivity); p.cam = ""; p.w = 0; p.h = 0; p.fps = 0; p.br = 0
                restoreSelection(); fillCam(); fillModes(); fillFps(); fillBr()
            } }
        val quit = Button(this).apply { text = "Stop service"
            setOnClickListener { startService(Intent(this@MainActivity, UsbCamService::class.java).setAction(UsbCamService.ACTION_QUIT)) } }
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for (b in listOf(btnApply, reset, quit)) row.addView(b, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        col.addView(statusTv); col.addView(guardTv)
        col.addView(label("Camera / lens")); col.addView(spCam)
        col.addView(label("Resolution")); col.addView(spMode); col.addView(hintTv)
        col.addView(label("Frame rate")); col.addView(spFps)
        col.addView(label("Bitrate")); col.addView(spBr)
        col.addView(cbLl); col.addView(cbGuard)
        col.addView(row)
        col.addView(TextView(this).apply {
            textSize = 11f; setTextColor(Color.GRAY); setPadding(0, dp(12), 0, 0)
            text = "Your choice is used when the PC viewer / OBS source asks for the default (size 0). Changes apply to the next start; " +
                   "tap \"Apply to running stream\" to restart a running one.\n\n" + (capsJson?.let { Caps.text(it) } ?: "caps unavailable")
        })
        return ScrollView(this).apply { setBackgroundColor(Color.BLACK); addView(col) }
    }

    private fun adapter(items: List<String>) = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, items)

    private fun fillCam() { spCam.adapter = adapter(cams.map { it.label }); spCam.setSelection(camIdx, false) }

    private fun fillModes() {
        val m = curModes()
        val items = (0 until m.length()).map {
            val x = m.getJSONObject(it); val n = x.getString("note")
            "${x.getString("label")}   ${x.getInt("w")}x${x.getInt("h")}${if (n.isEmpty()) "" else "   [$n]"}"
        }
        spMode.adapter = adapter(items); spMode.setSelection(modeIdx, false); updateHint()
    }

    private fun fillFps() { spFps.adapter = adapter(fpsList().map { "$it fps" }); spFps.setSelection(fpsIdx, false) }

    private fun fillBr() {
        spBr.adapter = adapter(brOptions.map { if (it == 0) "Auto (about 0.2 bit/pixel)" else "$it Mbit/s" }); spBr.setSelection(brIdx, false)
    }

    private fun updateHint() {
        val n = curModes().optJSONObject(modeIdx)?.optString("note") ?: ""
        hintTv.text = when (n) {
            "cropped" -> "Cropped: on this phone this size shows a zoomed-in part of the sensor."
            "unverified" -> "Unverified: not measured on this model. It may be cropped or add delay."
            else -> ""
        }
    }

    private fun refreshStatus() {
        val s = UsbCamService
        statusTv.text = "UsbCam ${UsbCamService.VERSION}\n${s.status}"
        val batt = if (s.battLevel >= 0) "${s.battLevel}%${if (s.plugged) " charging" else ""}" else "?"
        guardTv.text = "Battery $batt  |  ${"%.1f".format(s.battTemp)} C  |  thermal ${s.thermal}" +
            (if (s.guardMsg.isNotEmpty()) "\nGuard: ${s.guardMsg}" else "")
    }
}
