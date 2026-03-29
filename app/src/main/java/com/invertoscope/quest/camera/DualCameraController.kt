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
import java.util.concurrent.atomic.AtomicBoolean

class DualCameraController(context: Context) {
    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null

    private var leftCamera: CameraDevice? = null
    private var rightCamera: CameraDevice? = null
    private var leftSession: CameraCaptureSession? = null
    private var rightSession: CameraCaptureSession? = null
    private val running = AtomicBoolean(false)

    private var leftSurface: Surface? = null
    private var rightSurface: Surface? = null

    @Synchronized
    @SuppressLint("MissingPermission")
    fun start(left: Surface?, right: Surface?) {
        if (running.get()) return
        if (left == null || right == null) return
        ensureCameraThread()
        running.set(true)
        leftSurface = left
        rightSurface = right

        val cameraIds = pickTwoCameraIds()
        if (cameraIds.isEmpty()) {
            Log.e(TAG, "Камеры не найдены на устройстве")
            running.set(false)
            return
        }

        if (cameraIds.size == 1) {
            Log.w(TAG, "Доступна одна камера (${cameraIds[0]}). Включается fallback single-stream.")
            openSingleCamera(cameraIds[0])
        } else {
            Log.i(TAG, "Выбраны камеры L=${cameraIds[0]}, R=${cameraIds[1]}")
            openCamera(cameraIds[0], isLeft = true)
            openCamera(cameraIds[1], isLeft = false)
        }
    }

    @Synchronized
    fun stop() {
        running.set(false)
        setOf(leftSession, rightSession).filterNotNull().forEach { it.close() }
        leftSession = null
        rightSession = null

        setOf(leftCamera, rightCamera).filterNotNull().forEach { it.close() }
        leftCamera = null
        rightCamera = null

        leftSurface = null
        rightSurface = null

        cameraThread?.quitSafely()
        try {
            cameraThread?.join(1000)
        } catch (_: InterruptedException) {
        }
        cameraThread = null
        cameraHandler = null
    }

    @Synchronized
    private fun ensureCameraThread() {
        if (cameraThread?.isAlive == true && cameraHandler != null) return
        cameraThread = HandlerThread("quest-camera-thread").apply { start() }
        cameraHandler = Handler(cameraThread!!.looper)
    }

    private fun pickTwoCameraIds(): List<String> {
        val ids = cameraManager.cameraIdList
        val sorted = ids.sortedByDescending { id ->
            val chars = cameraManager.getCameraCharacteristics(id)
            chars.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_EXTERNAL
        }
        sorted.forEach { id ->
            val chars = cameraManager.getCameraCharacteristics(id)
            val facing = chars.get(CameraCharacteristics.LENS_FACING)
            val capabilities = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.joinToString() ?: "n/a"
            Log.i(TAG, "cameraId=$id facing=$facing capabilities=$capabilities")
        }
        return if (sorted.size >= 2) sorted.take(2) else sorted
    }

    private fun openCamera(cameraId: String, isLeft: Boolean) {
        val handler = cameraHandler ?: return
        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (!running.get()) {
                    camera.close()
                    return
                }
                if (isLeft) {
                    leftCamera = camera
                    createSession(camera, leftSurface, true)
                } else {
                    rightCamera = camera
                    createSession(camera, rightSurface, false)
                }
            }

            override fun onDisconnected(camera: CameraDevice) {
                running.set(false)
                camera.close()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                Log.e(TAG, "Ошибка камеры ($cameraId): $error")
                running.set(false)
                camera.close()
            }
        }, handler)
    }

    private fun openSingleCamera(cameraId: String) {
        val handler = cameraHandler ?: return
        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (!running.get()) {
                    camera.close()
                    return
                }
                leftCamera = camera
                rightCamera = camera
                createSharedSession(camera, leftSurface, rightSurface)
            }

            override fun onDisconnected(camera: CameraDevice) {
                running.set(false)
                camera.close()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                Log.e(TAG, "Ошибка камеры ($cameraId): $error")
                running.set(false)
                camera.close()
            }
        }, handler)
    }

    private fun createSession(camera: CameraDevice, outputSurface: Surface?, isLeft: Boolean) {
        if (outputSurface == null) return
        val handler = cameraHandler ?: return
        camera.createCaptureSession(
            listOf(outputSurface),
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    if (!running.get()) {
                        session.close()
                        return
                    }
                    val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(outputSurface)
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(30, 60))
                    }
                    session.setRepeatingRequest(request.build(), null, handler)
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
            handler
        )
    }

    private fun createSharedSession(camera: CameraDevice, left: Surface?, right: Surface?) {
        val targets = listOfNotNull(left, right).distinct()
        if (targets.isEmpty()) return
        val handler = cameraHandler ?: return
        camera.createCaptureSession(
            targets,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    if (!running.get()) {
                        session.close()
                        return
                    }
                    val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        targets.forEach { addTarget(it) }
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(30, 60))
                    }
                    session.setRepeatingRequest(request.build(), null, handler)
                    leftSession = session
                    rightSession = session
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.e(TAG, "Сбой общей сессии захвата")
                }
            },
            handler
        )
    }

    companion object {
        private const val TAG = "DualCameraController"
    }
}
