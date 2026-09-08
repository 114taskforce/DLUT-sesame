# DLUT 门禁 ESP32 — 手机 App 接口文档

**协议版本 fwVer = 1** ｜ 固件对应 `src/prov.cpp`（本地）与 `src/cloud.cpp`（云）｜ 改字段/上限/错误码任一处都要同步改这份文档。

> 这份是**协议契约**（给写 App 的人看）。**怎么一步步配置**——固件、App、巴法云、小爱接入、
> 故障排查——看 [Android/CONFIG.md](../../Android/CONFIG.md)。

设备有两条通道，职责不重叠：

| 通道 | 用途 | 状态 |
|---|---|---|
| **本地 HTTP**（设备自开热点） | 冷启动配网、调试兜底 | 已实现 |
| **云 MQTT**（巴法云） | 日常配置下发、小爱开门、回执 | 已实现（小爱开门已实测通） |

所有文本 UTF-8；所有响应是**单行 JSON**（无换行无缩进）；App 侧解析要按"字段可能缺失"处理（缺 = 该版本没有此项）。

---

## 0. 通用约定

- **PIN**：8–16 位字母数字。既是**配网热点的 WPA2 口令**，也是所有 HTTP/云报文的鉴权码。出厂默认见固件 [src/config.h](../src/config.h) 的 `DEVICE_PIN`；`/save` 可改（改完热点会用新口令重开，手机需重连）。
- **鉴权方式**：HTTP 用查询串 `?k=<PIN>` 或请求头 `X-Auth: <PIN>`。不带或带错 → `401 {"ok":false,"err":"bad-key"}`，**不回显任何配置**。
- **单线程**：固件在 `loop()` 里一次只处理一个请求；`/apply` 会跑完整条登录链（最坏 60~90 秒），期间其它请求排队等待。
- 三个云主题名（T3 用）：`doorTopic` 控制（米家可见）、`cfgTopic` 配置下行（私有）、`ackTopic` 回执（私有）。

---

## 1. 本地 HTTP

### 1.1 连接方式

- 热点名 `DLUT-Door-<MAC 后两字节大写hex>`（例 `DLUT-Door-3FA7`），口令 = PIN，地址 `http://192.168.4.1`。
- 热点**不提供外网**。手机连上它之后，Android/iOS 会自动继续用蜂窝数据上网 —— 所以**连着设备热点的同时仍能在 App 里抓 cookie**，两步不冲突。
- 热点开启条件：① 首次上电且 NVS 里没有 WiFi 配置；② 长按 GPIO0 **8 秒**手动开；③ 无人操作 **10 分钟**后自动关。`GET /status` 的 `ap` 字段可查当前是否开着。

### 1.2 `GET /status` — 探测与健康（只读，可轮询）

