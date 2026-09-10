package com.dlut.dooropener

import com.dlut.dooropener.net.DesCipher
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.FormBody
import okhttp3.HttpUrl
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.util.concurrent.TimeUnit

/**
 * WebVPN 探针:复现 open_door_puppeteer.js 的链路,验证「先打开 VPN 再访问」在 OkHttp 下怎么走 ——
 *   1) /login?cas_login=true → CAS 登录(service 指向网关)→ 回到门户,票据升级为已认证;
 *   2) 之后 /http/<enc>/... 由网关处理:校外代理到门禁,校园网内 302 回真实域名;
 *   3) POST 遇到 302 必须原样重发(跟随重定向会把 POST 降级成 GET,开门等于白发)。
 * 全程只读:只取门禁首页与设备列表,不触发开门。
 * 凭据:环境变量 DOOR_ACC/DOOR_PW,或项目根 env / probe.txt(见 probe.txt.template)。
 * 运行:.\gradlew.bat :app:testDebugUnitTest --tests com.dlut.dooropener.VpnProbeTest -i
 */
class VpnProbeTest {

    private val enc = "57787a7876706e323032336b65794024751d0c12f50ddd4aa659ee7694bf90698d72"
    private val vpnBase = "https://webvpn.dlut.edu.cn/http/$enc"
    private val vpnIndex = "$vpnBase/cser/static/menjin/index.html"
    private val vpnLoginEntry = "https://webvpn.dlut.edu.cn/login?cas_login=true"
    private val sso = "https://sso.dlut.edu.cn"

    private val jar = mutableListOf<Cookie>()

    private val cookieJar = object : CookieJar {
        override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {
            for (c in cookies) {
                jar.removeAll { it.name == c.name && it.domain == c.domain }
                jar.add(c)
                println("      +cookie ${c.name}@${c.domain} path=${c.path} len=${c.value.length}")
            }
        }

        override fun loadForRequest(url: HttpUrl) = jar.filter { it.matches(url) }
    }

    private val client = OkHttpClient.Builder()
        .protocols(listOf(Protocol.HTTP_1_1))
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(20, TimeUnit.SECONDS)
        .cookieJar(cookieJar)
        .build()

    private fun tokenOf(host: String) = jar.firstOrNull { it.name == "shfb-token" && it.domain == host }?.value
    private fun cookieNames() = jar.joinToString("; ") { "${it.name}@${it.domain}" }

    /** 与 LoginProbeTest 相同的凭据加载:环境变量 DOOR_ACC/DOOR_PW 或项目根 env/probe.txt */
    private fun credentials(): Pair<String, String> {
        var acc = System.getenv("DOOR_ACC").orEmpty()
        var pw = System.getenv("DOOR_PW").orEmpty()
        if (acc.isEmpty() || pw.isEmpty()) {
            val f = listOf("probe.txt", "env", "../probe.txt", "../env", "../../probe.txt", "../../env")
                .map { java.io.File(it) }.firstOrNull { it.exists() }
            if (f != null) {
                for (line in f.readLines()) {
                    val idx = line.indexOf('=')
                    if (idx <= 0) continue
                    val k = line.substring(0, idx).trim().lowercase()
                    val v = line.substring(idx + 1).trim()
                    when (k) {
                        "account", "acc", "user", "username", "door_acc" -> acc = v
                        "password", "pw", "pass", "door_pw" -> pw = v
                    }
                }
                if (acc.isEmpty() || pw.isEmpty()) {
                    val lines = f.readLines().filter { it.isNotBlank() }
                    if (acc.isEmpty() && lines.isNotEmpty() && !lines[0].contains('=')) acc = lines[0].trim()
                    if (pw.isEmpty() && lines.size > 1 && !lines[1].contains('=')) pw = lines[1].trim()
                }
            }
        }
        return acc to pw
    }

    /** GET,可选是否跟随重定向;返回 (code, finalUrl, body) */
    private fun get(url: String, follow: Boolean = true): Triple<Int, String, String> {
        val c = if (follow) client else client.newBuilder().followRedirects(false).build()
        return c.newCall(Request.Builder().url(url).header("User-Agent", "Mozilla/5.0").build())
            .execute().use { r ->
                val body = r.body?.string().orEmpty()
                println("   GET $url ==> ${r.code} final=${r.request.url} len=${body.length}")
                Triple(r.code, r.request.url.toString(), body)
            }
    }

