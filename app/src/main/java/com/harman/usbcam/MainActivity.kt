package com.harman.usbcam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import android.widget.ScrollView
import android.widget.TextView

/**
 * Thin launcher: asks for permissions, starts UsbCamService (which owns camera, encoder and control channel) and shows status.
 * Intent extras are forwarded to the service, so `run.bat` still works:
 *   adb shell am start -n com.harman.usbcam/.MainActivity --ei w 1920 --ei h 1080 --ei fps 30 --ei br 12000000 [--es cam <id>] [--ez hdr true]
 * With no stream extras the service just goes idle and waits for the PC on the control port (5002).
 */
class MainActivity : Activity() {
    private lateinit var tv: TextView
    private val caps by lazy { try { Caps.text(Caps.build(this)) } catch (e: Exception) { "caps failed: ${e.message}" } }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        tv = TextView(this).apply {
            setPadding(32, 32, 32, 32)
            textSize = 11f
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.BLACK)
        }
        setContentView(ScrollView(this).apply { addView(tv) })
        render()
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
        else { UsbCamService.status = "Camera permission denied"; render() }
    }

    private fun launch() {
        startForegroundService(Intent(this, UsbCamService::class.java).setAction(UsbCamService.ACTION_START).putExtras(intent))
    }

    override fun onResume() {
        super.onResume()
        UsbCamService.listener = { runOnUiThread { render() } }
        render()
    }

    override fun onPause() {
        UsbCamService.listener = null
        super.onPause()
    }

    private fun render() {
        tv.text = "UsbCam ${UsbCamService.VERSION}\n${UsbCamService.status}\n\nModes (camera + encoder limits; [cropped] = measured sensor crop):\n$caps"
    }
}