```json
{"fwVer":1,"online":true,"wifi":"DLUT-LingShui","ip":"10.6.12.34","campus":"logged-in",
 "tokenAge":412,"tokenTtl":3600,"timeSynced":true,"seedCookies":2,"freeHeap":143260,
 "user":"20241034000","code":"DL-LY-107000","cookieLen":204,"mqtt":"disabled",
 "ap":"on","lastDoor":"ok@12s","balance":"12.34元"}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `online` | bool | STA 是否连上路由器（不代表校园网已认证，看 `campus`） |
| `campus` | str | `logged-in` / `offline`，来自最近一次认证或 30 分钟巡检的**缓存值**，本请求不发探测 |
| `tokenAge`/`tokenTtl` | 秒 | 距上次拿到门禁 token 的时长 / 有效期（1 小时） |
| `timeSynced` | bool | 是否已校时（开门签名的时间戳来源） |
| `seedCookies` | int | 基础 cookie 中实际注入会话的条数（白名单过滤后） |
| `freeHeap` | int | 空闲内部 RAM。**低于 120000 时 App 要提示风险**（TLS 登录链峰值约 250KB/320KB，禁用了 PSRAM） |
| `user`/`code` | str | 设备**当前真正生效**的学号与设备编号（写完后回读校验就靠它） |
| `cookieLen` | int | 已存 cookie 的字节数。**cookie 内容永不回传** |
| `mqtt` | str | 云通道状态：`disabled` / `connecting` / `connected` / `down`（T3 前恒为 `disabled`） |
| `ap` | str | 配网热点 `on`/`off` |
| `lastDoor` | str | `ok@12s` / `no-token@300s` / `no-time@5s` / `rejected@2s` / `ignored@1s` / `none`（@后是距上次的秒数） |
| `balance` | str | 门户返回的"套餐&余额"，中文原样 |

建议超时 3 秒。冷启动时 App 可用它做"设备是否可达"的探测。

### 1.3 `POST /save` — 写字段（落 NVS，断电不丢）

`Content-Type: application/x-www-form-urlencoded`。**只写提交上来的字段**，省略即保持原值。

| 字段 | 上限 | 说明 |
|---|---|---|
| `ssid` | 32 | 目标 WiFi 名 |
| `wifipswd` | 64 | WiFi 密码；`DLUT-LingShui` 是开放网络，传空串 |
| `user` | 20 | 学号，同时是门禁 `personId` |
| `pass` | 36 | 统一身份认证密码 |
| `code` | 20 | 门禁设备编号，如 `DL-LY-107000` |
| `cookie` | 480 | **浏览器整条 `Cookie:` 头原文**，`CASTGC=..; JSESSIONIDCAS=..` 格式。固件按白名单只把 `CASTGC`/`JSESSIONIDCAS` 注入会话，其余名字仍可按名备用（换票失败时用串里的 `shfb-token` 兜底） |
| `pin` | 16 | 改口令（≥8 位，见 0）。值与当前不同才算改：改了固件会用新口令**重开配网热点**，手机要用新口令重连一次；值没变则不重开 |
| `bemfakey` | 32 | 巴法云私钥 |
| `doortopic` | 64 | 控制主题名（须字母数字、后三位为设备类型码，否则米家不可见） |
| `cfgtopic` | 64 | 配置下行主题名 |
| `acktopic` | 64 | 回执主题名 |

成功：

```json
{"ok":true,"saved":["code","cookie"],"errors":{},"needReboot":false}
```

部分字段非法时 `ok:false`，已合法的字段**仍已写入**，逐项原因在 `errors`（长度超限是例外：见下表 `too-long`，整体拒绝）：

```json
{"ok":false,"saved":["code"],"errors":{"pin":"too-short"},"needReboot":false}
```

| `errors.<字段>` / `err` | HTTP | 含义 |
|---|---|---|
| `too-long` | 413（整体拒绝，带 `field`/`max`） | 超上限。例 `{"ok":false,"err":"too-large","field":"cookie","max":480}` |
| `too-short` | 200 | 仅 `pin`：< 8 位，热点开不了 WPA2 |
| `unknown-field` | 200 | 提交了文档里没有的名字（已忽略） |
| `nvs` | 200 | 写 flash 失败，**只在本次运行生效** |
| `no-fields` | 400 | 一个可识别字段都没提交（只提交未知字段也算） |
| `bad-key` | 401 | PIN 不对 |

`needReboot` 恒为 `false`（改完配置用 `/apply` 生效，不需要重启）。建议超时 5 秒。

### 1.4 `POST /apply` — 立即生效（WiFi → 校园网认证 → 校时 → 门禁 token）

改了 `ssid/wifipswd/user/pass/code/cookie` 之后必须调一次，否则新值只在下次上电/巡检时生效。频控 10 秒。建议超时 **90 秒**。

```json
{"ok":false,"steps":{"wifi":true,"campus":true,"time":true,"token":false},
 "detail":{"why":"door-token-failed"},"freeHeap":118904}
