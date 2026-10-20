# thread_reed_transporter（首版）

常在线 ESP32 REED 节点的标准 ESPHome 实体 UDP 代理。只引用现有实体 ID，实体自己负责采样、filters、状态发布和本地自动化。不依赖 Native API，不使用 RTC 基准，不调用 component.update，不修改 Thread 角色或路由策略。

`thread_sed_transporter` 保持原样，仍用于间歇唤醒节点。两者不能在同一节点重复绑定、代理同一组实体。

## 示例

完整可用配置入口：`../../esphome-simple-usb-sw/config.yaml`，本组件示例为其同目录的 `thread_reed_transporter.yaml`。节点已移除 API，状态 LED 改为指示 transporter 与网关的通讯状态；OTA 和本地 Web 保留。

```yaml
external_components:
  - source:
      type: local
      path: ../external_components
    components: [openthread, thread_reed_transporter, thread_entity_bridge]

thread_reed_transporter:
  id: node_transporter
  openthread_id: openthread_main
  node_id: usb-switch           # 稳定 ID，1..32 ASCII 字母、数字、_、-
  name: USB Switch
  manifest_revision: 1
  # gateway_id: thread-br-2f    # 可选，限定接收目标
  discovery:
    multicast_address: ff13::8000:8562
    port: 18512
  heartbeat_interval: 10s      # 1s..15s
  batch_delay: 100ms           # 大于 0，最大 10s
  full_sync_interval: 60s      # 最小 1s
  ack:
    timeout: 2s               # 100ms..60s，每轮发送完成后计时
    resend: 2                 # 0..10 次
  switches: [relay]
  buttons: [restart_button]
  text_sensors: [firmware_version]
```

`openthread.device_type` 必须为 FTD。本版本平台为 ESP32 + ESP-IDF；没有声明 nRF52 支持。只负责应用层 UDP，不调用设备角色或路由配置 API。

## 支持的实体列表

- `sensors`、`binary_sensors`、`text_sensors`
- `switches`、`numbers`、`selects`、`buttons`、`texts`
- `lights`、`fans`、`covers`、`climates`、`locks`、`valves`

每项直接填写现有 ID；不接受 source_id、name、lambda、filters 等二次实体配置。名称、单位、图标、entity_category、disabled_by_default 从源实体读取，复杂实体能力从源 traits 读取。内部实体也可以引用，源实体的 internal 不妨碍通过网关暴露。

基础数值/布尔/文本沿用既有 v1 标量状态格式；复杂控制实体使用对象状态。能力置于清单的 capabilities，number/select/text 另携带旧协议所需的兼容字段。

## 清单及同步

1. 无有效目标或地址 TTL 到期，通过 realm-local 多播查询网关，校验请求编号、来源地址及可选 gateway_id；数据端口、地址与 TTL 从 offer 获取。
2. 用 presence 携带 manifest_revision 核对网关 NVS。版本一致仅回确认，不重新发送清单；缺失或版本不同才发送完整分包清单。
3. 首次确认、重新发现或网关请求恢复状态时补发完整有效状态，之后由源实体状态回调置脏，按 batch_delay 合并发送。
4. 所有分包收到后网关立即回本轮 report ACK，不等待数值处理或 HA。冻结报文按配置重试；新状态用 generation 保留，不被旧轮 ACK 清除。周期 full_sync_interval 校正状态。
5. 心跳也核对清单，并维持 30s 控制租约。持续三个心跳周期没有有效网关回复，重新发现目标。

增删实体、修改名称/元数据/能力时手动递增 manifest_revision。普通值变化不用递增。版本一致时不会下载清单来发现用户忘记递增的修改。每节点最多 128 个 ID；最多 16 页，每页 UDP payload 不超过 1232 字节，实体不会拆到两页。

首版资源限制：单实体 capabilities 最多 900 字节；文本状态最多 255 UTF-8 字节；select 最多 32 个选项，每项最多 64 UTF-8 字节。超限能力使初始化失败并记录日志，不静默截断。无有效状态不发送；非法/超长状态跳过，等待下一次源更新或周期同步。

## 控制与实际状态

网关按本地标准接口创建代理，HA 命令通过 UDP 到达节点，再调用对应源实体的标准控制接口。能力和参数在两端校验；复杂命令一次性提交，灯的颜色/亮度/效果/过渡/闪烁参数通过 API 适配完整转发，不在网关先运行一遍灯效。

