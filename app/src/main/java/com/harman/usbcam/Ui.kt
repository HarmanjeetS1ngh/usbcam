package com.harman.usbcam

import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup

object Ui {
    /** Shows a check mark on the selected segment so the selection is obvious in every theme. */
    fun markSegments(g: MaterialButtonToggleGroup) {
        for (i in 0 until g.childCount) {
            val b = g.getChildAt(i) as? MaterialButton ?: continue
            b.setIconResource(if (b.isChecked) R.drawable.ic_check else 0)
        }
    }
}
