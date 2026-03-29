package com.invertoscope.quest.gl

import android.content.Context
import android.util.AttributeSet
import android.opengl.GLSurfaceView
import com.invertoscope.quest.camera.DualCameraController

class StereoGlSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : GLSurfaceView(context, attrs) {

    private val renderer = StereoRenderer()
    private val cameraController = DualCameraController(context)

    init {
        setEGLContextClientVersion(3)
        preserveEGLContextOnPause = true
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    fun onPermissionsGranted() {
        queueEvent {
            renderer.ensureStreams()
            cameraController.start(renderer.leftEyeSurface, renderer.rightEyeSurface)
        }
    }

    fun updateLeftEye(transform: EyeTransform) {
        queueEvent {
            renderer.leftTransform = transform
        }
    }

    fun updateRightEye(transform: EyeTransform) {
        queueEvent {
            renderer.rightTransform = transform
        }
    }

    fun reset() {
        queueEvent {
            renderer.leftTransform = EyeTransform()
            renderer.rightTransform = EyeTransform()
        }
    }

    fun stopCamera() {
        cameraController.stop()
    }
}
