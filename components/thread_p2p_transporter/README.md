# thread_p2p_transporter

Thread 网内单播通讯，支持自动多跳，不依赖 Wi-Fi、HA、BR 或网关服务。
依赖本项目的 `openthread`，当前支持 ESP32 / ESP-IDF。

本文按 **SED 发送 → REED 发送 → REED 接收** 配置。示例统一使用你的网络前缀：

```yaml
mesh_local_prefix: "fdd1:dd68:9fa3:d942::/64"
```

这是已有 `openthread:` 的配置项，不属于 `thread_p2p_transporter:`。
本文只列 P2P 组件及其 action；`external_components`、入网参数和本地实体由节点已有配置提供。

| 示例节点 | OpenThread构建 | P2P本机地址 |
|---|---|---|
| SED发送端 | `device_type: MTD` | 不配置，使用原生ML-EID |
| REED发送端 | `device_type: FTD` | `fdd1:dd68:9fa3:d942::1001` |
| REED接收端 | `device_type: FTD` | `fdd1:dd68:9fa3:d942::1002` |

这里的REED配置同样适用于已经晋升为Router的FTD，角色变化不影响P2P收发。
每个节点只配置一个组件实例；发送和接收可以在同一个实例中同时使用。

## 1. SED发送配置

SED使用MTD构建，休眠和轮询由节点自身配置控制。本组件只发送业务消息，并接收发送所需的ACK；
不能配置本机 `address`、`port` 或 `on_receive`。

**组件配置：**

```yaml
thread_p2p_transporter:
  id: p2p_transport

  send_to_addr_list:
    - id: receiver_node
      address: "fdd1:dd68:9fa3:d942::1002"
      port: 18520  # 目标监听端口，可省略，默认18520

  on_send_success:
    then:
      - logger.log:
          format: "P2P ACK成功：flag=0x%02X，message_id=%u"
          args: ['static_cast<unsigned>(send_flag)', 'static_cast<unsigned>(message_id)']
          level: INFO
      # 可在此调用自己的深睡流程

  on_send_fail:
    then:
      - logger.log:
          format: "P2P发送失败：flag=0x%02X，message_id=%u"
          args: ['static_cast<unsigned>(send_flag)', 'static_cast<unsigned>(message_id)']
          level: WARN
      # 失败后的重试或深睡策略由自己的自动化决定
```

**发送action：** 放入按键、采样脚本等自动化的 `then` 中。

```yaml
- thread_p2p_transporter.send:
    # id: p2p_transport  # 可省略，自动关联唯一实例
    send_to: receiver_node
    send_flag: 0x12
    payload_type: bool
    payload: true
    ack:
      timeout: 500ms
      resend: 2
```

调用前应确保Thread已附着，且 `id(p2p_transport).is_ready()` 为true。
等待ACK期间必须保持唤醒；组件不会自动调整 `poll_period` 或阻止深睡。
长轮询周期不适合短ACK超时，应按实际接收窗口设置timeout。

## 2. REED发送配置

前置 `openthread.device_type: FTD`。与SED不同，FTD需要配置全网唯一的本机应用地址。

**组件配置：**

```yaml
thread_p2p_transporter:
  id: p2p_transport
  address: "fdd1:dd68:9fa3:d942::1001"
  port: 18520  # 本机监听端口，可省略，默认18520

  send_to_addr_list:
    - id: receiver_node
      address: "fdd1:dd68:9fa3:d942::1002"
      port: 18520  # 目标监听端口，可省略，默认18520

  on_send_success:
    then:
      - logger.log:
          format: "P2P ACK成功：flag=0x%02X，message_id=%u"
          args: ['static_cast<unsigned>(send_flag)', 'static_cast<unsigned>(message_id)']
          level: INFO

  on_send_fail:
    then:
      - logger.log:
          format: "P2P发送失败：flag=0x%02X，message_id=%u"
          args: ['static_cast<unsigned>(send_flag)', 'static_cast<unsigned>(message_id)']
          level: WARN
```

**发送action：** 以下三种负载分别对应接收示例中的0x12、0x13、0x14。
`local_temperature` 需为本地已定义的sensor；其他两条使用固定示例值。