    /** 在给定 CAS 登录页上做 rsa 表单登录,返回跟随 ticket 回跳后的落地 URL */
    private fun casLogin(loginUrl: String, acc: String, pw: String): String {
        val (_, _, page) = get(loginUrl)
        val lt = Regex("name=\"lt\" value=\"([^\"]+)\"").find(page)?.groupValues?.get(1) ?: ""
        val exec = Regex("name=\"execution\" value=\"([^\"]+)\"").find(page)?.groupValues?.get(1) ?: ""
        println("   登录页 lt=${lt.isNotEmpty()} exec=${exec.isNotEmpty()}")
        if (lt.isEmpty() || exec.isEmpty()) return "无登录表单(信任 cookie 已放行)"

        val body = FormBody.Builder()
            .add("rsa", DesCipher.strEnc(acc + pw + lt))
            .add("ul", acc.length.toString())
            .add("pl", pw.length.toString())
            .add("sl", "0")
            .add("lt", lt)
            .add("execution", exec)
            .add("_eventId", "submit")
            .build()
        var loc: String? = null
        client.newBuilder().followRedirects(false).build().newCall(
            Request.Builder().url(loginUrl)
                .header("User-Agent", "Mozilla/5.0")
                .header("Origin", sso)
                .header("Referer", loginUrl)
                .post(body).build()
        ).execute().use { r ->
            val b = r.body?.string().orEmpty()
            loc = r.header("Location")
            println("   POST code=${r.code} Location=$loc len=${b.length}")
            if (loc == null) {
                val err = Regex("id=\"errormsghide\"[^>]*>([^<]{1,200})<").find(b)?.groupValues?.get(1)
                val hints = listOf("短信", "动态码", "验证码", "二次", "密码", "锁定", "失败").filter { b.contains(it) }
                println("   未跳转 err=[$err] 关键词=${hints} 片段=${b.replace(Regex("\\s+"), " ").take(300)}")
            }
        }
        if (loc != null) {
            val (c, u, _) = get(loc!!)
            return "ticket 回跳 code=$c → $u"
        }
        return "未拿到 ticket 跳转"
    }

    @Test
    fun vpnChain() {
        val (acc, pw) = credentials()
        assumeTrue("跳过:未提供 DOOR_ACC/DOOR_PW 或 env/probe.txt", acc.isNotEmpty() && pw.isNotEmpty())
        println("账号长度=${acc.length} 密码长度=${pw.length}")

        println("\n===== 1. WebVPN 登录入口(跟随到 CAS 登录页) =====")
        val (_, casUrl, page1) = get(vpnLoginEntry)
        println("   CAS 页=${page1.contains("name=\"lt\"")} loginFrom=${casUrl.contains("loginFrom=webVPN")}")
        println("   cookies: ${cookieNames()}")

        println("\n===== 2. CAS 表单登录(service 指向网关) =====")
        println("   结果: ${casLogin(casUrl, acc, pw)}")
        println("   cookies: ${cookieNames()}")

        println("\n===== 3. 用 /http/ 路径取门禁首页 =====")
        val (code3, url3, body3) = get(vpnIndex)
        println("   code=$code3 final=$url3 被代理在原域=${url3.startsWith("https://webvpn.dlut.edu.cn")} " +
            "含门禁HTML=${body3.contains("menjin") || body3.contains("门禁")} 是CAS页=${body3.contains("name=\"lt\"")}")
        println("   shfb-token@webvpn=${tokenOf("webvpn.dlut.edu.cn")?.take(12)} " +
            "shfb-token@menjin=${tokenOf("menjin.dlut.edu.cn")?.take(12)}")

        println("\n===== 4. POST 设备列表:/http/ 路径 + 302 原样重发(App 的实现方式) =====")
        listPost("$vpnBase/cser/medium/device/listWithRoom", acc, repost = true)

        println("\n===== 5. 对照:跟随重定向(旧写法,POST 会被降级成 GET) =====")
        listPost("$vpnBase/cser/medium/device/listWithRoom", acc, repost = false)
    }

    private fun listPost(url: String, acc: String, repost: Boolean) {
        val token = tokenOf("webvpn.dlut.edu.cn") ?: tokenOf("menjin.dlut.edu.cn").orEmpty()
        val conditions =
            """{"deviceType":"","deviceCode":"","deviceStatus":"10","sendStatus":"10","mediumType":"","personId":"$acc","orderColumn":"send_status_time","isAsc":false,"attrResults":"doorModel,isOpen,inRair,onLine,doorLock","delStatus":"0","keyword":"","regionCds":""}"""
        val f = FormBody.Builder()
            .add("conditions", conditions)
            .add("currentPage", "1").add("isTop", "true").add("pageSize", "-1")
            .add("projectCd", "DA_LIAN_LI_GONG_MENJIN")
            .add("token", token)
            .build()
        val req = Request.Builder().url(url)
            .header("User-Agent", "Mozilla/5.0")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("AUTH-SIGN", "probe")
            .header("AUTH-TIMESTAMP", System.currentTimeMillis().toString())
            .post(f).build()
        if (!repost) {
            client.newCall(req).execute().use { r ->
                val b = r.body?.string().orEmpty()
                println("   POST $url ==> ${r.code} final=${r.request.url} len=${b.length}")
                println("     body=${b.take(200)}")
            }
            return
        }
        var resp = client.newBuilder().followRedirects(false).build().newCall(req).execute()
        val loc = if (resp.code in 300..399) resp.header("Location") else null
        if (loc != null) {
            val target = req.url.resolve(loc)!!
            resp.close()
            println("   网关 302 → $target,按真实地址原样重发 POST")
            resp = client.newCall(req.newBuilder().url(target).build()).execute()
        }
        resp.use { r ->
            val b = r.body?.string().orEmpty()
            println("   POST $url ==> ${r.code} final=${r.request.url} len=${b.length}")
            println("     body=${b.take(200)}")
        }
    }
}
