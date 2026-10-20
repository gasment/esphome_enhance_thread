# 项目配套 OpenThread Info

与相邻的 [openthread fork](../openthread/README.md) 一起通过 `external_components` 加载，基线和许可证见 `upstream.json`、`LICENSE`。fork 版本 1.0.0。保留原有文本和数值诊断配置。

```yaml
text_sensor:
  - platform: openthread_info
    ip_address:
      name: "Thread OMR IPv6"
    mesh_local_address:
      name: "Thread Mesh-Local IPv6"
    rloc_address:
      name: "Thread RLOC IPv6"
    link_local_address:
      name: "Thread Link-Local IPv6"
```

| 字段 | 含义 | 从 PC/HA LAN 访问 |
|---|---|---|
| `ip_address` | BR 发布前缀下的 OMR 地址 | 使用此地址，LAN 仍需要到 OMR 的路由 |
| `mesh_local_address` | Thread 网内稳定端点 ML-EID | 不用于普通 LAN 访问 |
| `rloc_address` | Thread 内部路由定位地址 | 不用于普通 LAN 访问，随拓扑变化 |
| `link_local_address` | Thread 链路本地地址 | 不能用 PC 网卡接口直接访问 |

内部地址实体在节点未附着时置空。OMR 尚未获得/已经失效时也置空，不回退到 RLOC 或 ML-EID。暂时抢不到 OT 锁时跳过本次更新。

新增实体使用 fork 专有接口，必须同时加载本项目 `openthread`。不要只覆盖此组件并搭配官方 `openthread`。

## Thread 入网状态

```yaml
binary_sensor:
  - platform: openthread_info
    thread_attach:
      id: thread_attach
      name: "Thread Attach"
      internal: true
```

内置选项 `thread_attach` 是诊断类 connectivity 二进制传感器；Child/Router/Leader 为 ON，Disabled/Detached 为 OFF。启动时发布当前值，之后在主循环中跟踪状态变化并 publish，可使用原生 on_state/on_press/on_release。无需 update_interval，不触发 Thread 扫描、网关发现或传感器采样。

`id(thread_attach).is_thread_connected()` 查询底层当前入网状态，`id(thread_attach).state` 读取发布后的值（如配置 filters，会经过过滤）。不调用受锁保护的原生 OT API，只读取本项目 openthread 的缓存状态。与 sensor/text_sensor 平台一致，直接使用唯一的 OpenThread 实例，不提供 `openthread_id` 配置项。SED/REED transporter 只提供 `is_gateway_connencted()` 网关通讯查询，不再提供 `is_connected()` 或 `is_thread_connected()`。

## Router 最佳邻居链路质量

仅提供 text_sensor，数值 sensor 版本已移除：

```yaml
text_sensor:
  - platform: openthread_info
    best_router_link_quality:
      id: best_router_link_quality
      name: "Best Router Link Quality"
      update_interval: 10s  # 可选，默认10s；支持never及component.update
```

仅在当前角色为 Router 或 Leader 时有效，不以配置的 FTD 能力判断。
遍历已建立直接链路的 Router 邻居，排除本机、未分配和仅经多跳到达的 Router；
子设备不参与。每个邻居取 `min(LinkQualityIn, LinkQualityOut)`，最终取最大值并转换为文本：

| 内部等级 | 发布文本 |
|---|---|
| 3 | `best` |
| 2 | `good` |
| 1 | `normal` |
| 0 | `worst` |
| 非Router/Leader | `unknown` |

没有直接 Router 邻居时为 `worst`。Child/REED、Detached、Disabled 或MTD构建时发布
`unknown`，不保留之前的Router评价。首次读取及评价变化时发布。

它反映本机最好的直接 Router 链路，不代表到特定网关的端到端质量，亦不是RSSI/dBm。
沿用诊断类文本传感器，可配置原生 filters、on_value；只读取本地路由表，不发起无线探测。
暂时抢不到OpenThread锁时跳过本次更新。原有数值版配置需要移至 `text_sensor:` 下；
若被 REED/SED transporter 引用，也应从 `sensors` 列表移至 `text_sensors` 列表。