`command_result.success` 表示参数通过并调用了本地控制入口，不承诺电机/空调等已经完成物理动作。实际状态始终依据源实体发布。每会话保存最近 32 个命令结果，同编号重试不重复执行；同一网关重新发现不会清空去重缓存。节点真正重启切换 session，旧会话命令不重放。

每节点最多排队 8 条、网关最多 16 条待处理命令；同节点串行，不同节点可并行。每条最多发送 3 次、间隔 1s，离线不缓存控制。状态序号阻止旧报告或重复命令结果覆盖更新状态。

## 网关设置及 HA

`thread_gateway.capacity` 新增 switches/numbers/selects/buttons/texts/lights/fans/covers/climates/locks/valves，每项 0..1024，默认 0；含三类只读实体的总容量为 1..2048。实际 `esphome-thread-br/thread-br.yaml` 已添加各类型容量，按需调整后自行编译。占位只预留注册容量，不向 HA 暴露虚假实体。

网关管理页中，REED 的只读和可写实体均可批准；旧 SED 控制实体继续仅 Web 控制。保存 NVS 后重启应用，按节点创建 sub_device。复杂实体在 Web 端提供 JSON 参数调试入口，其主要日常控制界面在 HA。

网关编译目录内的 API 适配由 api_compat.py 生成，所有中间文件位于项目 temp，不改 ESPHome 安装目录：

- 跳过预留但未使用的 sub_device 槽。
- API 元数据直接引用清单中的单位/类别/图标，不受本地编译字符串表限制。
- 声明 has_deep_sleep，让 HA 2026.7.0 的预期断开逻辑保留最后状态。网关不进入深睡，普通安全重启、应用清单重启也走正常 DisconnectRequest。
- 首次有效远端状态到达前，不发送默认值或 missing_state 覆盖 HA 缓存。
- 灯的 API 命令直接路由到 UDP，避免代理本地乐观执行、丢失过渡参数。

意外掉电/崩溃/网络丢失按非预期断开处理。保留显示不代表重启期间可以控制；已通知预期断开却启动失败时，HA 可能继续保留旧状态。未做实机验证，不宣称可消除所有 HA 版本或重连阶段的显示变化。

## 验证范围

只运行配置校验、Python/JavaScript 语法与回归检查，没有编译、刷机或操作设备。临时 all-types 配置验证了 14 类实体的 ID 类型解析。C++ 编译、REED 多跳收发、HA 控制以及重启显示连续性由后续双端固件实测验证。

报文统一携带 `node_type: reed` 和 `heartbeat_interval_ms`。REED 表示应用层常在线节点分类；OpenThread 仍配置 `device_type: FTD`，实际 Child/Router 角色及多跳路由由 Thread 协议决定。

网关通讯状态通过 `id(node_transporter).is_gateway_connencted()` 查询。Thread 入网状态通过 openthread_info 的 `thread_attach` 二进制传感器提供，两者分开。


## 地址 TTL 续期与重新发现

最近一次发现回复的 ttl_s 作为续期时长。收到来源地址/端口、网关、节点及会话均匹配的完整清单/心跳确认，或与当前待发轮次及总分包数匹配的完整 report ACK 时，从当前时间续期。单纯本地 UDP 提交或无效 ACK 不续期。

目标不存在、TTL 到期或连续三个心跳间隔未收到回复时重新发现。一个状态批次的 ACK 重试耗尽后，也会立即重新发现，保留原 report_id、seq、全部报文及快照；确认清单后重发，不重新读取实体。每批次最多执行一次 ACK 失败触发的恢复；恢复后使用原 ack.timeout/resend。恢复发现及清单确认最长等待 10 秒，超时或恢复发送仍失败则放弃本批次，保留实体待同步状态，由后续批次继续尝试。新源状态不会被旧快照 ACK 清除。

```yaml
# node_transporter 为 thread_reed_transporter 的组件 id。
- thread_reed_transporter.discovery:
    id: node_transporter
# 简写：
# - thread_reed_transporter.discovery: node_transporter
```

手动 action 异步清除目标及清单确认状态并立即尝试发现，不创建实体、不主动采样。有正在发送的批次时保留其冻结报文，发现完成后重发；没有待发批次时，重新同步后按正常批处理发送最新状态。无需修改网关配置、协议或 manifest_revision。
