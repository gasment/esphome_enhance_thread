# Thread gateway（第一版）

通过独立的 `thread_gateway` 配置启用，必须用 `thread_br_id` 引用 BR，运行在 ESP32-S3 网关上，使用 IDF OpenThread 的原生 UDP Socket 接收 Thread 多跳节点报文，再通过网关自身的 ESPHome Native API 向 HA 提供只读实体及常在线 REED 的标准控制实体。不需要 MQTT。

节点发送端见 [thread_sed_transporter](../thread_sed_transporter/README.md)，使用下面的 UDP v1 协议。

## 配置

external_components 必须同时包含 `thread_br`、`thread_gateway`，目录均位于项目的 external_components。
完整接收端配置见 [网关 YAML](../../esphome-thread-br/thread-br.yaml)。可设置：

```yaml
thread_br:
  id: thread_br_main
  # UART、Dataset、Web 等原有配置照常保留

thread_gateway:
  thread_br_id: thread_br_main
  gateway_id: thread-br-2f
  entity_auto_register: false
  discovery:
    multicast_address: "ff13::8000:8562"
    port: 18512
    address_ttl: 1h
    announce_interval: 60s
  udp:
    port: 18513
  capacity:
    sensors: 32
    binary_sensors: 16
    text_sensors: 16
    nvs: 300KB  # 清单大小上限，默认 300000 字节；不改变 Flash 分区。
```

省略整个 `thread_gateway` 配置即可不启用；不支持 true/false 简写。gateway_id 省略时采用网关 MAC 派生值。
身份 ID 限 1～32 位 ASCII 字母、数字、下划线、短横线。端口为 1～65535，发现与业务端口不能相同。
两项时长为 1s～24h；多播地址须为 scope=3、transient=1 的应用组，不占用 Thread 保留组。

capacity 的 sensors、binary_sensors、text_sensors 限定批准接入 HA 的实体数量，每类 0～1024，总和 1～2048；nvs 不计入实体或子设备槽位。
这个数字是配置边界，不是已实测可承载数量；实际还受 RAM、NVS 和 Native API 清单传输时间限制。
全局最多记录 128 个节点、2048 个实体，每节点最多 128 个实体，控制项计入注册表上限；获批的 REED 控制项也占用对应 HA 类型容量。
NVS 注册表单份大小由 capacity.nvs 限制（默认 300KB），并受实际 NVS 分区剩余空间限制。容量内增删批准项无需 OTA，扩容需要 OTA。
配置中的 0 禁止批准该类型；内部可能保留一个未暴露的指针槽以满足 ESPHome 编译期类型支持。

## 注册与 Web

NVS 命名空间为 thread_gateway，Web 入口为 /thread_gateway，API 前缀为 /thread_gateway/api/，实体标识前缀为 thread_gateway_。


顶部标签栏“网络拓扑”右侧“网关管理”进入 /thread_gateway，沿用原有 Basic 认证，不新增 Web 配置。
第一级是节点列表，/thread_gateway/node/<node_id> 包含：

- 只读实体的当前值、接入选择、已批准/当前 HA 已生效状态。
- switch、number、select、button、input_text 控制项。SED 控制项只在 Web 操作；thread_reed_transporter 的控制项可批准暴露到 HA。
- 节点地址、端口、会话、清单版本、最后上报时间、错误、最近报文和执行结果。

保存批准清单后，再点击“重启并应用清单”。固定存 NVS，重启时在 Native API 启动前建立实际实体清单。
entity_auto_register=true 自动批准容量及元数据允许的新实体（SED 控制项除外），写入 NVS，但不会自动重启。
未完整收到全部 manifest 分片，不替换已有清单；节点睡眠或状态包缺少某项都不会删除实体。
相同 ID 但不同类型的实体不沿用原批准状态。修改清单元数据必须递增 revision。
取消接入并重启后，网关不再暴露该实体；HA 中原有实体注册记录可能需要用户自行删除。

