package com.harman.usbcam

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.ViewCompat
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.color.DynamicColors
import org.json.JSONArray
import org.json.JSONObject

/** Plain-language device information. Technical text is one tap away and can be copied for support. */
class DeviceInfoActivity : AppCompatActivity() {
    private var techText = ""

    override fun onCreate(b: Bundle?) {
        AppCompatDelegate.setDefaultNightMode(UiPrefs.nightMode(this))
        if (UiPrefs.isDynamic(this)) DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(b)
        setContentView(R.layout.activity_device_info)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }
        val box = findViewById<LinearLayout>(R.id.info_container)
        val s = UsbCamService
        val p = Prefs(this)
        val caps: JSONObject? = try { Caps.build(this) } catch (_: Exception) { null }
        val yes = getString(R.string.di_yes); val no = getString(R.string.di_no)

        addCard(box, R.string.di_phone, listOf(
            getString(R.string.di_model) to (Build.MANUFACTURER.replaceFirstChar { it.uppercase() } + " " + Build.MODEL),
            getString(R.string.di_android) to Build.VERSION.RELEASE,
            getString(R.string.di_app) to UsbCamService.VERSION))

        val heat = when { s.thermal <= 1 -> R.string.heat_normal; s.thermal == 2 -> R.string.heat_warm; else -> R.string.heat_hot }
        addCard(box, R.string.di_power, listOf(
            getString(R.string.di_battery) to (if (s.battLevel >= 0) getString(R.string.di_batt_value, s.battLevel) else getString(R.string.di_unknown)),
            getString(R.string.di_charging) to (if (s.plugged) yes else no),
            getString(R.string.di_temp) to getString(R.string.di_temp_value, s.battTemp),
            getString(R.string.di_heat) to getString(heat),
            getString(R.string.di_protect) to getString(if (p.guard) R.string.di_on else R.string.di_off)))

        val sending = if (p.hasChoice) getString(R.string.di_sending_value, p.w, p.h, if (p.fps > 0) p.fps else 30) else getString(R.string.di_auto)
        addCard(box, R.string.di_conn, listOf(
            getString(R.string.di_pc) to (if (s.instance?.clientOn() == true) yes else no),
            getString(R.string.di_sending) to sending))

        val camRows = ArrayList<Pair<String, String>>()
        val arr = caps?.optJSONArray("cameras") ?: JSONArray()
        for (i in 0 until arr.length()) {
            val c = arr.getJSONObject(i)
            val face = facing(c.getString("facing"))
            val m = c.getJSONArray("modes")
            if (m.length() > 0) camRows.add(getString(R.string.di_cam_name, face) to camValue(m))
            val ph = c.optJSONArray("phys") ?: continue
            for (k in 0 until ph.length()) {
                val pc = ph.getJSONObject(k); val pm = pc.getJSONArray("modes")
                if (pm.length() > 0) camRows.add(getString(R.string.di_lens_name, face, "%.1f".format(pc.getDouble("focal"))) to camValue(pm))
            }
        }
        if (camRows.isNotEmpty()) addCard(box, R.string.di_cams, camRows)

        techText = (caps?.let { Caps.text(it) } ?: getString(R.string.info_unavailable)) + "\n\n" + getString(R.string.info_status) + ": " + s.status +
            (if (s.guardMsg.isNotEmpty()) "\n" + s.guardMsg else "")
        findViewById<TextView>(R.id.tech_text).text = techText
        val body = findViewById<View>(R.id.tech_body); val arrow = findViewById<ImageView>(R.id.tech_arrow); val head = findViewById<View>(R.id.tech_header)
        ViewCompat.setStateDescription(head, getString(R.string.state_collapsed))
        head.setOnClickListener {
            val o = body.visibility != View.VISIBLE
            body.visibility = if (o) View.VISIBLE else View.GONE
            arrow.rotation = if (o) 180f else 0f
            ViewCompat.setStateDescription(head, getString(if (o) R.string.state_expanded else R.string.state_collapsed))
        }
        findViewById<View>(R.id.btn_copy).setOnClickListener {
            (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("UsbCam", techText))
            Toast.makeText(this, R.string.copied, Toast.LENGTH_SHORT).show()
        }
    }

    private fun facing(f: String): String = when (f) {
        "back" -> getString(R.string.facing_back)
        "front" -> getString(R.string.facing_front)
        else -> f.replaceFirstChar { it.uppercase() }
    }

    /** e.g. "720p, 1080p, 4K (zoomed in) · up to 30 frames per second" */
    private fun camValue(modes: JSONArray): String {
        val names = ArrayList<String>(); var maxFps = 0
        for (i in 0 until modes.length()) {
            val x = modes.getJSONObject(i)
            val n = x.getString("label") + if (x.getString("note") == "cropped") getString(R.string.note_zoomed_item) else ""
            if (!names.contains(n)) names.add(n)
            val f = x.getJSONArray("fps"); for (k in 0 until f.length()) maxFps = Math.max(maxFps, f.getInt(k))
        }
        return getString(R.string.di_cam_value, names.joinToString(", "), maxFps)
    }

    private fun addCard(box: LinearLayout, titleRes: Int, rows: List<Pair<String, String>>) {
        val inf = LayoutInflater.from(this)
        val card = inf.inflate(R.layout.item_info_card, box, false)
        card.findViewById<TextView>(R.id.card_title).setText(titleRes)
        val list = card.findViewById<LinearLayout>(R.id.card_rows)
        for ((l, v) in rows) {
            val r = inf.inflate(R.layout.item_info_row, list, false)
            r.findViewById<TextView>(R.id.row_label).text = l
            r.findViewById<TextView>(R.id.row_value).text = v
            list.addView(r)
        }
        box.addView(card, box.indexOfChild(findViewById(R.id.tech_header))) // before the "Technical details" header
    }
}
