package com.harman.usbcam

import android.util.Log
import org.json.JSONObject
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * JSON control channel: newline-delimited JSON, one request line -> one reply line, loopback only.
 * PC reaches it with `adb forward tcp:5002 tcp:5002`. One client at a time.
 */
class ControlServer(private val port: Int, private val handler: (JSONObject) -> JSONObject) {
    @Volatile private var running = true
    private var ss: ServerSocket? = null

    fun start() { Thread({ loop() }, "ctl").start() }

    fun stop() {
        running = false
        try { ss?.close() } catch (_: Exception) {}
    }

    private fun loop() {
        try {
            val s = ServerSocket(port, 8, InetAddress.getByName("127.0.0.1"))
            ss = s
            while (running) {
                val c = try { s.accept() } catch (_: Exception) { break }
                // one thread per client: the PC tray can talk to the phone while the viewer / OBS plugin is connected
                Thread({
                    try { serve(c) } catch (_: Exception) {}
                    try { c.close() } catch (_: Exception) {}
                }, "ctl-client").start()
            }
        } catch (e: Exception) {
            Log.e("UsbCam", "control server error: ${e.message}")
        }
    }

    private fun serve(c: Socket) {
        c.tcpNoDelay = true
        val r = c.getInputStream().bufferedReader(Charsets.UTF_8)
        val w = c.getOutputStream()
        while (running) {
            val line = r.readLine() ?: break
            if (line.isBlank()) continue
            val reply = try { handler(JSONObject(line)) }
                        catch (e: Exception) { JSONObject().put("ok", false).put("err", e.message ?: "bad request") }
            w.write((reply.toString() + "\n").toByteArray(Charsets.UTF_8))
            w.flush()
        }
    }
}