每个至少批准一项实体的节点，都在 HA 中对应一个 ESPHome 子设备（sub-device），其名称取自节点 name（空时回退到 node_id）。
各节点实体归属其自己的子设备，网关自身的诊断实体仍属于网关。这是同一 ESPHome 集成下的独立设备，不是自动生成仪表盘折叠卡片。
无需在 YAML 逐个填写 esphome.devices，网关重启时从 NVS 批准清单自动创建；全部未批准的节点不创建 HA 子设备；仅有获批控制项的 REED 也会创建。

子设备 ID 为 thread_gateway_<node_id> 的 FNV-1a 哈希；实体 API key 仍由 thread_gateway_<node_id>_<entity_id> 生成。
HA 显示实体的 name（空时回退到 entity_id），不再以长的协议标识作为友好名称。
身份与显示名分离，同名节点仍是不同设备；重命名不会改变设备 ID 或 API key。修改名称需递增节点 manifest_revision，并重启网关应用。
检测与网关手工声明子设备的 ID 冲突及实体 key 冲突，发现冲突则拒绝批准。

子设备最多预留 min(128, capacity 全部实体类型总和) 个槽位，另外保留 YAML 手工声明的设备槽位。空槽位不暴露。
当前 ESPHome Native API 使用固定长度 DeviceInfo 数组，组件在构建时对 api_pb2.cpp 的设备编码/长度计算添加 device_id=0 空槽过滤。
适配仅应用于网关构建目录，中间文件保存在 temp/thread_gateway_api，不修改已安装的 ESPHome 或节点固件。
配置检查会校验当前 API 源码布局，上游布局不匹配时明确报错，避免静默产生空设备。

从原“所有实体属于网关”的版本升级时，实体的 device_id 会由 0 变为节点设备 ID，HA 可能作为新实体处理，原实体记录及自动化引用需按实际情况整理。
离线仍保留子设备及实体；删除节点或取消其全部接入选择，在下次网关重启后才停止暴露该子设备。
ESPHome 2026.9 的单位、device_class、icon 使用编译期字符串表，不能无限动态扩展。
组件预编译常用单位、ESPHome 当前 sensor/binary_sensor 的 device_class，以及 __init__.py 中的常用图标。
留空允许使用；不受支持的元数据仍能在 Web 查看，但不能批准接入 HA，需要扩展表并 OTA。
网关记录本轮运行中最后接收的值；网关重启后等待节点重新发送状态，不持久化每次读数，避免 NVS 持续写入。
节点应在每次唤醒发送当前完整状态快照或定期全量重发，不能永远只发送相对上次的变化项。

清单协议的 entity_category 字段识别空/省略（普通）、config（配置）和 diagnostic（诊断）。thread_sed_transporter 的只读 sensor/binary_sensor/text_sensor 配置仍沿用 ESPHome 原生限制，仅允许普通和 diagnostic。
网关解析、版本比较、NVS 保存和 HA 注册均保留该字段，Web 显示分类。旧 NVS 清单缺少此字段时按普通实体读取。
分类更改也属于元数据更改，节点需递增 manifest_revision，网关重启后应用；不改变实体 API key。

## 清单元数据与缺省值

节点按有效配置中存在的 HA 元数据发送，不在报文构造阶段为缺失字段补值。ESPHome schema 已提供的默认值也可能出现在报文中。
网关对每份新描述从默认值开始完整解析，缺失字段不继承旧清单。显式 false、0 和空字符串均按真实值处理，不用真值判断代替字段存在性判断。
提供了字段但类型/内容无效时，拒绝清单并返回错误，不静默用默认值掩盖。

| 清单字段 | 缺省值/应用规则 |
| --- | --- |
| id、type | 必填，没有缺省值 |
| name | 使用实体 id |
| unit、device_class、icon | 空字符串 |
| state_class、entity_category | 无状态类别/普通实体 |
| accuracy_decimals | 0 |
| disabled_by_default | false，应用到 HA 默认禁用标志 |
| force_update | false，应用于数值传感器 |
| trigger_on_initial_state | false，应用于布尔传感器 |
| max_length | text 缺省255字节，input_text 缺省128字节；显式限制会校验并应用 |
| min/max/step、options | 仍为对应 number/select 控制类型的必需参数 |

