package com.dlut.dooropener

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Process
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import com.dlut.dooropener.ui.DoorAppTheme
import com.dlut.dooropener.ui.MainScreen
import com.dlut.dooropener.ui.SettingsScreen

class MainActivity : ComponentActivity() {

    private val vm: MainViewModel by viewModels()

    /** 跳转 App 内网页登录页的标记:这种「离开」不算退出应用 */
    private var launchingWebLogin = false

    /** 跳转外部浏览器(关于页链接)的标记:同样不算退出应用 */
    private var launchingExternal = false

    /** 旋转/系统重建恢复的这一次前台不算「重新打开 App」,不补自动开门 */
    private var restoredFromSavedState = false

    /** 进程启动后的第一次 onResume:冷启动也算「打开 App」 */
    private var firstResume = true

    /** Activity 真正离开过前台(切到别的应用等);下拉通知栏、权限弹窗这类不会触发 onStop */
    private var wentToBackground = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        restoredFromSavedState = savedInstanceState != null
        setContent {
            DoorAppTheme {
                val state by vm.uiState.collectAsState()
                if (state.showSettings) {
                    SettingsScreen(
                        state = state,
                        // 返回箭头:始终回主界面(与「保留后台」开关无关)
                        onBack = { vm.toggleSettings(false) },
                        onAccountChange = vm::onAccountChange,
                        onPasswordChange = vm::onPasswordChange,
                        onDeviceCodeChange = vm::onDeviceCodeChange,
                        onAutoOpenChange = vm::onAutoOpenChange,
                        onKeepBackgroundChange = vm::onKeepBackgroundChange,
                        onUseVpnChange = vm::onUseVpnChange,
                        onFetchDevices = vm::fetchDevices,
                        onSelectDevice = vm::selectDevice,
                        onDismissCandidates = vm::dismissCandidates,
                        onWebLogin = {
                            launchingWebLogin = true
                            startActivity(Intent(this, WebLoginActivity::class.java))
                        },
                        onEditCookies = vm::openCookieEditor,
                        onClearWebCookies = vm::clearWebCookies,
                        onCookieSsoChange = vm::onCookieDraftSsoChange,
                        onCookieMenjinChange = vm::onCookieDraftMenjinChange,
                        onCookieSave = vm::saveCookieEditor,
                        onCookieDismiss = vm::dismissCookieEditor,
                        onDeviceIpChange = vm::onDeviceIpChange,
                        onDevicePinChange = vm::onDevicePinChange,
                        onNewPinChange = vm::onNewPinChange,
                        onBemfaKeyChange = vm::onBemfaKeyChange,
                        onDoorTopicChange = vm::onDoorTopicChange,
                        onCfgTopicChange = vm::onCfgTopicChange,
                        onAckTopicChange = vm::onAckTopicChange,
                        onSyncDevice = vm::syncToDevice,
                        onReadStatus = vm::readDeviceStatus,
                        onDeviceOpen = vm::deviceTestOpen,
                        onAbout = {
                            launchingExternal = true
                            startActivity(
                                Intent(
                                    Intent.ACTION_VIEW,
                                    Uri.parse("https://github.com/114taskforce/DLUT-sesame"),
                                )
                            )
                        },
                    )
                    // 设置页的系统返回键:始终回主界面,不退出程序
                    BackHandler { vm.toggleSettings(false) }
                } else {
                    MainScreen(
                        state = state,
                        onOpenDoor = vm::openDoor,
                        onOpenSettings = { vm.toggleSettings(true) },
                    )
                    // 主界面返回键:保留后台=退回后台;关闭=彻底退出进程
                    BackHandler {
                        if (vm.keepBackgroundEnabled()) {
                            moveTaskToBack(true)
                        } else {
                            finishAndRemoveTask()
                            Process.killProcess(Process.myPid())
                        }
                    }
                }
            }
        }
        // 启动时静默自动登录,提前备好 token(无凭据/最近登录过则跳过)
        vm.ensureTokenFresh()
    }

    override fun onStop() {
        super.onStop()
        wentToBackground = true
    }

    override fun onResume() {
        super.onResume()
        val fromWebLogin = launchingWebLogin
        val fromExternal = launchingExternal
        // 只有冷启动或真的从后台回来才算「重新打开 App」
        val reopened = firstResume || wentToBackground
        firstResume = false
        wentToBackground = false
        launchingWebLogin = false
        launchingExternal = false
        // 从网页登录页返回后刷新信任 cookie 状态
        vm.refreshWebCookieStatus()
        if (fromWebLogin) {
            // 网页登录完成:立即用信任 cookie 强制静默补登录,换新 token
            vm.onWebLoginDone()
        } else {
            // 回到前台时若 token 已过期,后台静默补登录(15 分钟内获取过则跳过)
            vm.ensureTokenFresh()
        }
        // 应用内来回切页面、网页登录/外链返回、旋转重建都不算重新打开 App。
        // MainViewModel 里还有 10 秒去重兜底,连续启动不会重复开门
        if (reopened && !fromWebLogin && !fromExternal && !restoredFromSavedState) {
            vm.autoOpenIfNeeded()
        }
        restoredFromSavedState = false
    }

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        // 「保留后台」关闭时,任意界面按 Home 键也彻底退出进程
        // (跳转网页登录页/外部浏览器时 onUserLeaveHint 也会触发,用标记排除)
        if (!launchingWebLogin && !launchingExternal && !vm.keepBackgroundEnabled()) {
            finishAndRemoveTask()
            Process.killProcess(Process.myPid())
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // 「保留后台」关闭时,任何结束方式(含从最近任务划掉)都彻底结束进程;
        // isFinishing=false 的销毁(如旋转重建)不杀
        if (isFinishing && !vm.keepBackgroundEnabled()) {
            Process.killProcess(Process.myPid())
        }
    }
}
