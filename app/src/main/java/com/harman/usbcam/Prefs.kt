package com.harman.usbcam

import android.content.Context

/** Choice made on the phone screen (or by the PC tray). Empty/0 = "use the app default". PC tools that send no w/h/cam get this. */
class Prefs(ctx: Context) {
    private val sp = ctx.getSharedPreferences("usbcam", Context.MODE_PRIVATE)
    var cam: String
        get() = sp.getString("cam", "") ?: ""
        set(v) { sp.edit().putString("cam", v).apply() }
    var w: Int
        get() = sp.getInt("w", 0)
        set(v) { sp.edit().putInt("w", v).apply() }
    var h: Int
        get() = sp.getInt("h", 0)
        set(v) { sp.edit().putInt("h", v).apply() }
    var fps: Int
        get() = sp.getInt("fps", 0)
        set(v) { sp.edit().putInt("fps", v).apply() }
    /** bits per second, 0 = automatic */
    var br: Int
        get() = sp.getInt("br", 0)
        set(v) { sp.edit().putInt("br", v).apply() }
    var ll: Int
        get() = sp.getInt("ll", 1)
        set(v) { sp.edit().putInt("ll", v).apply() }
    var guard: Boolean
        get() = sp.getBoolean("guard", true)
        set(v) { sp.edit().putBoolean("guard", v).apply() }
    val hasChoice: Boolean get() = w > 0 && h > 0
}
