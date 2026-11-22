package com.harman.usbcam

import android.graphics.PointF
import android.graphics.Rect
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata

/** Manual camera controls shared by the preview screen (CameraActivity) and the streamer. Survives stream restarts. */
class CamControls {
    // user settings
    @Volatile var aeLock = false
    @Volatile var afLock = false
    @Volatile var awbLock = false
    @Volatile var manualExp = false; @Volatile var iso = 100; @Volatile var expNs = 10_000_000L
    @Volatile var manualFocus = false; @Volatile var focusDist = 0f   // diopters, 0 = infinity
    @Volatile var ev = 0                                              // exposure-compensation steps
    @Volatile var point: PointF? = null                               // tap point, 0..1 within the picture

    // live values from the latest frame
    @Volatile var liveIso = 100; @Volatile var liveExpNs = 10_000_000L; @Volatile var liveFocus = 0f
    @Volatile var crop: Rect? = null

    // what this camera supports
    @Volatile var manualOk = false; @Volatile var aeLockOk = false
    @Volatile var isoMin = 50; @Volatile var isoMax = 800
    @Volatile var expMin = 100_000L; @Volatile var expMax = 33_000_000L
    @Volatile var evMin = 0; @Volatile var evMax = 0; @Volatile var evStep = 0f
    @Volatile var minFocus = 0f
    @Volatile var activeArray: Rect? = null
    @Volatile var fps = 30

    val locked: Boolean get() = aeLock || afLock

    fun load(ch: CameraCharacteristics?, fpsNow: Int) {
        fps = fpsNow
        ch ?: return
        val caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
        manualOk = caps.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)
        ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let { isoMin = it.lower; isoMax = it.upper }
        ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)?.let { expMin = it.lower; expMax = it.upper }
        ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)?.let { evMin = it.lower; evMax = it.upper }
        evStep = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0f
        minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        aeLockOk = ch.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE) == true
        activeArray = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
    }

    fun lockAeAf() { aeLock = true; afLock = true; focusDist = liveFocus }
    fun unlockAeAf() { aeLock = false; afLock = false; point = null }
    fun reset() { unlockAeAf(); awbLock = false; manualExp = false; manualFocus = false; ev = 0 }
}
