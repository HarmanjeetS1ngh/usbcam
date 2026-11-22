package com.harman.usbcam

import android.content.Context
import android.util.AttributeSet
import android.widget.FrameLayout

/** Keeps the camera preview at 16:9, centred. Inflated from activity_camera.xml. */
class AspectLayout @JvmOverloads constructor(c: Context, a: AttributeSet? = null) : FrameLayout(c, a) {
    override fun onMeasure(w: Int, h: Int) {
        val mw = MeasureSpec.getSize(w); val mh = MeasureSpec.getSize(h)
        var cw = mw; var ch = (mw * 9f / 16f).toInt()
        if (ch > mh) { ch = mh; cw = (mh * 16f / 9f).toInt() }
        super.onMeasure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY))
    }
}
