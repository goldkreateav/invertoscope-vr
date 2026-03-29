package com.invertoscope.quest

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.SurfaceTexture
import android.os.Bundle
import android.view.Surface
import androidx.activity.ComponentActivity
import androidx.annotation.Keep
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.invertoscope.quest.databinding.ActivityMainBinding
import com.invertoscope.quest.xr.OpenXrBridge

class MainActivity : ComponentActivity() {
    private lateinit var binding: ActivityMainBinding
    private val cameraController by lazy { com.invertoscope.quest.camera.DualCameraController(this) }
    private var runtimeStarted = false

    private var leftSurfaceTexture: SurfaceTexture? = null
    private var rightSurfaceTexture: SurfaceTexture? = null
    private var leftSurface: Surface? = null
    private var rightSurface: Surface? = null
    private val leftMatrix = FloatArray(16)
    private val rightMatrix = FloatArray(16)
    @Volatile
    private var leftFrameAvailable = false
    @Volatile
    private var rightFrameAvailable = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        requestCameraPermissionsAndStart()
    }

    private fun startImmersiveRuntime() {
        if (runtimeStarted) {
            cameraController.start(leftSurface, rightSurface)
            return
        }
        val status = OpenXrBridge.start(this, this)
        binding.openxrStatusText.text = status
        val leftTexId = OpenXrBridge.getCameraTextureId(0)
        val rightTexId = OpenXrBridge.getCameraTextureId(1)
        if (leftTexId <= 0 || rightTexId <= 0) {
            binding.openxrStatusText.text = "$status\nНе удалось создать GL-текстуры камер."
            return
        }
        setupCameraSurfaces(leftTexId, rightTexId)
        cameraController.start(leftSurface, rightSurface)
        runtimeStarted = true
    }

    private fun setupCameraSurfaces(leftTexId: Int, rightTexId: Int) {
        leftSurfaceTexture?.release()
        rightSurfaceTexture?.release()
        leftSurface?.release()
        rightSurface?.release()

        leftSurfaceTexture = SurfaceTexture(leftTexId).apply {
            setOnFrameAvailableListener { leftFrameAvailable = true }
            setDefaultBufferSize(1280, 720)
        }
        rightSurfaceTexture = SurfaceTexture(rightTexId).apply {
            setOnFrameAvailableListener { rightFrameAvailable = true }
            setDefaultBufferSize(1280, 720)
        }
        leftSurface = Surface(leftSurfaceTexture)
        rightSurface = Surface(rightSurfaceTexture)
    }

    private fun requestCameraPermissionsAndStart() {
        val permissions = arrayOf(
            Manifest.permission.CAMERA,
            "horizonos.permission.HEADSET_CAMERA"
        )

        val missing = permissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQUEST_PERMISSIONS)
        } else {
            startImmersiveRuntime()
        }
    }

    /**
     * Called from native render thread once per XR frame.
     */
    @Keep
    fun onNativeUpdateCameraTextures(): Boolean {
        try {
            if (leftFrameAvailable) {
                leftSurfaceTexture?.updateTexImage()
                leftSurfaceTexture?.getTransformMatrix(leftMatrix)
                OpenXrBridge.setCameraTextureMatrix(0, leftMatrix)
                leftFrameAvailable = false
            }
            if (rightFrameAvailable) {
                rightSurfaceTexture?.updateTexImage()
                rightSurfaceTexture?.getTransformMatrix(rightMatrix)
                OpenXrBridge.setCameraTextureMatrix(1, rightMatrix)
                rightFrameAvailable = false
            }
            return true
        } catch (_: Throwable) {
            return false
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (grantResults.all { it == PackageManager.PERMISSION_GRANTED }) {
                startImmersiveRuntime()
            } else {
                binding.openxrStatusText.text = getString(R.string.permission_required_status)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        OpenXrBridge.onResume()
        requestCameraPermissionsAndStart()
    }

    override fun onPause() {
        cameraController.stop()
        OpenXrBridge.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        cameraController.stop()
        leftSurface?.release()
        rightSurface?.release()
        leftSurfaceTexture?.release()
        rightSurfaceTexture?.release()
        OpenXrBridge.stop()
        runtimeStarted = false
        super.onDestroy()
    }

    companion object {
        private const val REQUEST_PERMISSIONS = 42
    }
}
