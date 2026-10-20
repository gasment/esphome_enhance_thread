# ESPHome thread_br — Thread Border Router

独立 ESPHome 外部组件，目标为 ESP32-S3 + ESP32-H2 UART RCP。
参考本仓库 `esp-thread-br-main`，底层复用 ESP-IDF 的 OpenThread 和 BR 库。

## 版本与构建

- ESPHome：2026.9.1；本版依赖其 native ESP-IDF 构建接口。
- ESP-IDF：5.5.x，示例固定 5.5.5。
- S3：8MB Flash；当前主配置要求 Octal 80MHz PSRAM。
- H2：使用同版本 IDF 的 `examples/openthread/ot_rcp` 构建并手工烧录。
  保存 H2 二进制、IDF 版本、sdkconfig 和校验值，后续升级 S3 时重新验证兼容性。
  H2 的 IO 配置和构建操作见根目录 `h2_rcp/README.md`。

```powershell
esphome config esphome-thread-br/config.yaml
esphome compile esphome-thread-br/config.yaml
esphome upload esphome-thread-br/config.yaml
```

初次通过 S3 USB 烧录，之后可以使用 ESPHome OTA。应用沿用 ESPHome 的双 OTA
分区；官方 Web 静态资源已 gzip 压缩嵌入应用，没有 SPIFFS/Web/RCP 固件分区。
OTA 与回滚同时更新页面和后端。本组件不刷写 H2。

将 thread_br 放入项目的 external_components，并按需加入 thread_gateway；在 external_components.components 中列出所用组件，不依赖
原始官方源码目录。可编辑的简体中文页面位于 `thread_br_web/frontend/`，修改页面后
运行 `python tools/build_web_assets.py` 重新生成随组件分发的 `web_assets.h`；
此命令只打包 Web 资源，不生成或覆盖 C 接口源码。保留了 Espressif 的 Apache-2.0
许可证和文件版权声明，`thread_br_web/upstream-sha256.json` 记录输入文件哈希。

## 工作范围

- ESPHome 管理 Wi-Fi、AP 配网、Native API、S3 OTA、mDNS 主机名、日志和 LED。
- 组件绑定 ESPHome 的 `WIFI_STA_DEF` 为 backbone，不另建 Wi-Fi 连接。
- OpenThread 独占配置的硬件 UART，无需也不能再为这些引脚声明 `uart:`。
- RESET/BOOT 用于正常启动和故障恢复，使用开漏输出；正常运行释放 BOOT 为高电平，
  允许手动 BOOT/RESET 按钮拉低。
- 使用预定义 Dataset，无需网络扫描、Joiner/Commissioner、接入码或逐台授权。
- 保留 IDF BR 的 SRP/DNS-SD 代理，使 Thread 节点的 ESPHome 服务可在 LAN 发现。
  不加载 ESPHome 终端侧的 `openthread:`，不定义其 `USE_OPENTHREAD` 宏。
- 禁用 TREL，首版保留 IDF BR 默认网络功能，重点验证 IPv6 局域网路由。

初始化和 RCP 探测在后台任务执行。诊断实体发布在 ESPHome 主循环，避免跨任务
发布状态。Spinel 故障沿用 IDF 的恢复机制，每次 S3 启动最多进行 3 次物理复位恢复。
IDF 内部不可恢复的 Spinel 初始化/兼容性错误仍可能触发 panic；本版没有改写
底层 IDF 的断言和错误策略，也不承诺不兼容或未烧录的 H2 能使 S3 正常运行。

## Thread UDP 接收端

可选独立组件 thread_gateway（通过 thread_br_id 引用本组件） 提供节点发现、地址 TTL、UDP 状态接收、NVS 注册表和网关 Native API 实体暴露。
顶部标签栏“网络拓扑”右侧“网关管理”进入 /thread_gateway，沿用原 Basic 认证，按节点管理。
只读实体可选接入 HA，控制实体仅限 Web 操作；保存清单并确认重启后应用。
节点端使用 [thread_sed_transporter](../thread_sed_transporter/README.md) 进行手动采样和 UDP 上报。
配置范围、元数据限制和 UDP v1 协议见 [thread_gateway 说明](../thread_gateway/README.md)。

## Dataset 与 ESP32 节点

