package com.harman.usbcam

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Camera -> SurfaceTexture (camera sees a normal GPU surface, not a video-encoder surface)
 * -> GL scale/centre-crop -> encoder input surface. GPU only, no CPU copies.
 * Purpose: let the camera run a big full-FOV mode (e.g. 4080x3060) and downscale to 3840x2160 here.
 */
class GlBridge(
    private val out: Surface, private val outW: Int, private val outH: Int,
    private val srcW: Int, private val srcH: Int
) {
    private val thread = HandlerThread("gl").also { it.start() }
    private val h = Handler(thread.looper)
    private var dpy: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var ctx: EGLContext = EGL14.EGL_NO_CONTEXT
    private var win: EGLSurface = EGL14.EGL_NO_SURFACE
    private var tex = 0
    private var st: SurfaceTexture? = null
    private var prog = 0
    private var aPos = 0
    private var aTex = 0
    private var uMtx = 0
    private val mtx = FloatArray(16)
    private var verts: FloatBuffer? = null
    private var texc: FloatBuffer? = null
    @Volatile private var camSurface: Surface? = null
    @Volatile private var live = true

    /** Returns the Surface the camera must write to, or null if GL setup failed. */
    fun start(): Surface? {
        val latch = CountDownLatch(1)
        h.post {
            try { init() } catch (e: Exception) { Log.e("UsbCam", "gl init failed: $e") }
            latch.countDown()
        }
        latch.await(3, TimeUnit.SECONDS)
        return camSurface
    }

    fun release() {
        live = false
        val latch = CountDownLatch(1)
        h.post {
            try { st?.release(); camSurface?.release() } catch (_: Exception) {}
            try {
                EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                EGL14.eglDestroySurface(dpy, win)
                EGL14.eglDestroyContext(dpy, ctx)
                EGL14.eglTerminate(dpy)
            } catch (_: Exception) {}
            latch.countDown()
        }
        latch.await(1, TimeUnit.SECONDS)
        thread.quitSafely()
    }

    private fun init() {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val ver = IntArray(2)
        EGL14.eglInitialize(dpy, ver, 0, ver, 1)
        val attrs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGLExt.EGL_RECORDABLE_ANDROID, 1, EGL14.EGL_NONE
        )
        val cfgs = arrayOfNulls<EGLConfig>(1)
        val n = IntArray(1)
        EGL14.eglChooseConfig(dpy, attrs, 0, cfgs, 0, 1, n, 0)
        val cfg = cfgs[0]!!
        ctx = EGL14.eglCreateContext(dpy, cfg, EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        win = EGL14.eglCreateWindowSurface(dpy, cfg, out, intArrayOf(EGL14.EGL_NONE), 0)
        EGL14.eglMakeCurrent(dpy, win, win, ctx)

        val vs = "attribute vec4 aPos; attribute vec2 aTex; uniform mat4 uMtx; varying vec2 vTex;" +
            "void main(){ gl_Position = aPos; vTex = (uMtx * vec4(aTex, 0.0, 1.0)).xy; }"
        val fs = "#extension GL_OES_EGL_image_external : require\n" +
            "precision mediump float; varying vec2 vTex; uniform samplerExternalOES sTex;" +
            "void main(){ gl_FragColor = texture2D(sTex, vTex); }"
        prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, compile(GLES20.GL_VERTEX_SHADER, vs))
        GLES20.glAttachShader(prog, compile(GLES20.GL_FRAGMENT_SHADER, fs))
        GLES20.glLinkProgram(prog)
        aPos = GLES20.glGetAttribLocation(prog, "aPos")
        aTex = GLES20.glGetAttribLocation(prog, "aTex")
        uMtx = GLES20.glGetUniformLocation(prog, "uMtx")

        // Centre-crop the source to the output aspect ratio (4:3 -> 16:9 keeps full width, trims top/bottom).
        val srcAr = srcW.toFloat() / srcH
        val outAr = outW.toFloat() / outH
        var u0 = 0f; var u1 = 1f; var v0 = 0f; var v1 = 1f
        if (srcAr < outAr) { v0 = (1f - srcAr / outAr) / 2f; v1 = 1f - v0 }
        else { u0 = (1f - outAr / srcAr) / 2f; u1 = 1f - u0 }
        verts = fb(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f))
        texc = fb(floatArrayOf(u0, v0, u1, v0, u0, v1, u1, v1))

        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        tex = t[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        val s = SurfaceTexture(tex)
        s.setDefaultBufferSize(srcW, srcH)
        s.setOnFrameAvailableListener({ draw() }, h)
        st = s
        camSurface = Surface(s)
    }

    private fun draw() {
        if (!live) return
        try {
            val s = st ?: return
            s.updateTexImage()
            s.getTransformMatrix(mtx)
            GLES20.glViewport(0, 0, outW, outH)
            GLES20.glUseProgram(prog)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, tex)
            GLES20.glUniformMatrix4fv(uMtx, 1, false, mtx, 0)
            GLES20.glEnableVertexAttribArray(aPos)
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 0, verts)
            GLES20.glEnableVertexAttribArray(aTex)
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 0, texc)
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            EGLExt.eglPresentationTimeANDROID(dpy, win, s.timestamp)
            EGL14.eglSwapBuffers(dpy, win)
        } catch (_: Exception) {}
    }

    private fun compile(type: Int, src: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) Log.e("UsbCam", "shader error: " + GLES20.glGetShaderInfoLog(s))
        return s
    }

    private fun fb(a: FloatArray): FloatBuffer =
        ByteBuffer.allocateDirect(a.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply { put(a); position(0) }
}
