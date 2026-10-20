# thread_sed_transporter

手动采样、RTC 变化基准和 Thread UDP 上报组件。替代 rtc_reporter，不依赖 Native API、HA 订阅或 MQTT。
接收端为 [thread_gateway](../thread_gateway/README.md)，使用其 UDP v1 发现、清单、状态、心跳和控制协议。

## 配置

实际配置见 [电池节点包](../../esphome-battery-node/thread_sed_transporter.yaml)，通过 [主配置](../../esphome-battery-node/config.yaml) 引入。
节点不依赖 API，OTA 和本地自动化可继续使用。数值传感器仍可设置 internal=true、update_interval=never；传输清单直接绑定原实体。

```yaml
thread_sed_transporter:
  id: sensor_reporter
  openthread_id: openthread_main
  node_id: bedroom_sensor
  name: "卧室传感器"
  manifest_revision: 1
  discovery:
    gateway_id: thread-br-2f
    multicast_address: "ff13::8000:8562"
    port: 18512
    timeout: 1s
  acquisition_delay: 1s
  timeout: 5s
  full_report_every: 20
  control_ready: !lambda |-
    return id(usb_detect).state || id(key_wakeup_event);
  heartbeat_interval: 10s

  sensors:
    - id: sht30_temperature
      source_components: sht30_main
      delta: 0.2

  binary_sensors:
    - id: usb_detect

  text_sensors:
    - id: esphome_ver
      max_length: 32

  switches:
    - sys_led_enable

  on_complete:
    then:
      - lambda: |-
          ESP_LOGI("sed", "ok=%s sent=%u unchanged=%u invalid=%u errors=%u",
                   YESNO(success), unsigned(sent), unsigned(unchanged),
                   unsigned(invalid), unsigned(errors));
      - script.execute: deepsleep_process
```

触发一次：
```yaml
- thread_sed_transporter.report:
    id: sensor_reporter
    ack:  # 可选；不配置即不启用 ACK。
      timeout: 2s
      resend: 2  # 首发之外最多重发两次。
```

普通 Component，没有 update_interval，不自动执行采样/遥测。重复 report 在上一轮采样、传输或完成回调期间被忽略。
实体只在源组件中定义一次。这里的 id 引用原有实体，也直接作为报文实体 ID；不再创建、注册或恢复第二个 Sensor/BinarySensor/TextSensor。
名称、单位、精度、设备类别、状态类别、图标、诊断分类等，通过 thread_entity_bridge 从源实体运行时读取。源实体的 internal 标志不会阻止网关按批准清单暴露它。
SED 列表只保留 id、delta、source_components 和文本 RTC max_length；不再接受 source_id、lambda 或重复的实体元数据。
source_components 为 PollingComponent ID 或列表，每轮按指针去重，只调用一次 update。数值源实体自身继承 PollingComponent 时可省略并自动更新自身；共享父组件需显式指定。
二进制/文本默认读取现有有效状态，GPIO 不必产生新边沿；显式提供 source_components 时继续按原机制等待本轮发布，超时跳过缺少新值的项目。不会把 RTC 基准写回本地传感器来冒充测量值。
filters、lambda 转换以及 on_value/on_state 等自动化只在本地源实体上配置；本地回调独立于网络就绪，屏幕可离线刷新。RTC 基准提交不再触发第二份实体回调。
从旧版迁移时须把原 reported_* 的名称、单位等配置移到源实体，递增 manifest_revision，并更新网关 on_node_update 中的 entity_id。原报文 ID 与现有源 ID 不同的项目会作为新清单身份处理，需要在网关确认接入选择后重启应用。
只读项的 entity_category 沿用 ESPHome 原生限制：不设置或空字符串为普通实体，diagnostic 为诊断；不允许 config。
该字段写入清单，由网关保存 NVS 并在注册 HA 实体时应用，不仅影响节点本地实体。
修改分类需递增 manifest_revision，网关收到新清单后重启应用。从不支持此字段的版本升级时，先更新网关，再更新节点。


清单按字段存在性透传受支持的 HA 元数据：name、单位、device_class、icon、state_class、accuracy_decimals、entity_category、disabled_by_default、force_update、trigger_on_initial_state、文本 max_length。
未提供的字段交由网关默认，显式 false/0/空字符串保留。采样和自动化参数保留在节点，不当作 HA 元数据传递。
各类型允许的 YAML 配置项仍使用 ESPHome 原生 schema 校验。网关缺省值与应用规则见 [清单元数据说明](../thread_gateway/README.md)。

## 可选的整轮通讯 ACK 与重发

