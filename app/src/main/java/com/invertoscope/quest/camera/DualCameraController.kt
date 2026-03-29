package com.invertoscope.quest.camera

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface

class DualCameraController(context: Context) {
    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private val cameraThread = HandlerThread("quest-camera-thread").apply { start() }
    private val cameraHandler = Handler(cameraThread.looper)

    private var leftCamera: CameraDevice? = null
    private var rightCamera: CameraDevice? = null
    private var leftSession: CameraCaptureSession? = null
    private var rightSession: CameraCaptureSession? = null
    private var running = false

    private var leftSurface: Surface? = null
    private var rightSurface: Surface? = null

    @SuppressLint("MissingPermission")
    fun start(left: Surface?, right: Surface?) {
        if (running) return
        if (left == null || right == null) return
        running = true
        leftSurface = left
        rightSurface = right
        val cameraIds = pickTwoCameraIds()
        if (cameraIds.isEmpty()) {
            Log.e(TAG, "Камеры не найдены на устройстве")
            running = false
            return
        }

        if (cameraIds.size == 1) {
            openSingleCamera(cameraIds[0])
        } else {
            openCamera(cameraIds[0], isLeft = true)
            openCamera(cameraIds[1], isLeft = false)
        }
    }

    fun stop() {
        running = false
        setOf(leftSession, rightSession).filterNotNull().forEach { it.close() }
        leftSession = null
        rightSession = null

        setOf(leftCamera, rightCamera).filterNotNull().forEach { it.close() }
        leftCamera = null
        rightCamera = null
    }

    private fun pickTwoCameraIds(): List<String> {
        val ids = cameraManager.cameraIdList
        val sorted = ids.sortedByDescending { id ->
            val chars = cameraManager.getCameraCharacteristics(id)
            chars.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_EXTERNAL
        }
        return if (sorted.size >= 2) sorted.take(2) else sorted
    }

    private fun openCamera(cameraId: String, isLeft: Boolean) {
        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (isLeft) {
                    leftCamera = camera
                    createSession(camera, leftSurface, true)
                } else {
                    rightCamera = camera
                    createSession(camera, rightSurface, false)
                }
            }

            override fun onDisconnected(camera: CameraDevice) {
                running = false
                camera.close()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                Log.e(TAG, "Ошибка камеры ($cameraId): $error")
                running = false
                camera.close()
            }
        }, cameraHandler)
    }

    private fun openSingleCamera(cameraId: String) {
        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                leftCamera = camera
                rightCamera = camera
                createSharedSession(camera, leftSurface, rightSurface)
            }

            override fun onDisconnected(camera: CameraDevice) {
                running = false
                camera.close()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                Log.e(TAG, "Ошибка камеры ($cameraId): $error")
                running = false
                camera.close()
            }
        }, cameraHandler)
    }

    private fun createSession(camera: CameraDevice, outputSurface: Surface?, isLeft: Boolean) {
        if (outputSurface == null) return
        camera.createCaptureSession(
            listOf(outputSurface),
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(outputSurface)
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(30, 60))
                    }
                    session.setRepeatingRequest(request.build(), null, cameraHandler)
                    if (isLeft) {
                        leftSession = session
                    } else {
                        rightSession = session
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Сбой сессии захвата для ${if (isLeft) "левого" else "правого"} глаза")
                }
            },
            cameraHandler
        )
    }

    private fun createSharedSession(camera: CameraDevice, left: Surface?, right: Surface?) {
        val targets = listOfNotNull(left, right).distinct()
        if (targets.isEmpty()) return
        camera.createCaptureSession(
            targets,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        targets.forEach { addTarget(it) }
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(30, 60))
                    }
                    session.setRepeatingRequest(request.build(), null, cameraHandler)
                    leftSession = session
                    rightSession = session
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Сбой общей сессии захвата")
                }
            },
            cameraHandler
        )
    }

    companion object {
        private const val TAG = "DualCameraController"
    }
}