```yaml
# bool：控制对端继电器；改为false则关闭
- thread_p2p_transporter.send:
    send_to: receiver_node
    send_flag: 0x12
    payload_type: bool
    payload: true
    ack:
      timeout: 500ms
      resend: 2

# float：发送本地温度
- thread_p2p_transporter.send:
    send_to: receiver_node
    send_flag: 0x13
    payload_type: float
    payload: !lambda 'return id(local_temperature).state;'
    ack:
      timeout: 500ms
      resend: 2

# string：直接填写目标地址，不使用别名；省略ack表示无需确认
- thread_p2p_transporter.send:
    address: "fdd1:dd68:9fa3:d942::1002"
    send_flag: 0x14
    payload_type: string
    payload: "ON"
```

需要同时接收时，把下一节的 `on_receive` 合并到此实例中，保留自己的本机地址，
并根据实际发送目标调整 `send_to_addr_list`。不要在同一节点再定义一个P2P实例。

## 3. REED接收配置

前置 `openthread.device_type: FTD`。接收端无需登记发送者，按照 `send_flag` 与负载过滤。
下面引用的实体需已在本地定义：`local_relay`（switch）、`remote_temperature`（sensor）、
`remote_text`（text_sensor）。

**组件配置：** 包含业务接收、直接发布传感器，以及收到查询后主动发送回复的示例。

```yaml
thread_p2p_transporter:
  id: p2p_transport
  address: "fdd1:dd68:9fa3:d942::1002"
  port: 18520

  # 仅供主动回复使用，不是接收来源白名单
  send_to_addr_list:
    - id: sender_node
      address: "fdd1:dd68:9fa3:d942::1001"
      port: 18520

  on_receive:
    then:
      # bool：按消息标识和值控制本地继电器
      - if:
          condition:
            and:
              - thread_p2p_transporter.send_flag: 0x12
              - thread_p2p_transporter.payload: true
          then:
            - switch.turn_on: local_relay

      - if:
          condition:
            and:
              - thread_p2p_transporter.send_flag: 0x12
              - thread_p2p_transporter.payload: false
          then:
            - switch.turn_off: local_relay

      # float：自动解码并发布到本地sensor；类型不匹配时跳过并记录警告
      - if:
          condition:
            thread_p2p_transporter.send_flag: 0x13
          then:
            - thread_p2p_transporter.publish:
                sensor_id: remote_temperature

      # string：自动发布到本地text_sensor
      - if:
          condition:
            thread_p2p_transporter.send_flag: 0x14
          then:
            - thread_p2p_transporter.publish:
                text_sensor_id: remote_text

      # 同时作为发送端：收到查询后，将本地状态固定回复给sender_node
      - if:
          condition:
            and:
              - thread_p2p_transporter.send_flag: 0x20
              - thread_p2p_transporter.payload: "GET_STATE"
          then:
            - thread_p2p_transporter.send:
                send_to: sender_node
                send_flag: 0x21
                payload_type: bool
                payload: !lambda 'return id(local_relay).state;'
                ack:
                  timeout: 500ms
                  resend: 2

  # 这两个回调用于本机主动发送的结果，包含上面的业务回复
  on_send_success:
    then:
      - logger.log: "本机发送的消息已收到ACK"

  on_send_fail:
    then:
      - logger.log: "本机发送的消息确认失败"
```

接收端自动回复发送者请求的ACK，不需要配置action。ACK回复到实际来源，且不会进入 `on_receive`。
上面的 `GET_STATE` 是单独的业务协议，固定回复给 `sender_node`；SED没有业务接收能力，不能使用该查询回复方式。

**更多接收写法：** 以下片段放在 `on_receive.then` 中。

```yaml
# float范围过滤：大于25且小于30，above/below至少配置一个
- if:
    condition:
      and:
        - thread_p2p_transporter.send_flag: 0x13
        - thread_p2p_transporter.payload:
            above: 25.0
            below: 30.0
    then:
      - logger.log: "温度位于指定范围"

# lambda直接读取已解码的值，无需解析字符串
- lambda: |-
    if (send_flag == 0x13 && payload.is_float()) {
      ESP_LOGI("p2p", "温度：%.2f", payload.as_float());
    }
```

`payload` 还提供 `is_bool()` / `as_bool()` 和 `is_string()` / `as_string()`。
自动发布action也支持 `binary_sensor_id`（bool）；与 `sensor_id`（float）、`text_sensor_id`（string）三选一。
发布经过本地实体原有的filters和回调。接收条件及publish只在接收自动化参数上下文中使用，
延时后仍读取当前自动化的原消息，不依赖全局“最后一条消息”。

## 公共配置规则