ack 只放在 thread_sed_transporter.report 动作内，省略整个 ack 块时关闭确认/重发，不触发 on_report_success 或 on_report_fail，仍执行原来的 on_complete。

~~~yaml
# 放在现有自动化的 then 内：
- thread_sed_transporter.report:
    id: sensor_reporter
    ack:
      timeout: 2s
      resend: 2
~~~

ack.timeout 是每次发送后的 ACK 等待时长，范围1ms～60s，默认2s。ack.resend 是首发之外的最大重发次数，范围0～10，默认0；ack: {} 表示启用2秒等待且不重发。示例首次发送后等待2秒，未确认则重发整轮，最多重发两次；全部未确认时约6秒后失败。不配置 ack 的简写仍有效：

~~~yaml
- thread_sed_transporter.report: sensor_reporter
~~~

组件级回调：

~~~yaml
thread_sed_transporter:
  # 其余配置略
  on_complete:
    - lambda: |-
        // 保留原本的本地处理语义，不等待 ACK，不用于本例的深睡触发。
        ESP_LOGI("sed", "local success=%s sent=%u", YESNO(success), unsigned(sent));
  on_report_success:
    then:
      - lambda: |-
          ESP_LOGI("sed", "report=%u ACK confirmed", unsigned(report_id));
      - script.execute: deepsleep_process
  on_report_fail:
    then:
      - lambda: |-
          ESP_LOGW("sed", "report=%u ACK failed", unsigned(report_id));
      # 失败后可自定义处理；本例仍结束本轮唤醒，避免无限等待。
      - script.execute: deepsleep_process
~~~

网关按 node + session + report_id 收集 part/parts（最多16包），收齐整轮分包立即回复总 report_ack，不等待数值处理或 HA。它不是逐包 ACK，也不是数值有效性、应用成功或 HA 送达保证。

本轮开始发送前固定每个包的数据、序号和总包数。超时重发所有分包，沿用同一 report_id、seq 和内容；不重新采样、不再次执行本地发布或 RTC 基准提交、不累计 on_complete 的 sent。ACK 丢失时，网关为完整轮次的重复包补回同一个总 ACK，已处理的分包不重复发布。首次补收到的缺失分包可处理；更新轮次到达后不允许旧轮次覆盖新状态。

on_report_success(report_id) 与 on_report_fail(report_id) 互斥，每次启用 ACK 的有效调用最多触发其中一个。report_id 仅为当前启动内的调用编号，协议另有启动会话隔离。正在执行而被忽略的重复调用不生成结果回调。

- 在首发及允许重发的等待窗口内收到匹配总 ACK，触发 on_report_success；不再重发。
- 首轮 ACK 重试耗尽后，不立即回调失败：清除目标缓存，重新发现并确认清单，然后重发原始整轮报文。每轮最多自动恢复一次；恢复后的发送仍使用本轮 ack.timeout/resend。重新发现超时、恢复后的 ACK 再次耗尽，才触发 on_report_fail。无法形成报文、待确认队列已满或初始采集/发现超时仍直接失败。
- ACK 失败不回滚 RTC，不改变强制全量标记；on_complete 仍在首轮本地处理后触发一次，不等待 ACK，也不因重发再次触发。两个结果维度可能不同。
- 没有变化但配置了 ack 时，发送一个 values={} 的报文测试通讯；sent 保持0。省略 ack 时保持原来的无变化行为。
- ACK 等待与重发不会阻止新的 report；最多保留8个待确认调用的冻结报文。普通重试保持原目标；重新发现恢复后改用新回复的网关地址/端口，报文的 report_id、seq 和内容保持不变。
- 若用 ACK 控制深睡，将深睡动作放在结果回调，避免 on_complete 提前深睡。省略 ack 的调用需自行使用原 on_complete 流程。
- ACK 流程不自动控制深睡或 Thread poll_period。初始采样/发现 timeout 预算不包含后续 ACK 等待；自动恢复的发现及清单确认另有一份 timeout 预算。最坏通讯等待约为两轮 ACK 窗口加一次恢复 timeout，每轮 ACK 窗口为 ack.timeout × (resend + 1)；另加初始采集/发现耗时。请据此留出唤醒时间。主循环调度可能影响实际回调时刻。

旧 need_ack 动作字段和 on_report_ack 回调已移除，请迁移至 ack 块和两个结果回调。先更新网关，再更新节点；旧网关可处理数值但不能完整支持本版重发去重。启用此动作配置无需递增实体清单版本。

## 生命周期与超时