数值 state_class 同时支持 measurement、measurement_angle、total 和 total_increasing。
以上字段均由网关解析、保存 NVS、比较清单版本并在重启注册时应用，按实体类型使用相应元数据。
source_id/source_components、delta、filters、lambda 和自动化由节点执行。节点 internal=true 是本地可见性策略，不传递成网关 HA 实体的 internal 标志；网关仍按批准清单控制暴露。
元数据修改需递增 manifest_revision，新版本节点示例使用 revision=3。

## 离线保留与显式删除

没有按离线时长自动清理节点的机制。节点深睡、断电或失联时，节点列表、清单、接入设置和已暴露的 HA 实体保留。
常在线控制心跳超过 30 秒后显示状态未知并停止允许下行；不等同于删除，也不清空最后的读数。
已存 NVS 的清单和接入设置跨网关重启保留；读数仅保存于运行期内存，网关重启后等待节点重发。

删除不依赖在线状态或上报间隔，用户随时可以主动删除；事件驱动节点数小时不上报不代表离线。
不根据30秒静默时长生成节点离线判定。原有30秒控制租约仅用于下行许可，不限制删除。

仅节点详情页“删除节点”按钮二次确认后，执行 POST /thread_gateway/api/node/<node_id>/delete，请求必须包含 confirm=true。
删除清单、接入设置、缓存读数、调试信息和控制记录，保存 NVS 成功后才返回成功；写入失败则回滚节点。
不维护删除屏蔽清单；后续报文（包括已在队列中的报文）仍可重新发现该节点。真正不再上报的节点保持删除状态。
当前 API 注册表保留至下次网关重启，之后按最新批准清单重建；若节点未重新加入并获批，就不再暴露其 HA 实体。删除不会自动重启。
HA 自身的实体注册记录可能仍需在 HA 内手动移除。

删除不是永久封禁：删除后，若该物理节点继续发送报文，会作为新发现节点重新加入，原接入设置不恢复。
如启用 entity_auto_register，新清单仍按自动批准规则处理，需再次重启才会暴露。

## UDP v1 协议

JSON UTF-8，单包最多 1232 字节（含全部字段），不接受超长包。所有 node/entity/session ID 遵循上述 ID 规则。
节点可从任意本地 UDP 端口发起，但必须维持该端口以接收回复和控制。回复目标就是报文源 IPv6/端口。
UDP 不保证送达，Thread 的逐跳确认不等同于 HA 接收确认。

### 发现

节点向 ff13::8000:8562:18512 发送：
```json
{"v":1,"type":"discover","request_id":1}
```
网关单播回复；周期宣告采用同一格式，无 request_id：
```json
{"v":1,"type":"offer","gateway_id":"thread-br-2f","address":"fd00::1234","port":18513,"ttl_s":3600,"control_lease_s":30,"request_id":1}
```
address 是当前 Mesh-Local EID，不使用易变的 RLOC。节点缓存 gateway_id/address/port 和 TTL；
无目标或过期后重新多播发现。存在多个网关时，节点应固定期望 gateway_id，避免任意切换。
TTL 是应用层缓存有效期，不是 Hop Limit。发送失败或持续收不到必要确认时也应主动重新发现。

### 完整清单

