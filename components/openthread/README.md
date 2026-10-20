# 项目维护的 OpenThread 节点组件

fork 版本：**1.0.0**。基线：本机安装的 **ESPHome 2026.9.1** 包快照。每个原始文件的 SHA-256 记录于 `upstream.json`，许可证保存在 `LICENSE`。不声称本机包快照与上游 tag 的文件逐字节一致；今后更新应核对该清单，而不是覆盖式复制。

本组件用于 C6/H2 等 ESP32 原生 802.15.4 Thread 节点，搭配本项目 S3 `thread_br`。保留官方 `openthread:` 名称、接口、FTD/MTD、轮询动作、SRP/mDNS、API/OTA 集成。运行时仍使用 ESP-IDF 的 OpenThread 协议栈。首版维护范围为 **ESP32 + ESP-IDF**，不支持 nRF52/Zephyr；保留的上游 Zephyr 源文件不会在 ESP32 构建中启用。

最低 ESPHome 2026.9.1，首版配置/代码生成检查环境为 ESPHome 2026.9.1 + ESP-IDF 5.5.5 配置。支持的芯片列表沿用基线；不同芯片是否受 IDF 5.5.5 支持仍以框架为准。尚未在本次修改中编译或烧录验证。升级 ESPHome 核心或 IDF 时需要主动验证；本地 fork 不会自动同步官方修复，也不能隔离核心接口变更。

## 使用

见完整示例 [thread-node.example.yaml](../../thread-node.example.yaml)。在与本项目 `esphome/` 同级的 YAML 中使用：

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [openthread, openthread_info]
```

路径相对于节点 YAML。节点放在其他目录时，将 `path` 改为项目 `esphome/components` 的实际路径。只加载上述两个组件，避免节点加载 BR；S3 BR 继续只加载 `thread_br`，两者不能在同一固件中启用。构建/配置日志应显示覆盖内置 `openthread, openthread_info`，节点启动配置日志含 `Project fork: 1.0.0`。

`openthread:` 的 Dataset 字段继续使用官方的扁平配置，与 BR 的 `thread_network_config` 填相同的网络参数。示例保留旧的公开示例 Dataset，请替换为实际网络；不会自动读取 BR 的 YAML，也不自动同步凭据。

PSKc、Network Key、Extended PAN ID 支持 `0x` 整数或完整十六进制字符串；字符串建议加引号。整数按 16/16/8 字节补零；字符串必须分别为 32/32/16 个十六进制字符，可带 `0x`，不会静默接受截短的字符串。

迁移到 fork 后，删除节点中专门绕过前导零问题的 `CONFIG_OPENTHREAD_NETWORK_PSKC` 等手工覆盖，避免与 YAML 出现两份不同的网络参数。`framework.log_level` 可独立按需要保留，不是修复前导零的必要配置。

## 本地补丁

1. 固定长度序列化 PSKc、Network Key、Extended PAN ID，保留前导零，包括全零整数；校验宽度、格式、负数、溢出和布尔值。
2. PAN ID 接受 `0x0000` 并实际写入，拒绝广播值 `0xffff`。网络名按 UTF-8 字节校验 1..16；Mesh-Local Prefix 限定 fd ULA /64 且拒绝含主机位的输入。
3. OMR 选择遍历所有 Network Data 前缀，要求 stable/on-mesh/SLAAC/preferred /64；匹配完整 64 位，排除 Mesh-Local、RLOC、链路本地、多播及无效/弃用地址。同候选按路由 preference 优先，再按地址排序稳定选择。并不保证其他 LAN 主机已经安装到该 OMR 的路由。
4. 查询地址使用短时加锁，初始化未完成或锁繁忙时不阻塞 ESPHome 主循环。诊断传感器复用已持有的锁，不重复加锁。
5. 配套 `openthread_info` 增加明确的内部地址实体。无 OMR/节点 detached 时将 OMR 实体置空，不把内部地址当作替代，也不继续保留旧地址。锁繁忙时保留上一采样值。初始化原先未初始化的角色、RLOC16、频道、PAN ID 缓存。
6. 链路本地与多播地址直接按网络字节序判断 `fe80::/10`、`ff00::/8`，不调用 IDF 5.5 公共 API 中不存在的地址分类函数。

多个 OMR 可同时存在，实体只选一个；它显示的地址不一定与现有 API 连接当时选择的地址相同。OMR 仍由 BR 发布，不在节点写死，也不会因 RLOC16 更换就跟着变。

## 维护与验证

```powershell
python -B -m unittest discover -s tools/tests -p "test_openthread_fork.py"
esphome config esphome/thread-node.example.yaml
```

在仓库根目录、ESPHome Python 环境运行。检查凭据边界、sdkconfig 参数、外部组件加载及内存中的 C++ 代码生成，不调用固件编译器。临时文件统一放 `tools/.cache/`；测试不保存带凭据的配置。

用户编译并烧录节点后，应确认：带前导零的凭据可启动；OMR 实体显示 `fda1:...`（以实际 BR 前缀为准）；RLOC 改变时 OMR 保持正确；PC/DSM 可 ping OMR，HA API、SRP/mDNS、OTA 正常。MTD 行为仍需要相应硬件验证。

升级流程：保留现有快照 → 对比新上游与 `upstream.json` 对应基线 → 逐项评估并合并补丁 → 更新来源/哈希/本地补丁版本和验证记录。外部 Git 发布后应固定具体 tag/commit，不使用浮动 dev 分支。

## FTD 与 poll_period

`poll_period` 仅对MTD有效。FTD允许显式配置 `poll_period: 0s`，其含义为无操作，
不会生成MTD专用的 `set_poll_period()` 调用；FTD配置非零值时在配置校验阶段报错。
MTD显式配置零值和非零值均保持原有行为。

## IPv4 DNS服务器

在已有 `openthread:` 配置内增加 `ipv4_dns`，可以指定局域网或公网DNS：

```yaml
openthread:
  # 保留已有device_type、network_key/TLV等入网参数
  ipv4_dns: "192.168.32.1"  # 可选，默认"8.8.8.8"；只接受IPv4地址，不接受域名或IPv6