每次 report：
1. 重置新采样标记，等待 acquisition_delay，再更新硬件；同时尝试 Thread 就绪与发现，二者并行。
2. RTC 中的目标不存在或过期时发 realm-local 多播查询，校验请求 ID、网关身份、源地址及端口，再缓存 ML-EID、端口和网关提供的 TTL。
3. 每次节点启动都重新发送完整实体清单，最多分成16片，每包最多1232字节；最多3轮重发，只有 manifest_ack complete=true 后才发状态。
4. 取得本轮输入后比较 RTC 基准，按变化或强制全量条件组包发送，分包独立提交基准。
5. 完成本轮，调用 on_complete。失败、超时、无变化也回调，递归 report 被忽略。

timeout 是从调用 report 开始的初始采集/发现等待预算，包含 acquisition_delay；不包含后续 ACK 窗口。若 ACK 耗尽触发自动恢复，恢复发现及清单确认额外获得一份 timeout 预算。它不保证中断底层组件同步阻塞，也不等于从芯片唤醒开始计时。
超时时已确认目标及清单的情况下，可发送本轮已取得的有效输入；缺失新读数记 invalid，不使用其旧缓存冒充新读数。
无目标或未取得清单确认时不提交新基准。未配置 ack 时，timeout 后可由 on_complete 进入既有深睡脚本，不需要等 HA；配置 ack 时，应由最终 on_report_success/on_report_fail 控制深睡，避免提前中断 ACK 或恢复。

discovery.timeout 每次1ms～10s，单轮最多3次查询，最终受总 timeout 限制。
node_id、gateway_id、实体 id 都是1～32位 ASCII 字母、数字、下划线、短横线。节点名与实体名最多96 UTF-8字节。
未指定 gateway_id 时接受第一个合格回复；多网关部署建议明确配置。
修改节点名称或实体清单、元数据、控制选项时递增 manifest_revision。版本必须持久保持，不能每次启动随机变化。
第一版每节点最多16个只读及控制项；只有一个 reporter 配置实例。

网关删除节点后，如节点仍在发送心跳或状态，网关会回复 manifest_request 请求重发清单。
发送端仅接受当前网关地址/端口、gateway_id、node_id 和 session 均匹配的请求，重置清单确认状态并标记下轮全量。
不自动发起新一轮传感器采样：常在线模式可立即重发清单，遥测仍由 report 触发。删除不是节点封禁，重新发现后按新节点管理。

## RTC 与阈值

数值绝对差达到 delta 且实际值不同才变化；delta=0 代表任意实际变化。布尔和文本按不相等判断。
未达到阈值不更新基准，缓慢变化可以积累。文本不截断，超过 max_length 或包含 NUL 记 invalid。
只读基准沿用 ESPHome RTC preferences，包含校验开销后最多占共享256字节；还需给其他使用同一池的组件留空间。
数值、布尔每项12字节，文本每项4+向上对齐到4字节的(8+max_length)；文本 max_length=1～128字节。
网络目标缓存、RTC时钟到期时间、计数和强制全量标记单独使用小型 RTC_DATA_ATTR 记录，不占这256字节池。
TTL 通过 IDF RTC 时钟计时，计入深睡时间，不要求 NTP/HA 校时。收到当前目标的有效整轮 report ACK，或完整清单/心跳确认时，以最近一次 offer 的 ttl_s 从当前时间续期，并连同 TTL 时长保存到 RTC；不依赖 HA。未配置 ACK 的状态发送本身不续期。TTL 到期或缓存不存在时重新发现。仅深睡复位且记录校验及配置身份匹配时恢复；冷启动重新初始化。

UDP 本地提交成功后才保存对应基准；本地发送失败的分包不提交其基准。已经成功的前面分包保留已提交结果。
状态包默认不请求 ACK；本地提交成功仍可能途中丢失。可用动作级 ack 配置请求独立的整轮收包确认，详见下节；ACK 不改变基准提交行为。
每 full_report_every 次 report 强制全量（1～65535，默认20）；冷启动、重新发现、配置身份变化也要求全量。强制全量发送当前有效值并更新其基准。
强制全量失败或有缺失输入时保持待全量标记，后续重试；它用于修复丢包和网关重启后的内存读数，恢复时延最多受下一次成功全量限制。
配置变化导致身份指纹不匹配时重置缓存和基准；这不会替代网关要求的 manifest_revision 递增。

回调参数：
- success：本轮 invalid/errors 都为0；全部未变化也成功。
- sent：本地 UDP 已提交的实体数，包括强制全量和控制状态；不是 HA 确认数。
- unchanged：采样有效但未达到变化条件的只读项数；全量时也可能计入 sent，两者不是互斥统计。
- invalid：没有有效读数、没有本轮新读数或文本超长的只读项数。
- errors：传输、握手、RTC 保存或组件错误数。部分失败时其他已成功分包不会回滚。

