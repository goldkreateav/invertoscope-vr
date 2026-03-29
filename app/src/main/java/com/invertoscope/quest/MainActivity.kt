package com.invertoscope.quest

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.SeekBar
import androidx.activity.ComponentActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.invertoscope.quest.databinding.ActivityMainBinding
import com.invertoscope.quest.gl.EyeTransform
import com.invertoscope.quest.xr.OpenXrBridge

class MainActivity : ComponentActivity() {
    private lateinit var binding: ActivityMainBinding

    private val leftEye = EyeTransform()
    private val rightEye = EyeTransform()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.openxrStatusText.text = OpenXrBridge.bootstrapRuntimeInfo()
        setupControls()
        requestCameraPermissions()
    }

    private fun setupControls() {
        binding.leftRotationSeek.setOnSeekBarChangeListener(rotationListener(leftEye, isLeft = true))
        binding.rightRotationSeek.setOnSeekBarChangeListener(rotationListener(rightEye, isLeft = false))

        binding.leftMirrorXSwitch.setOnCheckedChangeListener { _, checked ->
            leftEye.mirrorX = checked
            binding.stereoGlView.updateLeftEye(leftEye.copy())
        }
        binding.leftMirrorYSwitch.setOnCheckedChangeListener { _, checked ->
            leftEye.mirrorY = checked
            binding.stereoGlView.updateLeftEye(leftEye.copy())
        }
        binding.rightMirrorXSwitch.setOnCheckedChangeListener { _, checked ->
            rightEye.mirrorX = checked
            binding.stereoGlView.updateRightEye(rightEye.copy())
        }
        binding.rightMirrorYSwitch.setOnCheckedChangeListener { _, checked ->
            rightEye.mirrorY = checked
            binding.stereoGlView.updateRightEye(rightEye.copy())
        }

        binding.presetMirrorLeftButton.setOnClickListener {
            leftEye.rotationDegrees = 0f
            leftEye.mirrorX = true
            leftEye.mirrorY = false
            pushTransformsToUi()
        }

        binding.presetRotateRightButton.setOnClickListener {
            rightEye.rotationDegrees = 180f
            rightEye.mirrorX = false
            rightEye.mirrorY = false
            pushTransformsToUi()
        }

        binding.resetButton.setOnClickListener {
            leftEye.rotationDegrees = 0f
            leftEye.mirrorX = false
            leftEye.mirrorY = false
            rightEye.rotationDegrees = 0f
            rightEye.mirrorX = false
            rightEye.mirrorY = false
            pushTransformsToUi()
            binding.stereoGlView.reset()
        }
    }

    private fun pushTransformsToUi() {
        binding.leftRotationSeek.progress = leftEye.rotationDegrees.toInt()
        binding.leftMirrorXSwitch.isChecked = leftEye.mirrorX
        binding.leftMirrorYSwitch.isChecked = leftEye.mirrorY

        binding.rightRotationSeek.progress = rightEye.rotationDegrees.toInt()
        binding.rightMirrorXSwitch.isChecked = rightEye.mirrorX
        binding.rightMirrorYSwitch.isChecked = rightEye.mirrorY

        binding.stereoGlView.updateLeftEye(leftEye.copy())
        binding.stereoGlView.updateRightEye(rightEye.copy())
    }

    private fun rotationListener(model: EyeTransform, isLeft: Boolean): SeekBar.OnSeekBarChangeListener {
        return object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                model.rotationDegrees = progress.toFloat()
                if (isLeft) {
                    binding.stereoGlView.updateLeftEye(model.copy())
                } else {
                    binding.stereoGlView.updateRightEye(model.copy())
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        }
    }

    private fun requestCameraPermissions() {
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
            binding.stereoGlView.onPermissionsGranted()
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
                binding.stereoGlView.onPermissionsGranted()
            } else {
                binding.openxrStatusText.text = getString(R.string.permission_required_status)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        binding.stereoGlView.onResume()
        requestCameraPermissions()
    }

    override fun onPause() {
        binding.stereoGlView.stopCamera()
        binding.stereoGlView.onPause()
        super.onPause()
    }

    companion object {
        private const val REQUEST_PERMISSIONS = 42
    }
}