```

适用于本组件维护的ESP32 / ESP-IDF节点，FTD和MTD共用此配置。
参数写入 `CONFIG_OPENTHREAD_DNS_SERVER_ADDR`。节点取得网络内NAT64前缀后，
IDF自动将该IPv4 DNS地址合成为可经BR访问的IPv6地址；仍需可工作的NAT64路径。

此项只决定节点向哪个DNS服务器查询，不修改BR的Wi-Fi DNS，不依赖 `thread_gateway`，
也不自动继承BR通过DHCP取得的DNS。修改后需重新编译并更新节点固件，不属于运行时action。
如果要解析路由器中的内部域名，请填写提供这些记录的路由器/DNS服务器IPv4地址。
直接使用IPv4 SNTP地址等不需要域名解析的访问，不由此项决定目标服务器。

## DNS解析DEBUG日志

启用DEBUG级别即可记录节点经DNS64解析接口发起的请求，无需另加OpenThread开关：

```yaml
logger:
  level: DEBUG
  logs:
    openthread.dns: DEBUG
```

`openthread.dns` 记录请求域名、配置的IPv4 DNS、当前主DNS地址、解析得到的IPv4、
合成的NAT64 IPv6目标及耗时；立即返回/缓存结果与异步结果分别标记。
解析失败记录无结果或lwIP错误码；底层回调不提供具体DNS响应码，不能区分所有失败原因。
直接填写IPv4的访问标为 `Literal`，不视为实际域名查询。

这些是应用解析请求日志，不是DNS报文抓包；缓存命中或查询合并未必产生新的无线请求。
只观察本节点经过该DNS64接口的解析，不记录整个Thread网络的查询，也不记录BR的DNS转发。
使用链接包装调用原始IDF实现，不修改SDK缓存，不改变解析、重试和NAT64地址合成策略。
记录表容量随lwIP的DNS_TABLE_SIZE，满时不拦截请求，只跳过该请求的结果跟踪。
INFO及以下编译日志级别直接透传，不保留跟踪状态或打印域名日志。
