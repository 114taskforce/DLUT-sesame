package com.dlut.dooropener.net

import okhttp3.FormBody
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit

/** 设备返回的可读错误(口令错/字段超限/连不上…), message 直接给用户看 */
class DeviceException(message: String) : Exception(message)

/**
 * ESP32 门禁设备的本地 HTTP 客户端 —— 契约见固件仓库 docs/app-api.md 第 1 节。
 *
 * 与门禁服务器那套 Cookie/签名完全无关, 所以**单独一个不带 cookieJar 的 client**:
 * 用 DoorClient 的实例会把 sso/menjin 的会话 Cookie 顺手发给局域网设备。
 */
class DeviceClient {

    private val base = OkHttpClient.Builder()
        .protocols(listOf(Protocol.HTTP_1_1))
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .retryOnConnectionFailure(false)
        .build()

    private fun url(ip: String, path: String, pin: String) =
        "http://${ip.trim()}/$path?k=${pin.trim()}"

    private fun exec(req: Request, timeoutMs: Int? = null): JSONObject {
        val call = (if (timeoutMs != null)
            base.newBuilder().readTimeout((timeoutMs / 1000).toLong(), TimeUnit.SECONDS).build()
            else base
        ).newCall(req)
        try {
            call.execute().use { r ->
                val text = r.body?.string().orEmpty()
                val obj = try {
                    JSONObject(text)
                } catch (e: Exception) {
                    // 设备没起来/被路由器拦了:拿到的不是 JSON(校园网门户的劫持页就是这样)
                    throw DeviceException(
                        if (text.isBlank()) "设备无响应(HTTP ${r.code})"
                        else "回复不是 JSON(HTTP ${r.code}):${text.take(80)}"
                    )
                }
                if (!r.isSuccessful) {
                    val err = obj.optString("err", "HTTP ${r.code}")
                    val extra = when (err) {
                        "too-large" -> ",字段 ${obj.optString("field")} 上限 ${obj.optInt("max")}"
                        "busy-retry" -> ",${obj.optLong("afterMs") / 1000} 秒后再试"
                        else -> ""
                    }
                    throw DeviceException("设备拒绝:$err$extra")
                }
                return obj
            }
        } catch (e: IOException) {
            call.cancel()
            throw DeviceException(
                "连不上设备(${req.url.host})。手机要先连设备热点 DLUT-Door-xxxx" +
                    "(长按设备按键 8 秒),该热点不能上网属正常,蜂窝数据照常可用。"
            )
        }
    }

    fun status(ip: String, pin: String): JSONObject =
        exec(Request.Builder().url(url(ip, "status", pin)).get().build())

    fun save(ip: String, pin: String, fields: Map<String, String>): JSONObject {
        val fb = FormBody.Builder()
        fields.forEach { (k, v) -> fb.add(k, v) }
        return exec(Request.Builder().url(url(ip, "save", pin)).post(fb.build()).build())
    }

    /** 整链认证最坏几十秒, 读超时放到 120 秒 */
    fun apply(ip: String, pin: String): JSONObject =
        exec(Request.Builder().url(url(ip, "apply", pin)).post(FormBody.Builder().build()).build(), 120_000)

    fun open(ip: String, pin: String): JSONObject =
        exec(Request.Builder().url(url(ip, "open", pin)).post(FormBody.Builder().build()).build(), 40_000)

    companion object {
        const val DEFAULT_IP = "192.168.4.1"

        /** /status 摘要成一行中文, 设置页直接显示 */
        fun summarize(s: JSONObject): String = buildString {
            append(if (s.optBoolean("online")) "WiFi 已连" else "WiFi 未连")
            append(" · 校园网 ").append(s.optString("campus", "?"))
            append(" · token ").append(
                if (s.optInt("tokenAge", -1) < 0) "?" else "${s.optInt("tokenAge")}s前"
            )
            append(" · 云 ${s.optString("mqtt", "?")}")
            append(" · 空闲堆 ${(s.optLong("freeHeap") / 1024)}KB")
            s.optString("lastDoor").takeIf { it.isNotBlank() && it != "none" }
                ?.let { append(" · 上次开门 $it") }
        }
    }
}