`esphome-thread-br/thread-br.yaml` 与 `esphome/thread-node.example.yaml` 提供对应示例。
BR 的网络参数位于 `thread_br.thread_network_config`；终端侧仍使用原生
`openthread:` 格式。
节点示例现已通过 `external_components` 加载本项目维护的
[openthread / openthread_info fork](../openthread/README.md)，修复凭据前导零和 OMR
地址筛选；S3 本身不加载这两个节点组件。两端的配置、组件和框架版本应配套验证。
ESP32-C6/H2 等具有 802.15.4 的节点，可在 ESPHome `openthread:` 下分项预设
`channel`、`network_name`、`network_key`、`pan_id`、`ext_pan_id`、`pskc`、
`mesh_local_prefix`。两端使用相同参数，节点自行附着到网络。

示例凭据仅供开发，部署前修改 BR 和节点的整组参数。密钥和 PSKc 支持
`0x` 数字及精确长度的十六进制字符串，保留前导零；推荐使用 `!secret`。
不会在组件日志或诊断实体中输出密钥。经过认证的 Web 接口按官方行为可读取凭据。

- `thread_network_config.force_dataset: true`：每次启动应用固件中预定义参数，适合本项目。
- `thread_network_config.force_dataset: false`（默认）：优先恢复 NVS 中已有 Dataset；没有时应用预定义参数。
  只改 YAML 并不会覆盖已经保存的网络，应临时启用 `force_dataset` 再更新固件。

网络参数通过配置在构建时写入 IDF，本版不提供运行时修改入口。
`device_type: FTD` 是 Thread 路由能力，不代表每台终端必然担任 Router。

## 接线和无线共存

固定 S3 UART1 ↔ H2 UART0，460800/8N1，无硬件流控；默认 TX18/RX17 ↔ RX23/TX24。
S3 GPIO7 ↔ H2 EN，S3 GPIO8 ↔ H2 GPIO9。与 `hardware/io_used.md` 一致。
组件在 Thread 初始化时接管这两根线，不等待 Wi-Fi 连接；接管前依靠 H2 自身的 EN/BOOT 电路。
接管时先预置输出锁存值为 1，再启用开漏输出及弱上拉，避免输出开启时短暂拉低。
正常运行两线均写 1（释放），不以推挽方式驱动高电平；手动 EN/BOOT 按钮仍可拉低。
BR 初始化会主动复位 H2 一次：BOOT 保持释放，EN 拉低 20ms，再释放并等待 100ms。
运行故障恢复也使用此时序。H2 可独立启动，但当前 S3 启动流程并非完全不干预 H2。
logger 应使用 USB 或其他 UART，不能占用 UART1。UART 编号和波特率不暴露到 YAML。
UART1 是控制器编号，`rcp.uart_tx` / `rcp.uart_rx` 仍决定 GPIO Matrix 引脚，未被忽略。
随机网络参数生成见 `tools/README.md`，H2 YAML 构建工具见 `h2_rcp/README.md`。

本仓库定制 H2 支持 GPIO22 绿灯显示 Host UART 活动、GPIO25 蓝灯显示连接状态。
LED 控制为组件基础功能，无需额外启用；组件在约 5 秒一次的后台 Spinel 探测后
发送连接心跳，心跳不修改使能状态。模板开关示例见 `esphome-thread-br/thread-br.yaml`。
运行时可调用 `set_rcp_activity_led_enabled(bool)` 和
`set_rcp_connection_led_enabled(bool)`；这些方法仅排队发送动作。
对应 `get_...` 方法返回 `optional<bool>`，仅使用 H2 已上报的实际使能值；
无有效报告或报告过期时返回空值。模板开关使用 `restore_mode: DISABLED`、
`optimistic: false`，S3 不保存、恢复或在 H2 复位后重放 LED 使能设置。
H2 自身在 NVS 中持久化 `led_enabled`（位 0 为 Activity，位 1 为 Connection），启动读取后独立控制 GPIO。
S3 的 SET 指令由 H2 队列处理，只有 NVS 提交成功后才应用并上报对应动作序号；保存失败不会假确认，S3 按原规则重试。
H2 故障逻辑优先：未收到健康心跳或心跳超时，蓝灯强制闪烁；恢复健康连接后按持久化使能值显示。
心跳和查询不修改使能值，同值重复 SET 不重复写 Flash。首次没有有效记录时使用 H2 的 enabled_on_boot 默认值。