```

`ok` = `wifi && campus && token` 全真。`detail.why` 是**第一个致命失败**的原因，取值：

| `why` / `steps` | 含义 | App 建议文案 |
|---|---|---|
| `wifi-not-connected` | 连不上路由器或密码错 | "WiFi 连不上，检查 SSID/密码" |
| `campus-login-failed` | 门户/CAS 认证链失败 | "校园网登录失败，看设备串口第 [n] 步" |
| `time-sync-failed`（且 token 成功） | 校时没成但不致命（`steps.time:false`） | "外网不通，开门时间戳可能不可信" |
| `door-token-failed` | 拿不到门禁 token（多为 CASTGC 过期或需二次认证） | "CASTGC 已过期，请在浏览器重新登录后再抓一次 cookie" |
| `""` | 全部成功 | — |

`steps.*` 里任一步为 `false` 都意味着后续步骤没跑（不是它们自己的错）。

### 1.5 `POST /open` — 试开门（核对设备编号的主通道）

频控 1.5 秒（与按键/门磁共用同一个防抖窗口）。建议超时 25 秒。

```json
{"result":"rejected","source":"prov","resp":"{\"success\":false,\"message\":\"...\"}"}
```

| `result` | 含义 |
|---|---|
| `ok` | 服务端受理，门锁应已动作 |
| `no_token` | 拿不到 token（校园网/CAS 会话失效）。固件会先自动整链重登再试一次，仍失败才回这个 |
| `no_time` | 校时未成功，时间戳签名不可信 |
| `rejected` | 有 token 但被拒：`code` 设备编号错、学号无权限、不在允许时段 |
| `ignored` | 1.5 秒内重复触发，**没有发出请求**（不是失败） |

`resp` 是服务端响应正文前 199 字节（无正文时给响应首部）—— 排错主要看它。

### 1.6 浏览器手工页

`GET /`（无需 PIN）返回一个单页表单，能完成 1.3–1.5 的全部操作；App 没写好之前可用它先验通整条链路。它不含 `pin` 输入项（改口令请用 HTTP）。

---

## 2. 云 MQTT（巴法云）

**2026-09-08 实测**（从 PC 直连 `bemfa.com`）：

| 项 | 结论 |
|---|---|
| 地址 | **`mqtt.bemfa.com:9501`**（控制台给的地址）。`bemfa.com:9501` 实测同集群、行为一致；`1883` 是**关闭**的（网上教程多写 1883，别照抄）。加密口 `9503`(TLS)，wss `9504` path `/wss` |
| 鉴权 | 标准 MQTT 3.1.1，**clientId = 用户私钥**，不带用户名密码 → `CONNACK rc=0` |
| 主题 | 必须先在[控制台](https://cloud.bemfa.com)创建并选 **MQTT 协议类型**，否则 `SUBACK` 返回 **`0x80`**；建对的裸主题实测返回 `0x00` |
| `/set`、`/up` | **只能用于发布，订阅一定被拒（`0x80`）** —— 别把它当成"主题没建"。设备只订阅裸主题 |
| 私有 TCP | 另有 `8344`（键值对文本协议，`cmd=1/2/3/7/9`，`&` 分隔、`\r\n` 结尾，`cmd=7&type=3` 还能拿服务器时间戳）。本方案不用它：米家只同步 MQTT 类型的主题 |
| 心跳 | keepalive 60 秒，服务器 65 秒无心跳判掉线；发任何报文都算心跳 |

设备端**明文连接，不用 MQTTS**（再叠一条 TLS 会话要吃 40KB+ 内部 RAM，本项目禁用了 PSRAM）。App 侧建议也走 9501 明文（同一账号），或用巴法 App/HTTP API。

### 2.1 控制与状态（小爱同学走这条）

| 方向 | 主题 | payload | 说明 |
|---|---|---|---|
| App/小爱 → 设备 | `doorTopic` | `on` | 开门。`off` 只回状态不动作 |
| 设备 → 云 | `doorTopic/up` | `on`/`off` | `/up` = 只更新云端数据不回推（`/set` 才是推送给其它订阅者）。失败一律 `off`，米家不会误留"已开" |

重复下发由设备端 1.5 秒防抖吞掉（`DOOR_MIN_INTERVAL`），App 不必自己去重。若米家里的开关状态不跟着翻，把设备上报主题从 `doorTopic/up` 换成裸 `doorTopic` 试（固件 `cloud.cpp` 的 `handleDoorMsg` 一行）。

### 2.2 配置下发（分帧）

App → `cfgTopic`，一帧一条 MQTT 消息，逗号分隔 7 段：

```
c,<pin>,<msgId>,<total>,<seq>,<crc8>,<chunk>
```

- 整条配置 = JSON `{"user":"...","pass":"...","code":"...","cookie":"..."}`（**只放要改的键**）→ UTF-8 → **Base64url（无 `=` 填充）** → 按 `CFG_CHUNK=96` 字符切片。
- `msgId` 4 位 hex，App 每次下发自选；`total`/`seq` 两位十进制，`seq` 从 `01` 起；`crc8` = 对**原始 JSON 字节**的 CRC-8（多项式 0x07），两位 hex，所有帧相同。
- 单帧约 124 字节（定长前缀 + 96 字符 chunk）。**除末帧外每帧必须正好 96 字符**，否则设备回 `cfg-err,<msgId>,unaligned`。
- 帧序可乱序（设备按 `seq` 定长偏移装配）；**20 秒**没收齐则丢弃并回 `cfg-err,<msgId>,timeout`，App 收到后整套重发。
- **不要用 retain**：设备重启后订阅会立刻收到旧的 retain 消息，导致重放旧 cookie。
- 例（第 3 帧）：`c,47124712,a3f1,06,03,9c,VXNlciI6IjIwMjQxMDM0MDAw...`

设备校验：PIN 不符 → **静默丢弃**（不回错，避免主题名与口令被试探）；装配超时 → `timeout`；CRC 不符 → `crc`。成功则落 NVS 并**自动执行一次 1.4 的 apply**（App 不用另外发命令）。

### 2.3 回执（App 订阅 `ackTopic` 裸主题）

设备用普通 publish（不带 `/up`、`/set` 后缀）发到 `ackTopic`，这样订阅它的 App 能收到推送：

```
cfg-ok,<msgId>,<savedKeys逗号分隔>      # 例 cfg-ok,a3f1,cookie
cfg-err,<msgId>,<code>                  # timeout | crc | pin | too-long | unaligned | empty | full
apply-ok,token                          # 配置写入后自动跑的整链认证成功
apply-err,<step>,<why>                  # step ∈ campus|token，why 同 1.4 取值
door-ok | door-err,<code>               # no-token | rejected | no-time | ignored
```

App 判定"写入成功"的**唯一依据**是收到 `cfg-ok,<同一个msgId>`。没收到就等 `cfg-err` 或超时后整套重发（建议最多 3 次）。

### 2.4 建议时序

1. 发全部分帧（帧间隔 ≥150ms）→ 等 `cfg-ok`（设备装配超时是 20 秒，所以 App 等 **25 秒**再判失败，整套重发，最多 3 次）
2. 设备随后自己跑整链认证，等 `apply-ok,token`（≤90 秒）
3. 可选自检：发 `doorTopic` = `on`，等 `door-ok`
4. 任一步失败 → 按 1.4 的文案表提示，并引导用户连设备热点看 `/apply` 的 `detail` 与串口日志

### 2.5 小爱/米家绑定

米家 App → 我的 → **其他平台设备** → 添加 → 选「巴法」→ 填巴法云账号 → 设备自动同步 → 改名为"宿舍门"。之后"小爱同学，打开宿舍门"。主题名不合规则（非字母数字 / 后三位不是设备类型码）在米家里看不见。

---

## 3. 手工联调（不写 App 也能验完整条链）

```bash
curl "http://192.168.4.1/status?k=47124712"
# cookie 里有空格和分号, 一定要 URL 编码(--data-urlencode)
curl -X POST "http://192.168.4.1/save?k=47124712" \
     --data-urlencode "code=DL-LY-107000" \
     --data-urlencode "cookie=CASTGC=TGT-xxxx-cas; JSESSIONIDCAS=yyyy"