向 offer 给出的单播地址与业务端口发送。session 每次节点启动随机生成，常在线期间保持不变。
revision 是可持久保持的清单版本（1～4294967295），实体增删、名称或元数据变更时递增。
part 从 0 开始，parts 1～16，每片最多 32 项；完整清单最多 128 项，每包仍须遵守 1232 字节限制。
全部分片需在 15 秒内到齐，各片 revision/parts/name 一致。
```json
{"v":1,"type":"manifest","node":"room1","name":"卧室节点","session":"boot_a3f2","revision":1,"part":0,"parts":1,"entities":[
  {"id":"temperature","name":"温度","type":"numeric","unit":"°C","device_class":"temperature","state_class":"measurement","accuracy_decimals":1},
  {"id":"usb","name":"USB","type":"binary","device_class":"plug"},
  {"id":"status","name":"说明","type":"text"},
  {"id":"led","name":"指示灯","type":"switch"},
  {"id":"limit","name":"上限","type":"number","min":0,"max":100,"step":1},
  {"id":"mode","name":"模式","type":"select","options":["auto","manual"]},
  {"id":"sample","name":"立即采样","type":"button"},
  {"id":"label","name":"标签","type":"input_text","max_length":64}
]}
```
numeric/binary/text 也接受 sensor/binary_sensor/text_sensor 别名。
这里只是可读示例；实际发送端须压缩 JSON 并按字节长度拆包。数值必须有限，不能发送 NaN/Infinity。
文本状态最多 255 UTF-8 字节；input_text 默认 max_length=128，允许 1～255；select 为 1～32 个不重复选项。
单位/类/图标最多 64 字节，显示名最多 96 字节；accuracy_decimals 为 -6～6。
接收端每片回复：
```json
{"v":1,"type":"manifest_ack","gateway_id":"thread-br-2f","node":"room1","session":"boot_a3f2","revision":1,"accepted":true,"complete":true}
```
complete=false 表示尚缺分片。accepted=false 附 error。节点应在未收到 complete=true 时有界重发全部清单。
确认表示本地清单处理结果，不代表已批准、NVS 写入成功或 HA 收到；NVS 失败会显示在管理页。
相同清单重复发送不会反复写 NVS。

当已删除节点继续发送 state/presence 但网关没有其清单时，网关单播 manifest_request（包含 v、gateway_id、node、session），最多每秒一次。
节点验证当前网关来源和会话后重发清单并置位全量上报。常在线节点无需重启；短时唤醒节点在当前或下一次 report 中恢复。
这是清单重取，不是状态报文 ACK。旧版发送端若不支持 manifest_request，会在下次启动或主动重发清单时恢复。

### 状态与在线模式

```json
{"v":1,"type":"state","node":"room1","session":"boot_a3f2","seq":1,"values":{"temperature":25.2,"usb":true,"status":"正常","led":false}}
```
seq 每个会话内递增，重复/过期序号丢弃；支持 uint32 回绕。仅接受清单内实体，未知值不会创建实体。
状态默认不发应用层 ACK；need_ack=true 时仅做整轮收包确认，见下节。网关按原逻辑更新缓存和已生效 HA 实体。节点无需等待 HA API 连接。
节点深睡时网关保持在线并保留最后值；网关/API/网络本身离线仍会影响 HA 可用性。

常在线可接受控制的节点每约 10 秒发送：
```json
{"v":1,"type":"presence","node":"room1","session":"boot_a3f2","control_ready":true,"debug":{"rssi":-65}}
```
control_ready 也可放在 manifest/state；true 的租约为 30 秒，超时禁用控制，不推断节点已经深睡。
control_ready=false 表示短时唤醒但不接受控制；临睡前可发 presence + sleeping=true。
debug 可选 JSON 对象，序列化后最多 1024 字节。不要上报网络凭据。

### 可选的 report 总收包确认

状态包可以附带：
~~~json
{"v":1,"type":"state","node":"room1","session":"boot_a3f2","seq":5,"need_ack":true,"report_id":3,"part":0,"parts":2,"values":{"temperature":25.2}}
~~~
同一调用的 report_id/parts 相同，part 从0开始，每包保留独立 seq。网关仅保存收包位图，不缓存数据等待整体应用。每节点最多保留8个轮次，12分钟后清理（覆盖最长重发窗口）；节点会话改变时清空，不写入 NVS。