组件固定使用官方板的四线共存，不提供开关。四个 coex 引脚默认值为：
request=12、priority=10、grant=13、tx_line=11；H2 固件必须启用对应四线共存，
引脚分别为 0、2、1、3。S3 为 leader，H2 为 follower。
S3 外部共存与 BLE 控制器受 IDF 限制，不能同时使用 `esp32_improv` 等 BLE 功能；
组件会拒绝这种冲突。主配置已移除 BLE Improv，保留串口 Improv 和 captive portal。

## Web 和诊断

Web 配置位于 `thread_br.web`，子项为 `port`、`user`、`passwd`。
默认端口为 80，主配置示例显式使用 8080，并使用替换变量设置 HTTP Basic 用户名密码。
二者同时留空可关闭认证；配置其中一个会报错。认证覆盖所有页面与诊断 API。
可与 ESPHome `web_server` 使用不同端口；如果两个常驻服务的端口相同，配置校验报错。
未配置 `web_server` 时可以使用默认 80。断开 Wi-Fi 时后台任务停止 BR Web，
为 fallback captive portal 释放端口；Wi-Fi 恢复且 portal 停止后重新启动 BR Web。
停止时会等待当前诊断请求结束，因此建议保留 Wi-Fi fallback AP 默认等待时间，
不要缩短到小于诊断请求可能持续的时间（最长约 30 秒）。

页面保留官方 Dashboard、Topology、Tools（IPv6 ping）、About。
界面默认使用简体中文，包括导航、状态标签、节点详情和 Ping 提示；
Thread 角色和地址来源仅在显示时翻译，API 字段、协议值及网络名称保持原值。
移除 Network、Commissioner、地址修改页面；仅接收 `POST /ping`，
不注册 PUT/DELETE 或建网、加入、Dataset 写入、RCP OTA 接口。
保留 `/node` 及其若干只读资源、`/get_properties`、`/topology`、
`/node_information`、`/ipaddr`、`/diagnostics`、`/node/dataset/active`（JSON）。
这是诊断子集，不声称实现完整 OTBR REST 管理兼容性。

工具页提供“SRP 服务注册”查询，接口为认证后的只读 `GET /srp`：
显示 SRP Server 状态、已注册主机、IPv6 地址、服务类型、端口和删除标记。
它不替换 IDF 的 SRP 回调，不修改服务注册；只读取注册表，不能单独证明 LAN 上
mDNS 通告已送达。若存在未删除的 `_esphomelib._tcp` 服务，应继续检查代理发布与 LAN 抓包；
若注册表为空，则先检查 Thread 终端的 SRP 注册状态。

拓扑查询向 Thread 网络发送诊断请求，可能需要数秒至 30 秒；请求在 HTTP 任务中执行。
网络无响应或处于恢复状态时可能查询失败；Web 单个 HTTP 任务也会暂缓其他请求。
页面展示的是 Thread 拓扑，不是 HA 实体清单，也不据此判断所有睡眠节点在线状态。

可选实体：

| 配置项 | 含义 |
| --- | --- |
| `rcp.rcp_connectivity` | 后台 Spinel 发射功率属性查询是否成功，与 Thread 角色分开 |
| `thread_attached` | 本机 Thread 栈处于 child/router/leader 且 RCP 通信正常 |
| `thread_role` | 本机 Thread 角色 |
| `rcp_version` | H2 报告的 RCP 版本 |
| `neighbor_count` | 本机邻居表数量 |
| `child_count` | 本机直属 child 数，不是全网在线节点数 |

mDNS 动态数据（包括服务记录和 TXT 字符串）由 `used_psram` 统一控制：
启用时使用 PSRAM，服务容量为 128；关闭时使用内部 RAM，服务容量为 64。
mDNS 任务栈保持内部 RAM。此设置作用于固件中共享的 mDNS 库，容量包含
网关自身和 SRP 代理发布的服务；128 是配置容量，不是实测承载保证。
PSRAM 分配方式由库明确指定，不提供内部 RAM 自动回退。

`used_psram: true` 要求配置 PSRAM；Web JSON 序列化缓冲优先使用 PSRAM，失败时
回退到普通堆。不改变全局 cJSON 分配钩子。栈、UART 和无线驱动保留 IDF 默认分配策略。

## 上板验收

1. 烧录匹配的 H2 RCP，再烧录 S3；确认 UART 波特率和共存配置一致。
2. 日志显示 `Thread started independently of Wi-Fi`；RCP Connectivity 为真，角色最终为 leader/router。
3. 烧录示例节点；节点进入 child/router 状态，并获得 Thread IPv6 地址。
4. 在同一 LAN 中验证 SRP/DNS-SD 服务发现；HA 可直接连接节点 Native API。
   客户端/LAN 必须支持 IPv6 和相关组播，BR 不能替代 LAN 的 IPv6 支持。
