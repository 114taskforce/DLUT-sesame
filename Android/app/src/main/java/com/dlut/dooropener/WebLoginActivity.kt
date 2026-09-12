package com.dlut.dooropener

import android.annotation.SuppressLint
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.webkit.CookieManager
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.dlut.dooropener.data.SettingsStore
import com.dlut.dooropener.ui.DoorAppTheme

/**
 * 网页登录页:在 WebView 中完成 CAS 登录(含二次认证),
 * 登录成功后自动抓取 cookie 并保存(固件 COOKIE_INPUT 等价)。
 *
 * 两种入口(由 Intent 的 [EXTRA_VPN] 选择):
 * - 校园网直连:直接打开 CAS 登录页,登录后落在 menjin.dlut.edu.cn;
 * - 校外 WebVPN:打开门户 dashboard,登录完成后自动跳到网关里的门禁页——
 *   校外时门禁会话只存在于 webvpn 域(由网关的客户端 JS 写入),只有浏览器能拿到,
 *   所以必须用 WebView 走一遍再把 cookie 抓进 App。
 */
class WebLoginActivity : ComponentActivity() {

    companion object {
        const val TAG = "DoorClient"

        /** 是否走 WebVPN(校外)登录:由调用方通过 Intent 传入 */
        const val EXTRA_VPN = "vpn"

        /** 校外入口:WebVPN 门户(登录后门户里才有资源) */
        const val VPN_DASHBOARD = "https://webvpn.dlut.edu.cn/login#/dashboard"

        /**
         * 门禁站点在网关里的地址:登录完成后自动跳到这里(用户实测登录后落地的就是 mine.html),
         * 让网关的 JS 把门禁会话写进 cookie
         */
        const val VPN_MENJIN_PAGE =
            "https://webvpn.dlut.edu.cn/http/57787a7876706e323032336b65794024751d0c12f50ddd4aa659ee7694bf90698d72/cser/static/menjin/mine.html#/dashboard"
    }

    private var vpnMode = false

    /** 门户登录完成后跳门禁页的次数上限,避免门禁页反复弹回登录页时来回跳 */
    private var doorNavAttempts = 0

