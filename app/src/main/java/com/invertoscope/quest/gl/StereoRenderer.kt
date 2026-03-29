package com.invertoscope.quest.gl

import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

class StereoRenderer : GLSurfaceView.Renderer {
    private var program = 0
    private var vertexBuffer: FloatBuffer =
        ByteBuffer.allocateDirect(QUAD_VERTICES.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
            .apply {
                put(QUAD_VERTICES)
                position(0)
            }

    private var leftTextureId = 0
    private var rightTextureId = 0
    private var leftSurfaceTexture: SurfaceTexture? = null
    private var rightSurfaceTexture: SurfaceTexture? = null

    var leftEyeSurface: Surface? = null
        private set
    var rightEyeSurface: Surface? = null
        private set

    @Volatile
    private var leftFrameAvailable = false

    @Volatile
    private var rightFrameAvailable = false

    var leftTransform: EyeTransform = EyeTransform()
    var rightTransform: EyeTransform = EyeTransform()

    private val leftTextureMatrix = FloatArray(16)
    private val rightTextureMatrix = FloatArray(16)

    private var aPositionLocation = -1
    private var aTexCoordLocation = -1
    private var uTextureLocation = -1
    private var uTextureMatrixLocation = -1
    private var uRotationLocation = -1
    private var uMirrorLocation = -1

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        program = createProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        aPositionLocation = GLES30.glGetAttribLocation(program, "aPosition")
        aTexCoordLocation = GLES30.glGetAttribLocation(program, "aTexCoord")
        uTextureLocation = GLES30.glGetUniformLocation(program, "uTexture")
        uTextureMatrixLocation = GLES30.glGetUniformLocation(program, "uTextureMatrix")
        uRotationLocation = GLES30.glGetUniformLocation(program, "uRotation")
        uMirrorLocation = GLES30.glGetUniformLocation(program, "uMirror")

        GLES30.glClearColor(0f, 0f, 0f, 1f)
        ensureStreams()
    }

    fun ensureStreams() {
        if (leftSurfaceTexture != null && rightSurfaceTexture != null) return
        leftTextureId = createExternalTexture()
        rightTextureId = createExternalTexture()

        leftSurfaceTexture = SurfaceTexture(leftTextureId).apply {
            setOnFrameAvailableListener { leftFrameAvailable = true }
        }
        rightSurfaceTexture = SurfaceTexture(rightTextureId).apply {
            setOnFrameAvailableListener { rightFrameAvailable = true }
        }
        leftEyeSurface = Surface(leftSurfaceTexture)
        rightEyeSurface = Surface(rightSurfaceTexture)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        GLES30.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        leftSurfaceTexture?.let {
            if (leftFrameAvailable) {
                it.updateTexImage()
                it.getTransformMatrix(leftTextureMatrix)
                leftFrameAvailable = false
            }
        }
        rightSurfaceTexture?.let {
            if (rightFrameAvailable) {
                it.updateTexImage()
                it.getTransformMatrix(rightTextureMatrix)
                rightFrameAvailable = false
            }
        }

        GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT)
        GLES30.glUseProgram(program)

        val viewport = IntArray(4)
        GLES30.glGetIntegerv(GLES30.GL_VIEWPORT, viewport, 0)
        val halfWidth = viewport[2] / 2
        val height = viewport[3]

        drawEye(
            x = 0,
            width = halfWidth,
            height = height,
            textureId = leftTextureId,
            matrix = leftTextureMatrix,
            transform = leftTransform
        )
        drawEye(
            x = halfWidth,
            width = halfWidth,
            height = height,
            textureId = rightTextureId,
            matrix = rightTextureMatrix,
            transform = rightTransform
        )
    }

    private fun drawEye(
        x: Int,
        width: Int,
        height: Int,
        textureId: Int,
        matrix: FloatArray,
        transform: EyeTransform
    ) {
        GLES30.glViewport(x, 0, width, height)
        GLES30.glActiveTexture(GLES30.GL_TEXTURE0)
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES30.glUniform1i(uTextureLocation, 0)
        GLES30.glUniformMatrix4fv(uTextureMatrixLocation, 1, false, matrix, 0)
        GLES30.glUniform1f(uRotationLocation, transform.rotationDegrees * (PI / 180.0).toFloat())
        GLES30.glUniform2f(
            uMirrorLocation,
            if (transform.mirrorX) -1f else 1f,
            if (transform.mirrorY) -1f else 1f
        )

        vertexBuffer.position(0)
        GLES30.glVertexAttribPointer(aPositionLocation, 2, GLES30.GL_FLOAT, false, 16, vertexBuffer)
        GLES30.glEnableVertexAttribArray(aPositionLocation)

        vertexBuffer.position(2)
        GLES30.glVertexAttribPointer(aTexCoordLocation, 2, GLES30.GL_FLOAT, false, 16, vertexBuffer)
        GLES30.glEnableVertexAttribArray(aTexCoordLocation)

        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun createExternalTexture(): Int {
        val ids = IntArray(1)
        GLES30.glGenTextures(1, ids, 0)
        GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, ids[0])
        GLES30.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES30.GL_TEXTURE_MIN_FILTER,
            GLES30.GL_LINEAR
        )
        GLES30.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES30.GL_TEXTURE_MAG_FILTER,
            GLES30.GL_LINEAR
        )
        GLES30.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES30.GL_TEXTURE_WRAP_S,
            GLES30.GL_CLAMP_TO_EDGE
        )
        GLES30.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES,
            GLES30.GL_TEXTURE_WRAP_T,
            GLES30.GL_CLAMP_TO_EDGE
        )
        return ids[0]
    }

    private fun createProgram(vertex: String, fragment: String): Int {
        val vertexShader = compileShader(GLES30.GL_VERTEX_SHADER, vertex)
        val fragmentShader = compileShader(GLES30.GL_FRAGMENT_SHADER, fragment)
        val programId = GLES30.glCreateProgram()
        GLES30.glAttachShader(programId, vertexShader)
        GLES30.glAttachShader(programId, fragmentShader)
        GLES30.glLinkProgram(programId)
        return programId
    }

    private fun compileShader(type: Int, source: String): Int {
        val shaderId = GLES30.glCreateShader(type)
        GLES30.glShaderSource(shaderId, source)
        GLES30.glCompileShader(shaderId)
        return shaderId
    }

    companion object {
        private val QUAD_VERTICES = floatArrayOf(
            -1f, -1f, 0f, 0f,
            1f, -1f, 1f, 0f,
            -1f, 1f, 0f, 1f,
            1f, 1f, 1f, 1f
        )

        private const val VERTEX_SHADER = """
            #version 300 es
            layout(location=0) in vec2 aPosition;
            layout(location=1) in vec2 aTexCoord;
            uniform mat4 uTextureMatrix;
            out vec2 vTexCoord;
            void main() {
                vec4 transformed = uTextureMatrix * vec4(aTexCoord, 0.0, 1.0);
                vTexCoord = transformed.xy;
                gl_Position = vec4(aPosition, 0.0, 1.0);
            }
        """

        private const val FRAGMENT_SHADER = """
            #version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision mediump float;
            in vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            uniform float uRotation;
            uniform vec2 uMirror;
            out vec4 fragColor;
            void main() {
                vec2 uv = vTexCoord;
                vec2 centered = uv - vec2(0.5, 0.5);
                float s = sin(uRotation);
                float c = cos(uRotation);
                mat2 rot = mat2(c, -s, s, c);
                vec2 transformed = (rot * (centered * uMirror)) + vec2(0.5, 0.5);
                fragColor = texture(uTexture, transformed);
            }
        """
    }
}