curl -X POST -H "X-Auth: 47124712" "http://192.168.4.1/apply"     # 最坏等 90 秒
curl -X POST -H "X-Auth: 47124712" "http://192.168.4.1/open"
```

云侧（不写 App 也能验）：用 MQTTX 连 `mqtt.bemfa.com:9501`，**ClientID 填巴法私钥、用户名密码留空**，订阅 `ackTopic`，再向 `cfgTopic` 逐帧 publish 2.2 的报文；20 秒内应收到 `cfg-ok`。设备侧串口会打 `[云] 巴法云已连接` / `[云] <- 主题 : 报文`。

---

## 4. 状态与待办

已实测确定（2026-09-08，PC 直连）：地址 `mqtt.bemfa.com:9501`（`1883` 关闭）、MQTT 3.1.1 + clientId=私钥 鉴权通过（`CONNACK rc=0`）、裸主题订阅已授权（控制主题 `Qby0GQH72001` 后三位 `001` 合规）。

还差这几件事：

1. **再建两个 MQTT 主题**给配置下行与回执（名字随意、字母数字），建好后写入设备：`/save` 的 `cfgtopic`/`acktopic`，或 App 走云配置帧。**没建也不影响开门**，只是收不到 `cfg-ok` 回执。
2. **米家绑定后确认设备可见且能翻状态** —— 翻不动就把 2.1 说的 `/up` 改成裸主题。
3. **设备端跑起来**：巴法控制台里主题的"在线"只有当有客户端连上才会亮；固件没烧录前，米家里最多看到一个始终离线的插座设备。
4. **校园网里 9501 出站是否放行**（PC 上通的，不代表宿舍网也通）；不通就退回本地 HTTP 那条通道。
