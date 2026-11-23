package com.harman.usbcam

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate

/** UI-only preferences (look and first-use hints). Separate from [Prefs], which holds the streaming choice. */
object UiPrefs {
    private fun sp(c: Context) = c.getSharedPreferences("usbcam_ui", Context.MODE_PRIVATE)

    fun isDynamic(c: Context): Boolean = sp(c).getBoolean("dynamic", false)
    fun saveDynamic(c: Context, on: Boolean) { sp(c).edit().putBoolean("dynamic", on).apply() }

    /** One of AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM / _NO / _YES. */
    fun nightMode(c: Context): Int = sp(c).getInt("night", AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM)
    fun saveNightMode(c: Context, m: Int) { sp(c).edit().putInt("night", m).apply() }

    fun isHintSeen(c: Context): Boolean = sp(c).getBoolean("hint_focus", false)
    fun markHintSeen(c: Context) { sp(c).edit().putBoolean("hint_focus", true).apply() }

    /** Master camera switch, remembered so it stays off even when the PC tools relaunch the app. */
    fun isCamOn(c: Context): Boolean = sp(c).getBoolean("cam_on", true)
    fun saveCamOn(c: Context, on: Boolean) { sp(c).edit().putBoolean("cam_on", on).apply() }

    /** Last quality preset the user tapped: 0 Fast, 1 Balanced, 2 Sharp, -1 none. */
    fun preset(c: Context): Int = sp(c).getInt("preset", -1)
    fun savePreset(c: Context, i: Int) { sp(c).edit().putInt("preset", i).apply() }
}