收齐同一轮全部分包后，在处理当前包的数值及 HA 发布之前，立即返回：
~~~json
{"v":1,"type":"report_ack","gateway_id":"thread-br-2f","node":"room1","session":"boot_a3f2","report_id":3,"parts":2,"complete":true}
~~~
节点的动作通过可选 ack: {timeout: 2s, resend: 2} 启用确认和整轮重发；未配置 ack 时关闭，协议内部仍用 need_ack 标记，不需要用户填写该字段。\n\n这是接收端的整轮通讯回执，不是数值有效性、应用成功或 HA 送达确认；不逐包回复 ACK。重复包也参与收包确认，完整轮次的重复包可再次触发同一总 ACK，节点仅回调一次。ACK 报文按轮次记录分包：重复分包不再次发布，重发补到的缺失分包可处理；已收到更新轮次时，旧轮次不覆盖新状态。无 ACK 状态包沿用原 seq 去重。

无变化时允许 values={}，作为需要 ACK 的空值 report。关闭 need_ack 或不提供该字段，继续使用原状态协议。清单的 manifest_ack 和下行命令的 command_result 均不受影响。

### Web 下行与回执

只允许有有效 control_ready 租约的节点，发送到最近一次源 IPv6/端口：
```json
{"v":1,"type":"command","gateway_id":"thread-br-2f","node":"room1","session":"boot_a3f2","command_id":123,"entity":"led","operation":"set","value":true}
```
button 使用 operation=press，无 value；其余 set，值按类型及范围校验。
节点返回：
```json
{"v":1,"type":"command_result","node":"room1","session":"boot_a3f2","command_id":123,"success":true,"value":true}
```
失败用 success=false，可附 error（最多160字节）。value 可选，用于回报实际执行后的值。
网关每秒重试，最多 3 次；节点必须按 session+command_id 去重，并重复回复已缓存结果，尤其 button 不能重复执行。
回执超时显示“执行结果未确认”，不等同执行失败。节点会话变化取消在途命令，不向新会话重放。
每节点最多一条在途命令，全局最多16条，保留最近32条内存记录；重启不保留队列。
网关不缓存离线命令，不依靠 Thread parent 为应用控制提供跨深睡存储。

## 验证范围

本次只进行 ESPHome 配置校验、配置边界检查及 Web 脚本静态语法检查，没有编译固件。
UDP 多跳收发、动态实体启动注册、NVS 跨重启、HA 接入和节点命令需后续编译、联调验证。

## NVS 容量与页面状态

capacity.nvs 默认 300KB（300000 字节），范围 4096～508000 字节，可使用带单位的字节大小或整数。限制整份注册清单 blob 的字节数（包含结束符），不扩大或独占 NVS 分区。所有节点合并保存，更新时需有足够空间写入新版本；实际写入失败仍由 NVS 错误返回并在页面提示，不能将配置上限等同于可保证写满的空间。

清单省略可由解析器默认值恢复的空字段、false 标志、默认精度和长度，兼容已有完整字段清单；读数和 ACK 记录仍不写入 NVS。降低配置上限不会删除已有清单；可以继续加载，但超过上限时拒绝保存，需提高上限或减少节点。旧清单在下次正常保存时转为精简格式。

网关管理首页及节点页分开显示网关/Thread 状态、HA 已生效实体及设备数、三类实体容量、当前清单与配置上限、实际已保存清单、共享 NVS 分区及空闲条目空间。条目空间不等同于单个 blob 的保证可写大小。待应用、容量告警、存储错误属于系统通知，操作结果独立显示，轮询不会覆盖操作结果。

接入 100 个节点时还需按实际实体分布扩大三类实体槽位。例如每节点 6 个数值、1 个布尔、2 个文本，可配置 sensors: 600、binary_sensors: 100、text_sensors: 200、nvs: 300KB。此时自动预留 128 个 HA 子设备槽位，组件清单最多 128 个节点、2048 个实体（含控制项）。这些是配置边界，100 节点运行时 RAM 和共享 NVS 余量仍需实机验证。