| 项目 | 规则 |
|---|---|
| 本机与目标地址 | 当前生效的Mesh-Local /64内的单播地址；本机地址全网唯一 |
| 示例前缀 | `fdd1:dd68:9fa3:d942::/64`，不是OMR前缀，也不是多播组地址 |
| 禁止的地址 | 全零后缀、RLOC/ALOC范围、保留子网anycast范围；发送目标不能是本机 |
| `send_to_addr_list` | 可选，最多128项，绑定id、address和目标port；port默认18520，不参与接收过滤 |
| 别名id | 本机内部名称，1～32位ASCII字母、数字、下划线或连字符，必须唯一 |
| 发送目标 | action中的 `send_to` 和 `address` 必须二选一 |
| `port` | 1～65535；本机监听、目标表条目默认18520；action可显式覆盖目标端口 |
| `send_flag` | 必填，1字节，0x00～0xFF；含义自行约定，不提供身份认证 |
| `payload_type` | 必填，固定为 `bool` / `float` / `string` |
| `payload` | 必填，按声明类型转换、校验并编码；接收端自动解码 |
| 模板 | `send_to`、`address`、`port`、`send_flag`、`payload` 支持模板 |
| float | IEEE-754 float32，拒绝NaN、无穷大和溢出的静态值 |
| string | UTF-8，最多1208字节；单个UDP报文最多1232字节，不做应用层分包 |

发送端优先使用action显式指定的 `port`；未指定时，按 `send_to_addr_list` 中该目标的 `port` 发送。
直接使用 `address` 且action未指定端口时，使用18520。接收端组件顶层 `port` 是本机监听端口，
需要与发送目标的端口一致；MTD不配置本机监听端口，但可以在目标表中配置对端端口。

条件严格区分类型：`true` 匹配bool，`1` / `1.0` 匹配float，`"true"` 匹配string。
模板按声明类型返回bool、float或std::string。静态值转换失败时配置报错；
运行时非法负载拒绝发送并记录原因。旧 `peers`、`receiver`、`src_flag` 等配置已移除。

## ACK规则与限制

| 项目 | 行为 |
|---|---|
| 是否启用 | `ack`为可选项；完全省略时不启用，也不触发发送结果回调 |
| `timeout` | 每次尝试的等待时长，10ms～60s，默认500ms |
| `resend` | 初次之外的重发次数，0～10，默认2；500ms/2约等待1.5s |
| 成功回调 | 收到对应ACK触发一次 `on_send_success` |
| 失败回调 | 重试耗尽或本地拒绝发送，触发一次 `on_send_fail` |
| 回调参数 | `send_flag`（uint8_t）、`message_id`（uint32_t） |
| ACK含义 | 接收端校验通过后、执行自动化前立即尝试回复，不代表动作执行完成 |
| 发送action | 非阻塞，后续then继续执行；需要等待结果时使用回调 |
| 本地未就绪 | 拒绝发送，不缓存等待连接；启用ACK时异步回调失败 |
| UDP提交失败 | 已就绪时的提交失败消耗本次尝试，保留配置中的后续重发 |
| 去重 | 最近128条消息，最长15min；命中时回复ACK，不重复执行接收自动化 |
| 容量限制 | 最多8条待确认消息，接收队列16包 |
| 关闭/深睡 | 取消仍在等待的消息，不自动阻止深睡 |

ACK根据对端地址与端口、启动会话、消息编号和send_flag匹配。
去重表超出容量时淘汰最早项，接收端重启也会丢失记录，不提供跨重启的严格一次执行保证。

## 协议与验证记录

收发双方均需使用类型化协议，不兼容之前的原始UDP负载。协议头共24字节：
`P2P1(4) / kind(1) / payload_type(1) / send_flag(1) / options(1) / session(8) / message_id(4) / payload_length(2) / reserved(2)`。
整数使用网络字节序；kind=1为数据、2为ACK；type=1为bool、2为float、3为string。
ACK不包含负载，type=0，回显session、message_id和send_flag。

已使用ESPHome 2026.9.1 / ESP-IDF 5.5.5，在ESP32-H2上完成 `openthread`、`openthread_info`、
`thread_p2p_transporter`、`thread_reed_transporter` 及其 `thread_entity_bridge` 依赖的共存编译。
覆盖三种负载、接收过滤、延时上下文、本地发布、接收回调内发送和ACK回调。
构建检查资料位于项目 `temp`，未刷写设备或验证实机收发、多跳及ACK丢失重试。
