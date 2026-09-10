package com.dlut.dooropener.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.dlut.dooropener.StatusKind
import com.dlut.dooropener.UiState

/**
 * 设置页:账号、密码、门锁编号(可自动获取)、自动开门开关
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    state: UiState,
    onBack: () -> Unit,
    onAccountChange: (String) -> Unit,
    onPasswordChange: (String) -> Unit,
    onDeviceCodeChange: (String) -> Unit,
    onAutoOpenChange: (Boolean) -> Unit,
    onKeepBackgroundChange: (Boolean) -> Unit,
    onUseVpnChange: (Boolean) -> Unit,
    onFetchDevices: () -> Unit,
    onSelectDevice: (String) -> Unit,
    onDismissCandidates: () -> Unit,
    onWebLogin: () -> Unit,
    onEditCookies: () -> Unit,
    onClearWebCookies: () -> Unit,
    onCookieSsoChange: (String) -> Unit,
    onCookieMenjinChange: (String) -> Unit,
    onCookieSave: () -> Unit,
    onCookieDismiss: () -> Unit,
    onDeviceIpChange: (String) -> Unit,
    onDevicePinChange: (String) -> Unit,
    onNewPinChange: (String) -> Unit,
    onBemfaKeyChange: (String) -> Unit,
    onDoorTopicChange: (String) -> Unit,
    onCfgTopicChange: (String) -> Unit,
    onAckTopicChange: (String) -> Unit,
    onSyncDevice: () -> Unit,
    onReadStatus: () -> Unit,
    onDeviceOpen: () -> Unit,
    onAbout: () -> Unit,
) {
    var showBemfa by remember { mutableStateOf(false) }
    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = { Text("设置") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回")
                    }
                },
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            OutlinedTextField(
                value = state.account,
                onValueChange = onAccountChange,
                label = { Text("账号") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = state.password,
                onValueChange = onPasswordChange,
                label = { Text("密码") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = state.deviceCode,
                onValueChange = onDeviceCodeChange,
                label = { Text("门锁编号(留空开门时自动补全)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = onFetchDevices,
                enabled = !state.fetchingDevices,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(if (state.fetchingDevices) "正在获取…" else "自动获取门锁编号")
            }
            Text(
                text = state.status,
                style = MaterialTheme.typography.bodySmall,
                color = when (state.statusKind) {
                    StatusKind.FAIL -> MaterialTheme.colorScheme.error
                    else -> MaterialTheme.colorScheme.onSurfaceVariant
                },
            )

            HorizontalDivider()

            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("自动开门", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "打开 APP 时自动触发一次开门",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(checked = state.autoOpen, onCheckedChange = onAutoOpenChange)
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("保留后台", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "关闭后,退出应用时彻底结束进程,不在后台运行",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(checked = state.keepBackground, onCheckedChange = onKeepBackgroundChange)
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("使用 WebVPN", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "开启后先登录 webvpn.dlut.edu.cn 再访问门禁(校外/不在校园网时打开);" +
                            "关闭时直连门禁,不尝试打开 VPN",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Switch(checked = state.useVpn, onCheckedChange = onUseVpnChange)
            }

            HorizontalDivider()

            Text("二次认证(信任设备)", style = MaterialTheme.typography.titleMedium)
            Text(
                if (state.webCookieRecorded)
                    "已记录信任 Cookie:自动登录直接放行,token 超 15 分钟自动续取;「清除」会连同活动会话一起清掉(等效登出)"
                else
                    "未记录。若开门提示需要二次认证,请打开网页登录一次",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onWebLogin,
                    modifier = Modifier.weight(1f),
                ) {
                    Text(if (state.webCookieRecorded) "重新网页登录" else "打开网页登录")
                }
                OutlinedButton(onClick = onEditCookies) { Text("编辑") }
                if (state.webCookieRecorded) {
                    OutlinedButton(onClick = onClearWebCookies) { Text("清除") }
                }
            }

            HorizontalDivider()

            // ==================== 同步到 ESP32 设备 ====================
            Text("同步到门禁设备(ESP32)", style = MaterialTheme.typography.titleMedium)
            Text(
                "一次把账号密码、门锁编号、信任 Cookie、巴法云参数写给设备并让它立即重新认证;" +
                    "别人拿到设备只需连热点 + 填口令 + 点同步。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = state.deviceIp,
                    onValueChange = onDeviceIpChange,
                    label = { Text("设备地址") },
                    singleLine = true,
                    modifier = Modifier.weight(1.3f),
                )
                OutlinedTextField(
                    value = state.devicePin,
                    onValueChange = onDevicePinChange,
                    label = { Text("设备口令") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.weight(1f),
                )
            }
            OutlinedTextField(
                value = state.newPin,
                onValueChange = onNewPinChange,
                label = { Text("改设备口令(选填,≥8 位)") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                "长按设备按键 8 秒开热点,连上 DLUT-Door-xxxx 后设备地址就是 192.168.4.1;" +
                    "该热点不提供外网,手机照常走蜂窝数据。口令即热点密码(固件出厂默认 47124712)。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = onSyncDevice,
                    enabled = !state.syncing,
                    modifier = Modifier.weight(1.4f),
                ) {
                    Text(if (state.syncing) "请稍候…" else "同步到设备")
                }
                OutlinedButton(
                    onClick = onReadStatus,
                    enabled = !state.syncing,
                    modifier = Modifier.weight(1f),
                ) { Text("读状态") }
                OutlinedButton(
                    onClick = onDeviceOpen,
                    enabled = !state.syncing,
                    modifier = Modifier.weight(1f),
                ) { Text("试开门") }
            }
            if (state.deviceSummary.isNotBlank()) {
                Text(
                    state.deviceSummary,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            // 巴法云参数:只有要接小爱同学时才需要填,默认收起
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { showBemfa = !showBemfa }
                    .padding(vertical = 6.dp),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("巴法云(小爱同学)", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        if (state.doorTopic.isNotBlank()) "已填:${state.doorTopic}"
                        else "不接语音助手可留空(留空的字段不会被推送,不会覆盖设备现值)",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(if (showBemfa) "▲" else "▼", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (showBemfa) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = state.bemfaKey,
                        onValueChange = onBemfaKeyChange,
                        label = { Text("巴法云私钥") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = state.doorTopic,
                        onValueChange = onDoorTopicChange,
                        label = { Text("控制主题(后三位须是设备类型码,如 001)") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = state.cfgTopic,
                            onValueChange = onCfgTopicChange,
                            label = { Text("配置主题") },
                            singleLine = true,
                            modifier = Modifier.weight(1f),
                        )
                        OutlinedTextField(
                            value = state.ackTopic,
                            onValueChange = onAckTopicChange,
                            label = { Text("回执主题") },
                            singleLine = true,
                            modifier = Modifier.weight(1f),
                        )
                    }
                    Text(
                        "三个主题都要先在巴法控制台创建、协议类型选 MQTT。控制台给的连接地址是 " +
                            "mqtt.bemfa.com:9501(1883 不可用),设备用私钥当 clientId 连上去。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            HorizontalDivider()

            // 关于:项目主页链接
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onAbout)
                    .padding(vertical = 8.dp),
            ) {
                Column(Modifier.weight(1f)) {
                    Text("关于", style = MaterialTheme.typography.bodyLarge)
                    Text(
                        "GitHub: 114taskforce/DLUT-sesame",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    "↗",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }

    state.deviceCandidates?.let { candidates ->
        DevicePickerDialog(
            candidates = candidates,
            onSelect = onSelectDevice,
            onDismiss = onDismissCandidates,
        )
    }

    if (state.showCookieEditor) {
        CookieEditorDialog(
            draftSso = state.cookieDraftSso,
            draftMenjin = state.cookieDraftMenjin,
            onChangeSso = onCookieSsoChange,
            onChangeMenjin = onCookieMenjinChange,
            onSave = onCookieSave,
            onDismiss = onCookieDismiss,
        )
    }
}

/** 信任 Cookie 编辑器:直接查看/粘贴 sso 与 menjin 两段 cookie(等价于固件 COOKIE_INPUT) */
@Composable
private fun CookieEditorDialog(
    draftSso: String,
    draftMenjin: String,
    onChangeSso: (String) -> Unit,
    onChangeMenjin: (String) -> Unit,
    onSave: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("编辑信任 Cookie") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    "格式:name=value; name2=value2(分号分隔),可整段粘贴。\n" +
                        "sso 域建议包含 CASTGC,menjin 域建议包含 shfb-token",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                OutlinedTextField(
                    value = draftSso,
                    onValueChange = onChangeSso,
                    label = { Text("sso.dlut.edu.cn Cookie") },
                    minLines = 3,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = draftMenjin,
                    onValueChange = onChangeMenjin,
                    label = { Text("menjin.dlut.edu.cn Cookie") },
                    minLines = 3,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = onSave) { Text("保存") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("取消") }
        },
    )
}

@Composable
private fun DevicePickerDialog(
    candidates: List<String>,
    onSelect: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("选择门锁编号") },
        text = {
            LazyColumn(Modifier.heightIn(max = 320.dp)) {
                items(candidates) { code ->
                    Text(
                        text = code,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onSelect(code) }
                            .padding(vertical = 12.dp),
                    )
                    HorizontalDivider()
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("取消") }
        },
    )
}
