package com.harman.usbcam

import android.Manifest
import android.content.DialogInterface
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Rect
import android.graphics.drawable.GradientDrawable
import androidx.core.graphics.ColorUtils
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Filter
import android.widget.ImageView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.view.ViewCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.card.MaterialCardView
import com.google.android.material.chip.Chip
import com.google.android.material.color.DynamicColors
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.floatingactionbutton.ExtendedFloatingActionButton
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.snackbar.Snackbar
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import org.json.JSONArray
import org.json.JSONObject
import com.google.android.material.R as MR

/**
 * Phone screen (Material 3): status, camera on/off, quality presets, camera / size / frame rate, advanced and device info.
 * The choice is saved in [Prefs] and is what the PC viewer / OBS source get when they ask for the default.
 * Starts UsbCamService. Intent extras are still forwarded to the service, so `run.bat` keeps working:
 *   adb shell am start -n com.harman.usbcam/.MainActivity --ei w 1920 --ei h 1080 --ei fps 30 --ei br 12000000 [--es cam <id>] [--ez hdr true]
 */
class MainActivity : AppCompatActivity() {
    private class CamEntry(val id: String, val label: String, val modes: JSONArray)

    /** Drop-down adapter that always shows every item (the default one filters by the text in the field). */
    private class AllItemsAdapter(ctx: android.content.Context, private val src: List<String>) :
        ArrayAdapter<String>(ctx, R.layout.item_dropdown, src) {
        override fun getFilter(): Filter = object : Filter() {
            override fun performFiltering(c: CharSequence?): FilterResults = FilterResults().apply { values = src; count = src.size }
            override fun publishResults(c: CharSequence?, r: FilterResults?) { notifyDataSetChanged() }
        }
    }

    private class UiState(val title: Int, val text: Int, val icon: Int, val bg: Int, val fg: Int, val action: Int)

    private var capsJson: JSONObject? = null
    private val cams = ArrayList<CamEntry>()
    private var camIdx = 0
    private var modeIdx = 0
    private var fpsIdx = 0
    private var brIdx = 0
    private val brOptions = intArrayOf(0, 4, 6, 8, 12, 16, 20, 30, 40, 60) // Mbit/s, 0 = automatic
    private var dirty = false
    private var syncing = false
    private var advOpen = false
    private var picOpen = false
    private var pending: Snackbar? = null
    private var action = 0 // what the status-card button does: 1 = open app settings, 2 = try again

    private lateinit var coord: View
    private lateinit var fab: ExtendedFloatingActionButton
    private lateinit var statusCard: MaterialCardView
    private lateinit var statusIcon: ImageView
    private lateinit var statusContent: View
    private var gradKey = 0L
    private var lastIcon = 0
    private lateinit var statusTitle: TextView
    private lateinit var statusText: TextView
    private lateinit var chipBattery: TextView
    private lateinit var chipTemp: TextView
    private lateinit var statusAction: MaterialButton
    private lateinit var swCam: MaterialSwitch
    private lateinit var camSupport: TextView
    private lateinit var presetGroup: MaterialButtonToggleGroup
    private lateinit var summaryTv: TextView
    private lateinit var ddCam: MaterialAutoCompleteTextView
    private lateinit var ddRes: MaterialAutoCompleteTextView
    private lateinit var ddFps: MaterialAutoCompleteTextView
    private lateinit var ddBr: MaterialAutoCompleteTextView
    private lateinit var chipZoomed: Chip
    private lateinit var chipUntested: Chip
    private lateinit var swGuard: MaterialSwitch
    private lateinit var swLl: MaterialSwitch
    private lateinit var swDyn: MaterialSwitch
    private lateinit var btnUpdate: MaterialButton
    private var checking = false
    private var pendingUpdate: Updater.Info? = null

    private val ui = Handler(Looper.getMainLooper())
    private val tick = object : Runnable { override fun run() { refreshStatus(); ui.postDelayed(this, 1000) } }

    companion object {
        @Volatile private var capsCache: JSONObject? = null
        @Volatile private var lastAutoCheck = 0L
    }

    override fun onCreate(b: Bundle?) {
        AppCompatDelegate.setDefaultNightMode(UiPrefs.nightMode(this))
        if (UiPrefs.isDynamic(this)) DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)
        if (b != null) { dirty = b.getBoolean("dirty"); advOpen = b.getBoolean("adv"); picOpen = b.getBoolean("pic") }