5. 验证节点 ESPHome OTA、S3 OTA，确保 Web 和 Dataset 均正常恢复。
6. 验证 H2 短暂失联、S3 重启、Wi-Fi 重连、AP fallback 与 Web 端口并存。
   示例使用 `wifi.reboot_timeout: 0s`，避免长期离网触发整机重启。
   主配置已将 API reboot_timeout 设为 0，避免没有 HA 连接时定时重启路由。

没有硬件验证不能把编译成功视为路由、SRP 发现或 RF 共存已通过验收。

## 本次验证记录（2026-10-15）

以下记录针对此前版本；新增 H2 LED 固件和 Host 控制尚未编译或上板验证。

- ESPHome 2026.9.1 / ESP-IDF 5.5.5 主配置（固定四线共存）完整编译、链接并生成 Factory/OTA 镜像。
- 新配置结构的 17 项测试通过，覆盖旧选项拒绝、四线共存与 BLE 冲突、
  默认 80、captive portal、ESPHome Web 同端口冲突，以及实际代码生成的
  PAN ID（保留十六进制，防止 IDF 回退到默认值）、前导零密钥和固定硬件配置。
- 应用 Flash 统计 1,377,255 / 3,932,160 字节，静态 RAM 统计 153,171 / 341,760 字节。
  静态统计不代表运行时剩余堆，需上板观察 Wi-Fi/Web/OpenThread 同时运行的内存。
- 未连接硬件，尚未验证实际入网、IPv6 转发、SRP 服务发现、OTA 与 RCP 恢复。

## 组件名称和管理页裁剪

顶层配置使用 `thread_br:`；网关通过 `thread_gateway.thread_br_id` 绑定 BR。组件目录、命名空间、日志标签和本地 IDF 辅助模块已统一改名，不提供旧名称兼容。目前实现仍要求 ESP32-S3 + ESP-IDF，其他平台仅在规划中。

仅配置 thread_br 时，自动关闭内部 CONFIG_THREAD_BR_GATEWAY_WEB 构建开关：固件不包含 /thread_gateway 页面、专用脚本或网关 API 路由，共用导航不显示入口，也不查询网关状态。配置 thread_gateway 后才编译这些内容，标签名称为“网关管理”。开关由配置自动确定，不是用户配置项。修改 frontend 后仍需运行 tools/build_web_assets.py；生成头文件同时包含两种受条件编译保护的资源分支。

## RCP 引脚与连接诊断

```yaml
thread_br:
  id: thread_br_main
  rcp:
    uart_tx: GPIO18
    uart_rx: GPIO17
    rcp_reset_pin: GPIO7
    rcp_boot_pin: GPIO8
    coex_request_pin: GPIO12
    coex_priority_pin: GPIO10
    coex_grant_pin: GPIO13
    coex_tx_line_pin: GPIO11
    rcp_connectivity:
      id: rcp_connectivity
      name: "RCP Connectivity"
  # web、thread_network_config 和其他诊断实体仍位于 thread_br 根层级。
```

上述八项引脚与 rcp_connectivity 仅放在 rcp 下，不保留根层级旧写法。共存引脚默认值和电气方向不变；重复引脚、反相以及与 ESPHome uart 共用 Spinel UART 引脚的校验仍生效。

### Thread 与基础网络独立运行

Thread/RCP 初始化不等待 Wi-Fi 连接。Thread 附着后，`thread_gateway` 即可进行
Thread 内部 UDP 发现、上报、ACK 和控制，不依赖 Wi-Fi、HA 或互联网。
Wi-Fi STA 接口仍由 ESPHome 创建，当前配置仍要求包含 Wi-Fi 组件。

Wi-Fi 可用后单独初始化边界路由。断开时通知 OpenThread 暂停基础接口路由，
保留 Thread 和网关 UDP；重新连接后重建边界路由以刷新接口状态。
边界路由初始化失败每 10 秒重试，不将已启动的 Thread 标记为失败。
Web 继续按 Wi-Fi/captive portal 状态启停。

静态和配置检查无法验证实际掉线恢复；需上板覆盖断网开机、联网、断网、重连，
并确认每个阶段 Thread 节点均能收到 UDP ACK。