    private var finished = false
    private val handler = Handler(Looper.getMainLooper())
    private var pendingHarvest: Runnable? = null
    private var webView: WebView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vpnMode = intent.getBooleanExtra(EXTRA_VPN, false)
        setContent {
            DoorAppTheme {
                WebLoginScreen(
                    startUrl = if (vpnMode) VPN_DASHBOARD else
                        "https://sso.dlut.edu.cn/cas/login?service=http://menjin.dlut.edu.cn/cser/static/menjin/index.html",
                    hint = if (vpnMode)
                        "校外(VPN)登录:先在门户完成统一身份认证(要短信就按提示过一次),App 会自动打开门禁页并记录会话"
                    else
                        "请在网页中完成登录(含二次认证)。登录时请勾选「信任此设备」,之后 App 自动登录不再需要二次认证;登录成功跳转到门禁页后自动记录 Cookie",
                    onBack = { finish() },
                    onPageFinished = { url ->
                        val host = try { Uri.parse(url).host } catch (e: Exception) { null }
                        when {
                            // 校园网直连:已到门禁站说明登录链完成、cookie 已下发;
                            // 优先等 mine.html 面板渲染完成再抓取(用户能看到页面,避免白屏)
                            host == "menjin.dlut.edu.cn" ->
                                scheduleHarvest(if (url.contains("/cser/static/menjin/mine.html")) 600L else 2500L)

                            // 校外:门户登录完(落到门户页)后自动进入门禁资源页
                            vpnMode && host == "webvpn.dlut.edu.cn" && !url.contains("/cser/") &&
                                !finished && doorNavAttempts < 3 -> {
                                doorNavAttempts++
                                webView?.postDelayed({ webView?.loadUrl(VPN_MENJIN_PAGE) }, 1500)
                            }

                            // 校外:门禁页已在网关里加载,留时间让网关 JS 与 SPA 落地 cookie 再抓
                            vpnMode && url.contains("/cser/") -> scheduleHarvest(5000L)
                        }
                    },
                    onWebViewCreated = { webView = it },
                )
            }
        }
    }

    /** 延迟抓取:先让页面渲染出来再关闭,避免「白屏+直接弹成功」;多次触发以最后一次为准 */
    private fun scheduleHarvest(delayMs: Long) {
        if (finished) return
        pendingHarvest?.let { handler.removeCallbacks(it) }
        val r = Runnable { harvestAndFinish() }
        pendingHarvest = r
        handler.postDelayed(r, delayMs)
    }

    /** 抓取三个域的 cookie 并保存(等价于固件 COOKIE_INPUT;校外时关键的那份在 webvpn 域) */
    private fun harvestAndFinish() {
        if (finished) return
        finished = true
        pendingHarvest?.let { handler.removeCallbacks(it) }
        val cm = CookieManager.getInstance()
        cm.flush()
        // 注意:getCookie 只返回路径匹配的 cookie;CASTGC 的 Path=/cas,
        // 必须用 /cas/login 路径的 URL 才能拿到,根路径 URL 拿不到
        val sso = mergeCookies(
            cm.getCookie("https://sso.dlut.edu.cn/cas/login"),
            cm.getCookie("https://sso.dlut.edu.cn/"),
        )
        val menjin = mergeCookies(
            cm.getCookie("http://menjin.dlut.edu.cn/cser/static/menjin/index.html"),
            cm.getCookie("http://menjin.dlut.edu.cn/"),
        )
        // 校外(VPN)时门禁会话落在 webvpn 域:网关的客户端 JS 把门禁的 cookie 写在自己域下
        val vpn = mergeCookies(
            cm.getCookie(VPN_MENJIN_PAGE),
            cm.getCookie("https://webvpn.dlut.edu.cn/"),
        )
        val store = SettingsStore(this)
        store.webCookieSso = sso
        store.webCookieMenjin = menjin
        store.webCookieVpn = vpn
        Log.i(
            TAG,
            "网页登录抓取: sso含CASTGC=${sso.contains("CASTGC")} menjin含token=${menjin.contains("shfb-token")} " +
                "webvpn会话=${vpn.isNotEmpty()}(${vpn.length}字节)",
        )
        if (sso.isEmpty() && menjin.isEmpty() && vpn.isEmpty()) {
            Toast.makeText(this, "未获取到 Cookie,请确认已登录成功", Toast.LENGTH_LONG).show()
        } else {
            Toast.makeText(
                this,
                when {
                    sso.contains("CASTGC") -> "登录成功,信任 Cookie 已记录"
                    vpn.isNotEmpty() -> "已记录 WebVPN 会话 Cookie"
                    else -> "Cookie 已记录(未含 CASTGC,请确认登录完成)"
                },
                Toast.LENGTH_LONG,
            ).show()
        }
        finish()
    }

    /** 用户手动返回时兜底:若已登录拿到 cookie 但没触发抓取,也保存 */
    override fun onDestroy() {
        super.onDestroy()
        // 退出网页登录页时清空 WebView 页面缓存(应用体积大头;Cookie 不受影响)
        webView?.clearCache(true)
        clearWebViewMetricsFiles()
        pendingHarvest?.let { handler.removeCallbacks(it) }
        if (finished) return
        val cm = CookieManager.getInstance()
        cm.flush()
        val sso = mergeCookies(
            cm.getCookie("https://sso.dlut.edu.cn/cas/login"),
            cm.getCookie("https://sso.dlut.edu.cn/"),
        )
        val menjin = mergeCookies(
            cm.getCookie("http://menjin.dlut.edu.cn/cser/static/menjin/index.html"),
            cm.getCookie("http://menjin.dlut.edu.cn/"),
        )
        val vpn = mergeCookies(
            cm.getCookie(VPN_MENJIN_PAGE),
            cm.getCookie("https://webvpn.dlut.edu.cn/"),
        )
        if (sso.contains("CASTGC") || menjin.contains("shfb-token") || vpn.isNotEmpty()) {
            val store = SettingsStore(this)
            store.webCookieSso = sso
            store.webCookieMenjin = menjin
            store.webCookieVpn = vpn
            Log.i(
                TAG,
                "退出兜底: Cookie 已保存 sso含CASTGC=${sso.contains("CASTGC")} " +
                    "menjin含token=${menjin.contains("shfb-token")} webvpn会话=${vpn.isNotEmpty()}",
            )
        } else {
            Log.w(TAG, "退出兜底: 未发现 CASTGC/shfb-token,登录可能未完成,不保存")
        }
    }

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        // 「保留后台」关闭时,网页登录页按 Home 键同样彻底退出
        if (!SettingsStore(this).keepBackground) {
            webView?.clearCache(true)
            clearWebViewMetricsFiles()
            finishAndRemoveTask()
            Process.killProcess(Process.myPid())
        }
    }

    /**
     * 删除系统 WebView 组件写进应用目录的统计文件(BrowserMetrics-*.pma)。
     * 该文件由 WebView 提供方进程维护,无公开 API 可阻止其生成,
     * 只能在 WebView 用完后删掉,不让它常驻占用空间。
     */
    private fun clearWebViewMetricsFiles() {
        val targets = listOfNotNull(
            noBackupFilesDir,
            filesDir,
            filesDir?.parentFile?.let { java.io.File(it, "app_webview") },
        )
        for (dir in targets) {
            try {
                dir.listFiles()?.forEach { f ->
                    if (f.name.startsWith("BrowserMetrics")) f.delete()
                }
            } catch (e: Exception) {
                // 文件被 WebView 进程占用时忽略,下次退出再清
            }
        }
    }

    /** 合并多条 cookie 串(去重,保序) */
    private fun mergeCookies(vararg parts: String?): String {
        val map = LinkedHashMap<String, String>()
        for (p in parts) {
            if (p.isNullOrEmpty()) continue
            for (pair in p.split(";")) {
                val idx = pair.indexOf("=")
                if (idx <= 0) continue
                map[pair.substring(0, idx).trim()] = pair.substring(idx + 1).trim()
            }
        }
        return map.entries.joinToString("; ") { "${it.key}=${it.value}" }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun WebLoginScreen(
    startUrl: String,
    hint: String,
    onBack: () -> Unit,
    onPageFinished: (String) -> Unit,
    onWebViewCreated: (WebView) -> Unit,
) {
    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = { Text("网页登录", fontWeight = FontWeight.Bold) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.primary,
                    titleContentColor = Color.White,
                    navigationIconContentColor = Color.White,
                ),
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
        ) {
            Text(
                text = hint,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            )
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { context ->
                    createWebView(context, startUrl, onPageFinished).also(onWebViewCreated)
                },
            )
        }
    }
}

@SuppressLint("SetJavaScriptEnabled")
private fun createWebView(
    context: android.content.Context,
    startUrl: String,
    onPageFinished: (String) -> Unit,
): WebView {
    return WebView(context).apply {
        settings.javaScriptEnabled = true
        settings.domStorageEnabled = true
        webViewClient = object : WebViewClient() {
            override fun onPageFinished(view: WebView?, url: String?) {
                url?.let(onPageFinished)
            }
        }
        loadUrl(startUrl)
    }
}