## 上报回调与本地传感器路由

在 thread_gateway 下配置 on_node_update，按 node_id、entity_id 和 type 精确匹配。type 必填，支持 sensor、binary_sensor、text_sensor；x 分别为 float、bool、std::string。entity_id 是节点 thread_sed_transporter 清单中的输出 id，并非 source_id 或显示名称。可为同一来源配置多条回调。

~~~yaml
thread_gateway:
  thread_br_id: thread_br_main
  on_node_update:
    - node_id: battery-node-2f
      entity_id: reported_temperature
      type: sensor
      then:
        - sensor.template.publish:
            id: local_temperature
            state: !lambda return x;
    - node_id: battery-node-2f
      entity_id: reported_usb_detect
      type: binary_sensor
      then:
        - binary_sensor.template.publish:
            id: local_usb
            state: !lambda return x;
    - node_id: battery-node-2f
      entity_id: reported_esphome_ver
      type: text_sensor
      then:
        - text_sensor.template.publish:
            id: local_node_version
            state: !lambda return x;

sensor:
  - platform: template
    id: local_temperature
    name: "SED Temperature"
    unit_of_measurement: "°C"
    device_class: temperature
    state_class: measurement
    accuracy_decimals: 1
    update_interval: never

binary_sensor:
  - platform: template
    id: local_usb
    name: "SED USB"
    device_class: plug

text_sensor:
  - platform: template
    id: local_node_version
    name: "SED Version"
    entity_category: diagnostic
    update_interval: never
~~~

每个已知且有效的只读实体值触发一次，不等待 report 全部分包、不要求来源实体获准接入 HA，也不占来源实体的 HA 批准容量。目标本地模板传感器正常占用本地实体资源，并可使用自己的 filters 和 on_value/on_state 等自动化。来源也被批准暴露时，会同时存在自动暴露实体和本地路由实体；如仅需路由，可不批准来源实体。

回调接入现有报文去重之后：重复包、无效值、未知实体、类型不匹配、清单/心跳/控制执行结果不触发。节点本轮未发送的实体不触发；新的有效包即使值相同也触发，变化筛选由节点或目标本地 filters 负责。

ACK 的收齐即回复逻辑不变，仍先于数值处理；回调不参与 ACK 成功判定或 RTC 基准更新。回调在主循环中、释放注册表锁后执行，使用持有的值副本，不暴露 JSON/报文指针。自动化含 delay 时使用 ESPHome 标准异步自动化机制。


## 常在线 REED 控制代理（thread_reed_transporter）

网关现支持从 thread_reed_transporter 清单注册官方常用可写实体：switch、number、select、button、text、light、fan、cover、climate、lock、valve。只在标识 node_type=reed 的常在线节点开放控制实体的 HA 批准；现有 thread_sed_transporter 无需修改，SED 控制实体继续 Web-only。

capacity 为这些类型增加独立预留量，默认均为 0。完整配置、能力读取、命令去重、分包 ACK、重启保留状态及限制见 [thread_reed_transporter](../thread_reed_transporter/README.md)。应用批准清单仍需重启。HA 保留状态采用 has_deep_sleep + 正常 DisconnectRequest，网关不真正休眠。首次远端有效值到达前抑制默认状态，单位/类别/图标按清单直接进入 API。

Web 的控制实体也具有 HA 注册选项；复杂实体的 Web 控制输入为 JSON 调试参数。Web 可展开查看各类控制实体的已批准数和容量，原有五个紧凑存储卡保留。

## 节点分类和离线

节点报文必须携带 `node_type: sed` 或 `node_type: reed`。SED 不按静默时长判定离线。REED 按其上报的心跳间隔乘以 `reed_heartbeat_misses` 判定离线，默认连续 3 次，配置范围 2～10；离线不删除节点、注册设置或 HA 实体。网关重启后重新给予完整的心跳等待期。

本版本统一使用新名称，包括 NVS 命名空间、HA 身份前缀和 Web 路径；不读取旧名称清单。