## 动态控制

control_ready 可以为 bool 或无参数 bool lambda，运行中持续求值。false→true 时立即尝试发送允许控制的 presence，之后按 heartbeat_interval 心跳。
true→false 立即尝试发送禁止控制的 presence，并停止控制心跳；通知丢失时网关30秒租约兜底。
heartbeat_interval 允许1～15秒，默认10秒。在线模式失败的发现/清单请求会按心跳周期重新尝试，不忙等。
control_ready 只是控制许可，既不阻止深睡，也不自动修改 Thread poll_period。应与本地深睡判定保持一致。
需要下行的短窗口中应使用合适的短轮询周期或保持接收；当前示例 poll_period=0，醒着持续接收。
shutdown 时尽力发 sleeping=true，不等待网络确认。

控制实体只填写现有 ID，例如 switches: [sys_led_enable]，也接受 {id: sys_led_enable}。名称和能力自动从源实体读取：
- switches：指向现有 switch，调用 control。
- numbers：指向现有 number，采用源 traits 的范围和 step。
- selects：指向现有 select，采用源 traits 的选项。
- buttons：指向现有 button，调用 press。
- text_inputs：指向现有 text，采用源 traits 长度，传输最多255 UTF-8字节。

这些项只进入网关 Web，不接入 HA。id 同时是源实体引用和报文身份，不创建第二个控制实体。
网关命令必须来自当前目标的地址/端口，匹配 gateway_id、node_id 和本次启动随机 session；执行前再次检查动态 control_ready。
每会话缓存最近32个 command_id 的结果，重试只返回缓存，不再次执行，尤其避免 button 重复按下。
结果 success 表示本地控制调用已派发，不代表异步硬件已物理完成；实际状态由下一次 report 发送，不伪造目标状态。
控制记录不跨重启保存；新 session 拒绝旧会话报文。不要在按钮回调内立即重启/深睡，否则结果包可能来不及发送。

## 本次验证范围

只做 ESPHome YAML/schema 及静态检查，没有固件编译、刷机或实机收发验证。
需自行编译后验证 Thread 多跳发现、清单确认、深睡恢复、UDP上报、本地触发器、动态控制和 HA 展示。

报文统一携带 `node_type: sed`。网关不因节点睡眠或长时间无报文而判定离线。

## 连接状态查询

- `id(thread_attach).is_thread_connected()` / `id(thread_attach).state`：由 openthread_info 的 thread_attach 二进制传感器提供，适合 Thread 图标，不要求已发现网关。
- `id(sensor_reporter).is_gateway_connencted()`：Thread 已入网，当前启动会话已获网关清单确认，目标 TTL 有效，且最近三个心跳间隔内收到有效网关回复。与 REED 一样用于查询网关通讯状态；无连接状态自动化回调。

允许常在线控制时，原有 presence 心跳请求网关核对清单并回复；没有新增传感器轮询。普通 SED 不为维持这个状态额外唤醒。单次 report 是否确认送达仍以该次 `on_report_success` / `on_report_fail` 为准。


## 手动重新发现网关

```yaml
# sensor_reporter 为 thread_sed_transporter 的组件 id。
- thread_sed_transporter.discovery:
    id: sensor_reporter
# 也可简写：
# - thread_sed_transporter.discovery: sensor_reporter
```

此 action 异步清除地址有效期及清单确认状态，并立即尝试发现；不采样、不创建 report，不触发 report 结果回调。即使 control_ready 为 false，也会服务本次发现，最长使用组件 timeout 预算。应在 Thread 就绪时调用；action 返回不代表发现完成。

已有采集中的 report 保持原采集预算，等待新目标及清单就绪；已有待 ACK 报文暂停普通重试，发现完成后沿用同一 report_id/seq/内容重发，恢复等待重新计时。不重复提交 RTC 基准，不重复触发 on_complete。无论手动发现还是自动恢复，重发收到的匹配 ACK 均可续期 TTL；最终 on_report_success/on_report_fail 仍只触发一次。

每轮自动重新发现最多一次，多个待确认轮次可共享正在进行的发现。未配置 ack 的 report 仅按目标有效期发现，不因无 ACK 自动恢复。低频事件节点休眠期间 TTL 仍递减，超过有效期后下次唤醒先发现；有效期内则直接发送，必要时在本次唤醒内完成一次恢复。

RTC 目标记录已增加 TTL 时长并升级版本。首次运行本版本时，旧版本 RTC 缓存和基准不会恢复，将重新发现并全量上报一次；之后正常保留。无需修改 manifest_revision，也无需更新网关协议。
