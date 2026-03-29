package com.invertoscope.quest.xr

import android.app.Activity

object OpenXrBridge {
    init {
        System.loadLibrary("openxr_bootstrap")
    }

    external fun start(activity: Activity, callback: Any): String
    external fun stop()
    external fun onResume()
    external fun onPause()
    external fun getCameraTextureId(eye: Int): Int
    external fun setCameraTextureMatrix(eye: Int, matrix4x4: FloatArray)
}