        capsJson = capsCache ?: (try { Caps.build(this) } catch (_: Exception) { null }).also { capsCache = it }
        buildCams()
        restoreSelection()
        bindViews()
        wireViews()
        fillCam(); fillModes(); fillFps(); fillBr()
        refreshStatus() // paint the real state right away (also after rotation)

        if (b == null) { // not after a rotation: the service is already running and must not get the launch extras again
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                val p = if (Build.VERSION.SDK_INT >= 33) arrayOf(Manifest.permission.CAMERA, Manifest.permission.POST_NOTIFICATIONS)
                        else arrayOf(Manifest.permission.CAMERA)
                requestPermissions(p, 1)
            } else launch()
        }
    }

    override fun onSaveInstanceState(o: Bundle) {
        super.onSaveInstanceState(o)
        o.putBoolean("dirty", dirty); o.putBoolean("adv", advOpen); o.putBoolean("pic", picOpen)
    }

    /** After a theme change the screen is rebuilt and Android gives the old focus back to a drop-down, which then opens. Drop that focus. */
    override fun onRestoreInstanceState(s: Bundle) {
        super.onRestoreInstanceState(s)
        releaseDropdownFocus()
    }

    /** A tap anywhere outside the focused drop-down un-selects it. */
    override fun dispatchTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) {
            val f = currentFocus
            if (f is MaterialAutoCompleteTextView) {
                val r = Rect()
                f.getGlobalVisibleRect(r)
                if (!r.contains(e.rawX.toInt(), e.rawY.toInt())) releaseDropdownFocus()
            }
        }
        return super.dispatchTouchEvent(e)
    }

    private fun releaseDropdownFocus() {
        if (!::ddCam.isInitialized) return
        for (d in arrayOf(ddCam, ddRes, ddFps, ddBr)) {
            if (d.isPopupShowing) d.dismissDropDown()
            if (d.hasFocus()) d.clearFocus()
        }
    }

    override fun onNewIntent(i: Intent) {
        super.onNewIntent(i)
        intent = i
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) launch()
    }

    override fun onRequestPermissionsResult(rc: Int, p: Array<out String>, r: IntArray) {
        super.onRequestPermissionsResult(rc, p, r)
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) launch()
        refreshStatus()
    }

    private fun launch() {
        if (!UiPrefs.isCamOn(this)) {
            // The camera switch was left off: keep it off even though the PC tools relaunched the app (no stream extras are forwarded).
            startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_START))
            startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_CAM_OFF))
            return
        }
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_START).putExtras(intent))
    }

    override fun onStart() {
        super.onStart()
        // look for a new version whenever the app is opened (at most every 30 minutes)
        val now = SystemClock.elapsedRealtime()
        if (lastAutoCheck == 0L || now - lastAutoCheck > 30 * 60 * 1000L) { lastAutoCheck = now; checkForUpdate(false) }
    }

    override fun onResume() {
        super.onResume(); lastIcon = 0; ui.post(tick)
        if (dirty) showPending()
        val pu = pendingUpdate
        if (pu != null && Updater.canInstall(this)) { pendingUpdate = null; downloadAndInstall(pu) } // back from the "allow installs" settings page
    }
    override fun onPause() { ui.removeCallbacks(tick); super.onPause() }

    // ---- data (unchanged logic) ----

    private fun facingName(f: String): String = when (f) {
        "back" -> getString(R.string.facing_back)
        "front" -> getString(R.string.facing_front)
        else -> f.replaceFirstChar { it.uppercase() }
    }

    private fun buildCams() {
        val arr = capsJson?.optJSONArray("cameras") ?: return
        for (i in 0 until arr.length()) {
            val c = arr.getJSONObject(i)
            val modes = c.getJSONArray("modes")
            val face = facingName(c.getString("facing"))
            if (modes.length() > 0) cams.add(CamEntry(c.getString("id"), getString(R.string.cam_label, face, c.getString("id")), modes))
            val ph = c.optJSONArray("phys") ?: continue
            for (k in 0 until ph.length()) {
                val pc = ph.getJSONObject(k)
                val pm = pc.getJSONArray("modes")
                if (pm.length() > 0) cams.add(CamEntry(pc.getString("id"), "   " + getString(R.string.lens_label, face, "%.1f".format(pc.getDouble("focal")), pc.getString("id")), pm))
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
        p.ll = if (swLl.isChecked) 1 else 0
        p.guard = swGuard.isChecked
    }

    // ---- views ----

    private fun bindViews() {
        coord = findViewById(R.id.coord)
        fab = findViewById(R.id.fab_preview)
        statusCard = findViewById(R.id.status_card)
        statusIcon = findViewById(R.id.status_icon)
        statusContent = findViewById(R.id.status_content)
        statusTitle = findViewById(R.id.status_title)
        statusText = findViewById(R.id.status_text)
        chipBattery = findViewById(R.id.chip_battery)
        chipTemp = findViewById(R.id.chip_temp)
        statusAction = findViewById(R.id.status_action)
        swCam = findViewById(R.id.sw_cam)
        camSupport = findViewById(R.id.cam_support)
        presetGroup = findViewById(R.id.preset_group)
        summaryTv = findViewById(R.id.preset_summary)
        ddCam = findViewById(R.id.dd_cam); ddRes = findViewById(R.id.dd_res); ddFps = findViewById(R.id.dd_fps); ddBr = findViewById(R.id.dd_br)
        chipZoomed = findViewById(R.id.chip_zoomed); chipUntested = findViewById(R.id.chip_untested)
        swGuard = findViewById(R.id.sw_guard); swLl = findViewById(R.id.sw_ll); swDyn = findViewById(R.id.sw_dyn)
        btnUpdate = findViewById(R.id.btn_update)
    }

    private fun expander(header: View, body: View, arrow: ImageView, open: Boolean, onChange: (Boolean) -> Unit) {
        fun show(o: Boolean) {
            body.visibility = if (o) View.VISIBLE else View.GONE
            arrow.rotation = if (o) 180f else 0f
            ViewCompat.setStateDescription(header, getString(if (o) R.string.state_expanded else R.string.state_collapsed))
        }
        show(open)
        header.setOnClickListener { val o = body.visibility != View.VISIBLE; show(o); onChange(o) }
    }

    private fun wireViews() {
        val toolbar = findViewById<MaterialToolbar>(R.id.toolbar)
        toolbar.inflateMenu(R.menu.main_menu)
        toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.menu_device_info -> {
                    startActivity(Intent(this, DeviceInfoActivity::class.java))
                    true
                }
                R.id.menu_about -> {
                    MaterialAlertDialogBuilder(this).setTitle(R.string.about_title)
                        .setMessage(getString(R.string.about_body, UsbCamService.VERSION))
                        .setPositiveButton(R.string.ok, null).show()
                    true
                }
                else -> false
            }
        }
        fab.setOnClickListener { startActivity(Intent(this, CameraActivity::class.java)) }

        // camera on/off
        swCam.isChecked = UiPrefs.isCamOn(this)
        swCam.setOnCheckedChangeListener { _, on ->
            if (!syncing) UiPrefs.saveCamOn(this, on)
            if (!syncing) startForegroundService(Intent(this, UsbCamService::class.java)
                .setAction(if (on) UsbCamService.ACTION_CAM_ON else UsbCamService.ACTION_CAM_OFF))
        }
        findViewById<View>(R.id.row_camera).setOnClickListener { swCam.performClick() }

        // status card button
        statusAction.setOnClickListener {
            if (action == 1) {
                if (shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)) requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
                else startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null)))
            } else if (action == 2) {
                startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_CAM_ON))
            }
        }

        // presets
        presetGroup.addOnButtonCheckedListener { g, id, checked ->
            if (syncing) return@addOnButtonCheckedListener
            Ui.markSegments(g)
            if (checked) {
                val i = when (id) { R.id.preset_fast -> 0; R.id.preset_balanced -> 1; else -> 2 }
                UiPrefs.savePreset(this, i)
                applyPreset(i)
            } else presetGroup.post { updateSummary() } // tapping the selected preset again keeps it selected
        }

        // dropdowns
        ddCam.setOnItemClickListener { _, _, pos, _ ->
            if (pos != camIdx) { camIdx = pos; modeIdx = 0; fpsIdx = 0; fillModes(); fillFps(); changed() }
        }
        ddRes.setOnItemClickListener { _, _, pos, _ ->
            if (pos != modeIdx) { modeIdx = pos; fpsIdx = 0; fillFps(); changed() }
            updateHint()
        }
        ddFps.setOnItemClickListener { _, _, pos, _ -> if (pos != fpsIdx) { fpsIdx = pos; changed() } }
        ddBr.setOnItemClickListener { _, _, pos, _ -> if (pos != brIdx) { brIdx = pos; changed() } }
        // after a pick, or when the list is closed without a pick, the field loses its selected state
        for (d in arrayOf(ddCam, ddRes, ddFps, ddBr)) d.setOnDismissListener { ui.post { releaseDropdownFocus() } }
        chipZoomed.setOnClickListener { explain(R.string.chip_zoomed, R.string.chip_zoomed_why) }
        chipUntested.setOnClickListener { explain(R.string.chip_untested, R.string.chip_untested_why) }

        // protection and advanced
        val p0 = Prefs(this)
        swGuard.isChecked = p0.guard
        swGuard.setOnCheckedChangeListener { _, _ -> save() }
        findViewById<View>(R.id.row_guard).setOnClickListener { swGuard.performClick() }
        swLl.isChecked = p0.ll == 1
        swLl.setOnCheckedChangeListener { _, _ -> changed() }
        findViewById<View>(R.id.row_ll).setOnClickListener { swLl.performClick() }

        val dynRow = findViewById<View>(R.id.row_dyn)
        if (DynamicColors.isDynamicColorAvailable()) {
            swDyn.isChecked = UiPrefs.isDynamic(this)
            swDyn.setOnCheckedChangeListener { _, on -> UiPrefs.saveDynamic(this, on); recreate() }
            dynRow.setOnClickListener { swDyn.performClick() }
        } else dynRow.visibility = View.GONE

        val themeGroup = findViewById<MaterialButtonToggleGroup>(R.id.theme_group)
        themeGroup.check(when (UiPrefs.nightMode(this)) { AppCompatDelegate.MODE_NIGHT_NO -> R.id.theme_light; AppCompatDelegate.MODE_NIGHT_YES -> R.id.theme_dark; else -> R.id.theme_system })
        Ui.markSegments(themeGroup)
        themeGroup.addOnButtonCheckedListener { g, id, checked ->
            Ui.markSegments(g)
            if (!checked) return@addOnButtonCheckedListener
            val m = when (id) { R.id.theme_light -> AppCompatDelegate.MODE_NIGHT_NO; R.id.theme_dark -> AppCompatDelegate.MODE_NIGHT_YES; else -> AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM }
            UiPrefs.saveNightMode(this, m)
            AppCompatDelegate.setDefaultNightMode(m)
        }
        findViewById<TextView>(R.id.update_version).text = getString(R.string.update_version, Updater.installedName(this), Updater.installedCode(this))
        btnUpdate.setOnClickListener { checkForUpdate(true) }
        findViewById<View>(R.id.btn_reset).setOnClickListener {
            val p = Prefs(this); p.cam = ""; p.w = 0; p.h = 0; p.fps = 0; p.br = 0
            restoreSelection(); fillCam(); fillModes(); fillFps(); fillBr(); changed()
        }

        expander(findViewById(R.id.adv_header), findViewById(R.id.adv_body), findViewById(R.id.adv_arrow), advOpen) { advOpen = it }
        expander(findViewById(R.id.pic_header), findViewById(R.id.pic_body), findViewById(R.id.pic_arrow), picOpen) { picOpen = it }
    }

    private fun explain(titleRes: Int, bodyRes: Int) {
        MaterialAlertDialogBuilder(this).setTitle(titleRes).setMessage(bodyRes).setPositiveButton(R.string.ok, null).show()
    }

    // ---- app updates (GitHub Releases) ----

    private fun checkForUpdate(manual: Boolean) {
        if (checking) return
        checking = true
        if (manual) { btnUpdate.isEnabled = false; btnUpdate.setText(R.string.update_checking) }
        Thread {
            var failed = false
            val found: Updater.Info? = try { Updater.fetchLatest() } catch (_: Exception) { failed = true; null }
            val isFailed = failed
            runOnUiThread {
                checking = false
                if (isDestroyed || isFinishing) return@runOnUiThread
                btnUpdate.isEnabled = true
                btnUpdate.setText(R.string.update_check)
                if (found != null && found.code > Updater.installedCode(this)) showUpdateDialog(found)
                else if (manual) Snackbar.make(coord, if (isFailed) R.string.update_failed else R.string.update_uptodate, Snackbar.LENGTH_LONG).setAnchorView(fab).show()
            }
        }.start()
    }

    private fun showUpdateDialog(info: Updater.Info) {
        val notes = Updater.plainNotes(info.notes).ifEmpty { getString(R.string.update_no_notes) }
        val have = getString(R.string.update_version, Updater.installedName(this), Updater.installedCode(this))
        MaterialAlertDialogBuilder(this).setTitle(R.string.update_title)
            .setMessage(getString(R.string.update_msg, info.name, have, notes))
            .setPositiveButton(R.string.update_now) { _, _ -> startUpdate(info) }
            .setNegativeButton(R.string.update_later, null).show()
    }

    private fun startUpdate(info: Updater.Info) {
        if (!Updater.canInstall(this)) {
            pendingUpdate = info
            MaterialAlertDialogBuilder(this).setTitle(R.string.update_perm_title).setMessage(R.string.update_perm_msg)
                .setPositiveButton(R.string.update_open_settings) { _, _ ->
                    startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + packageName)))
                }
                .setNegativeButton(R.string.update_later) { _, _ -> pendingUpdate = null }.show()
            return
        }
        downloadAndInstall(info)
    }

    private fun downloadAndInstall(info: Updater.Info) {
        val v = layoutInflater.inflate(R.layout.dialog_update_progress, null)
        val bar = v.findViewById<LinearProgressIndicator>(R.id.update_bar)
        val pct = v.findViewById<TextView>(R.id.update_pct)
        pct.text = getString(R.string.update_pct, 0)
        val cancel = java.util.concurrent.atomic.AtomicBoolean(false)
        val dlg = MaterialAlertDialogBuilder(this).setTitle(R.string.update_downloading).setView(v).setCancelable(false)
            .setNegativeButton(R.string.update_cancel) { _, _ -> cancel.set(true) }.show()
        val app = applicationContext
        Thread {
            var file: java.io.File? = null
            try {
                file = Updater.download(app, info, { p ->
                    runOnUiThread { if (!isDestroyed) { bar.setProgressCompat(p, true); pct.text = getString(R.string.update_pct, p) } }
                }, { cancel.get() })
            } catch (_: Exception) { }
            val done = file
            runOnUiThread {
                if (!isDestroyed) dlg.dismiss()
                if (done != null) {
                    try { Updater.install(app, done) } catch (_: Exception) { }
                } else if (!isDestroyed && !cancel.get()) {
                    Snackbar.make(coord, R.string.update_dl_failed, Snackbar.LENGTH_LONG).setAnchorView(fab).show()
                }
            }
        }.start()
    }

    // ---- dropdowns ----

    private fun fillDropdown(dd: MaterialAutoCompleteTextView, items: List<String>, sel: Int) {
        dd.setAdapter(AllItemsAdapter(this, items))
        dd.setText(items.getOrElse(sel) { "" }, false)
    }

    private fun fillCam() = fillDropdown(ddCam, cams.map { it.label.trim() }, camIdx)

    private fun fillModes() {
        val m = curModes()
        val items = (0 until m.length()).map {
            val x = m.getJSONObject(it)
            val n = when (x.getString("note")) { "cropped" -> getString(R.string.note_zoomed_item); "unverified" -> getString(R.string.note_untested_item); else -> "" }
            getString(R.string.mode_item, x.getString("label"), x.getInt("w"), x.getInt("h"), n)
        }
        fillDropdown(ddRes, items, modeIdx); updateHint()
    }

    private fun fillFps() = fillDropdown(ddFps, fpsList().map { getString(R.string.fps_item, it) }, fpsIdx)

    private fun fillBr() = fillDropdown(ddBr, brOptions.map { if (it == 0) getString(R.string.bitrate_auto) else getString(R.string.bitrate_item, it) }, brIdx)

    private fun updateHint() {
        val n = curModes().optJSONObject(modeIdx)?.optString("note") ?: ""
        chipZoomed.visibility = if (n == "cropped") View.VISIBLE else View.GONE
        chipUntested.visibility = if (n == "unverified") View.VISIBLE else View.GONE
        updateSummary()
    }

    // ---- presets, summary, pending bar ----

    /** Mode index of a preset: Fast = nearest 720p, Balanced = nearest 1080p, Sharp = the biggest picture the selected camera offers. */
    private fun presetMode(i: Int): Int {
        val m = curModes(); if (m.length() == 0) return 0
        var best = 0
        if (i == 2) {
            for (k in 1 until m.length()) {
                val a = m.getJSONObject(k); val b = m.getJSONObject(best)
                if (a.getInt("w") * a.getInt("h") > b.getInt("w") * b.getInt("h")) best = k
            }
        } else {
            val t = if (i == 0) 720 else 1080
            for (k in 1 until m.length()) if (Math.abs(m.getJSONObject(k).getInt("h") - t) < Math.abs(m.getJSONObject(best).getInt("h") - t)) best = k
        }
        return best
    }

    private fun applyPreset(i: Int) {
        if (curModes().length() == 0) return
        modeIdx = presetMode(i); fillModes()
        val f = fpsList(); fpsIdx = f.indexOf(30).let { if (it < 0) 0 else it }; fillFps()
        brIdx = 0; fillBr(); changed()
    }

    private fun changed() { save(); dirty = true; updateSummary(); showPending() }

    private fun updateSummary() {
        if (!::summaryTv.isInitialized) return
        val m = curModes().optJSONObject(modeIdx) ?: return
        val fps = fpsList().getOrNull(fpsIdx) ?: 30
        val w = m.getInt("w"); val h = m.getInt("h")
        val mb = if (brOptions[brIdx] > 0) brOptions[brIdx] else Caps.bitrate(w, h, fps) / 1_000_000
        val ids = intArrayOf(R.id.preset_fast, R.id.preset_balanced, R.id.preset_sharp)
        val names = intArrayOf(R.string.preset_fast, R.string.preset_balanced, R.string.preset_sharp)
        val match = BooleanArray(3) { i -> presetMode(i) == modeIdx && fps == 30 && brIdx == 0 }
        // Several presets can map to the same size on phones with few sizes: prefer the one the user actually tapped
        val saved = UiPrefs.preset(this)
        val on = if (saved in 0..2 && match[saved]) saved else match.indexOfFirst { it }
        syncing = true
        if (on >= 0) presetGroup.check(ids[on]) else presetGroup.clearChecked()
        syncing = false
        Ui.markSegments(presetGroup)
        summaryTv.text = getString(R.string.summary_fmt, getString(if (on >= 0) names[on] else R.string.preset_custom), w, h, fps, mb)
    }

    private fun showPending() {
        if (pending?.isShown == true) return
        val sb = Snackbar.make(coord, R.string.pending_msg, Snackbar.LENGTH_INDEFINITE)
            .setAction(R.string.restart_stream) { applyNow() }
        sb.setAnchorView(fab)
        pending = sb
        sb.show()
    }

    private fun applyNow() {
        save(); dirty = false; updateSummary()
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_APPLY))
        Snackbar.make(coord, R.string.restarted, Snackbar.LENGTH_SHORT).setAnchorView(fab).show()
    }

    // ---- live status ----

    private fun attr(a: Int): Int = MaterialColors.getColor(statusCard, a)

    private fun stateNow(): UiState {
        val s = UsbCamService
        val low = s.status.lowercase()
        val permOk = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        val live = s.instance?.clientOn() == true
        val errBg = MR.attr.colorErrorContainer; val errFg = MR.attr.colorOnErrorContainer
        return when {
            !permOk -> UiState(R.string.st_noperm, R.string.st_noperm_text, R.drawable.ic_warning, errBg, errFg, 1)
            s.guardStage == 2 -> UiState(R.string.st_hot, R.string.st_hot_text, R.drawable.ic_warning, errBg, errFg, 0)
            !s.camEnabled -> UiState(R.string.st_off, R.string.st_off_text, R.drawable.ic_videocam_off, MR.attr.colorSurfaceContainerHighest, MR.attr.colorOnSurface, 0)
            low.contains("denied") || low.contains("fail") || low.contains("error") -> UiState(R.string.st_problem, R.string.st_problem_text, R.drawable.ic_warning, errBg, errFg, 2)
            live -> UiState(R.string.st_streaming, R.string.st_streaming_text, R.drawable.ic_sensors_live, MR.attr.colorTertiaryContainer, MR.attr.colorOnTertiaryContainer, 0)
            low.contains("stream") || low.contains("wait") || low.contains("listen") || low.contains("ready") || low.contains("idle") || low.contains("control") ->
                UiState(R.string.st_waiting, R.string.st_waiting_text, R.drawable.ic_videocam, MR.attr.colorPrimaryContainer, MR.attr.colorOnPrimaryContainer, 0)
            else -> UiState(R.string.st_starting, R.string.st_starting_text, R.drawable.ic_videocam, MR.attr.colorSurfaceContainerHigh, MR.attr.colorOnSurface, 0)
        }
    }

    private fun stylePill(tv: TextView, iconRes: Int, bg: Int, fg: Int) {
        val dens = resources.displayMetrics.density
        tv.setTextColor(fg)
        tv.background = GradientDrawable().apply { setColor(bg); cornerRadius = 16f * dens }
        val d = androidx.core.content.ContextCompat.getDrawable(this, iconRes)?.mutate()
        if (d != null) { val sz = (18 * dens).toInt(); d.setBounds(0, 0, sz, sz) }
        tv.setCompoundDrawablesRelative(d, null, null, null)
        tv.compoundDrawableTintList = ColorStateList.valueOf(fg)
    }

    private fun refreshStatus() {
        val s = UsbCamService
        val st = stateNow()
        val isLive = st.title == R.string.st_streaming
        val bg = if (isLive) getColor(R.color.live_start) else attr(st.bg)
        val fg = if (isLive) getColor(R.color.live_on) else attr(st.fg)
        // premium look: soft diagonal gradient from the state colour towards a tint of the accent (works in light, dark and dynamic colours)
        val accent = attr(if (st.bg == MR.attr.colorErrorContainer) MR.attr.colorError else MR.attr.colorPrimary)
        val end = if (isLive) getColor(R.color.live_end) else ColorUtils.blendARGB(bg, accent, 0.2f)
        val key = (bg.toLong() shl 32) xor (end.toLong() and 0xFFFFFFFFL)
        if (key != gradKey) {
            gradKey = key
            statusCard.setCardBackgroundColor(Color.TRANSPARENT)
            statusContent.background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(bg, end)).apply { cornerRadius = statusCard.radius }
        }
        if (st.icon != lastIcon) { // only when it changes, so the live animation is not restarted every second
            lastIcon = st.icon
            statusIcon.setImageResource(st.icon)
        }
        // keep the live animation running: it stops on rotation / when the window is hidden, so re-start it whenever it is not running
        (statusIcon.drawable as? android.graphics.drawable.Animatable)?.let { if (!it.isRunning) it.start() }
        statusIcon.imageTintList = ColorStateList.valueOf(fg)
        statusTitle.setText(st.title); statusTitle.setTextColor(fg)
        val warm = s.guardStage == 1 && st.action == 0 && s.camEnabled
        statusText.text = getString(st.text) + if (warm) getString(R.string.st_warm_note) else ""
        statusText.setTextColor(fg)
        action = st.action
        if (st.action == 0) statusAction.visibility = View.GONE
        else { statusAction.visibility = View.VISIBLE; statusAction.setText(if (st.action == 1) R.string.st_fix_perm else R.string.st_try_again) }

        // battery / temperature pills follow the card theme: translucent dark on the live green card, tinted container colour elsewhere
        val isLiveCard = isLive
        val pillBg = if (isLiveCard) 0x40000000 else ColorUtils.setAlphaComponent(fg, 0x26)
        chipBattery.text = if (s.battLevel >= 0) getString(R.string.chip_battery, s.battLevel) else getString(R.string.chip_battery_unknown)
        chipBattery.contentDescription = if (s.plugged && s.battLevel >= 0) getString(R.string.chip_battery_charging, s.battLevel) else chipBattery.text
        stylePill(chipBattery, if (s.plugged) R.drawable.ic_battery_charging else R.drawable.ic_battery, pillBg, fg)
        chipTemp.text = getString(R.string.chip_temp, s.battTemp)
        stylePill(chipTemp, R.drawable.ic_thermostat, pillBg, fg)

        if (swCam.isChecked != s.camEnabled) { syncing = true; swCam.isChecked = s.camEnabled; syncing = false }
        camSupport.setText(if (s.camEnabled) R.string.camera_on_support else R.string.camera_off_support)
    }
}
