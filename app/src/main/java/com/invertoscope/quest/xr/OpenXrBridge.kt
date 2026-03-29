package com.invertoscope.quest.xr

object OpenXrBridge {
    init {
        System.loadLibrary("openxr_bootstrap")
    }

    external fun bootstrapRuntimeInfo(): String
}
