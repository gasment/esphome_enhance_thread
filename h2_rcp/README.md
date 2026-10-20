# H2 RCP 固件

此目录集中存放 H2 的构建脚本、IO/LED 配置和自定义固件源码。
编辑 [h2.yaml](h2.yaml)，在 ESPHome Python 环境中从仓库根目录运行：

```powershell
python h2_rcp/build_h2.py
python h2_rcp/build_h2.py h2_rcp/h2.yaml
```

未激活环境时，也可在 PowerShell 中直接指定 ESPHome 的 Python 解释器：

```powershell
& "C:\path\to\esphome\Scripts\python.exe" h2_rcp/build_h2.py
```

## 配置与环境

YAML 定义 H2 IO 和 LED 行为，UART0 / 460800 / 四线 follower 固定。
S3 对应 UART1 TX18/RX17，H2 默认 RX23/TX24；更改 IO 时保证实际接线匹配。
Thread 网络参数由 S3 和 ESPHome 终端定义，不写入 H2 RCP。

工具复用 ESPHome 2026.9+ 管理的 IDF 环境及缓存，版本由 YAML 固定。
首次构建可能安装缺少的 IDF、IDF Python 环境或 H2 RISC-V 编译器。
仅构建过 S3 时也可能需要补装 RISC-V 工具链，不需要手动克隆另一套 IDF。

已导出环境时，复用 `IDF_PATH`、`IDF_PYTHON_ENV_PATH` 及编译器/CMake/Ninja PATH，
并核对 IDF 版本。也支持显式指定已有安装：

```powershell
python h2_rcp/build_h2.py --idf-path "C:\path\to\esp-idf" --idf-python "C:\path\to\idf-python\Scripts\python.exe"
```

显式指定时，编译器等工具仍需来自相应已导出环境。
工具复制官方 `ot_rcp` 应用和共存实现到独立工程，不修改 IDF 或 S3 工程；
裁去 CLI/debug/iperf 示例依赖，不实现 RCP OTA。
`firmware/` 保存自定义 LED 实现、构建定义和 vendor 命令片段。构建时仅替换
生成工程使用的 NCP vendor hook，保留官方其他 vendor 命令；UART 收发通过
链接包装函数观察，不增加串口读取任务，不改变 OpenThread 的 HDLC 解码器。
检查 GPIO 重复、Flash 保留脚和用于 BOOT 协同的 GPIO9。
官方 ot_rcp 手动 UART 配置范围为 GPIO0..25；GPIO26/27 固定保留给原生 USB 日志，
工具拒绝将它们分配给 UART、共存或 LED。

## USB 日志

固件固定启用 **INFO** 级别日志，通过 **H2 原生 USB Serial/JTAG** 输出：
GPIO26 为 D-，GPIO27 为 D+。连接 H2 的 USB 接口，在串口终端中打开其枚举出的
USB 串口即可查看；这是 H2 自己的 USB，不是 S3 的 USB。
USB 虚拟串口的波特率设置不改变 RCP UART0 的 460800 波特率。

ESP-IDF 应用和二级 Bootloader 的控制台均选择 USB，不启用 UART 控制台。
OpenThread RCP 原本经 Spinel 转发的日志也重定向到 USB，不发送到 S3。
启动输出固件协议版本、LED 引脚及等待 Host 的提示；应用控制指令时输出序号和
LED 使能状态，Host 连接/超时变化时输出 INFO/WARN。普通心跳与 30 秒状态报告
不逐次输出 INFO 日志；另每 10 秒输出一条 `H2 alive` 运行状态，避免串口打开较晚时
错过全部启动日志。OpenThread 栈自身的日志粒度遵循 IDF 的 RCP 映射。

应用启动时显式安装 USB Serial/JTAG 驱动，并将 ESP_LOG 输出直接接到 USB TX 队列，
不依赖 stdout FILE 的状态。队列写入不等待，电脑未连接或不读取日志时允许丢弃，
不会为了等待日志发送而阻塞 RCP 通信。启动早期和二级 Bootloader 仍使用 IDF 控制台路径。
如果没有输出，确认打开 H2 自己的原生 USB 串口、关闭硬件流控，并保持打开至少 10 秒；
复位后 USB 可能重新枚举，需要重新连接实际串口。

这不启用 OpenThread CLI 或 Spinel 远程控制台。ROM 最早期启动信息的输出由芯片
ROM/硬件设置决定，不受应用和二级 Bootloader 的控制台配置控制。

## LED 自定义

默认绿色 LED 为 GPIO22，蓝色 LED 为 GPIO25。`h2.yaml` 的 `leds` 配置：

```yaml
leds:
  activity:
    pin: GPIO22
    inverted: false
    enabled_on_boot: true
    blink_interval_ms: 100
    hold_ms: 200
  connection:
    pin: GPIO25
    inverted: false
    enabled_on_boot: true
    blink_interval_ms: 500
    timeout_ms: 20000
```

`inverted: false` 为高电平点亮；低电平点亮的接法改为 `true`。
`blink_interval_ms` 是翻转间隔，完整亮灭周期为其两倍。
`activity.hold_ms` 从最后一次 UART 收发计时，须大于等于翻转间隔。
时间参数以毫秒为单位，范围 20..60000；连接超时范围 10000..60000。
所有 UART、共存、LED 引脚不能重复。不写 `leds` 时也使用上述默认值。

| 状态 | 绿色：Host 通信活动 | 蓝色：RCP 与 Host 连接 |
| --- | --- | --- |
| 正常且使能 | 收发时闪烁，空闲熄灭 | 常亮 |
| 正常但 Host 关闭对应灯 | 熄灭 | 熄灭 |
| 尚未收到有效心跳、心跳超时或 Host 报告故障 | 仍按绿灯使能显示活动 | 强制闪烁，忽略蓝灯关闭指令 |

连接正常指 S3 最近完成了一次标准 Spinel 属性往返并向 H2 发送健康心跳，
与 Thread 是否附着、Wi-Fi 或 HA 是否连接无关。UART 本身没有连接事件，
因此拔线后的故障显示存在检测延迟。H2 启动后先按未连接闪烁，收到健康心跳才常亮。
串口噪声可能点亮绿灯，但不能刷新连接心跳。心跳本身也属于通信活动。

H2 在自身 NVS 命名空间 `h2_leds`、键 `led_enabled` 中保存两灯使能掩码（bit0=Activity，bit1=Connection）。
`enabled_on_boot` 仅在没有有效持久化记录时作为默认值；H2 自身复位、S3 重启或 UART 断开都不会清除已保存设置。
启动顺序为 NVS 初始化 → 读取使能值 → 启动本地 LED 逻辑 → 启动 OpenThread。
故障逻辑优先：无健康心跳、心跳超时或 Host 报告故障时，本地蓝灯强制闪烁；健康恢复后重新按持久化变量控制，无需 S3 再发恢复指令。

S3 仅修改和查询 H2 的变量，不持久化、不恢复、不重放状态。H2 接收到 SET 后将其排入队列；LED 任务在临界区和 Spinel 接收任务之外写 NVS，提交成功才应用新值并更新确认序号。
同值重试不重复写 Flash；首次明确设置与默认值相同时仍建立持久化记录。保存失败保留原值、不确认新序号，并输出错误日志，供 Host 的原有重试机制处理。
心跳和查询不会写 NVS，也不会修改使能值。使用独立命名空间，不自动擦除共享 NVS。

## Host 控制协议

LED 控制为配套 S3 组件的基础功能，无需额外启用。每轮约 5 秒的后台
Spinel 探测发送一次健康心跳。示例 `esphome/thread-br.yaml` 提供
`H2 Green LED Enable`、`H2 Blue LED Enable` 两个模板开关：

- 设置 `restore_mode: DISABLED` 和 `optimistic: false`，S3 开关仅跟随 H2 实际反馈，用户动作入发送队列。
- 后台工作线程约每 100ms 处理动作，按位修改，避免同时操作两灯时覆盖另一灯状态。
- H2 执行后返回当前使能掩码及动作序号，S3 仅根据有效 H2 反馈发布开关状态。
- 动作未确认时每 1 秒重试，总计最多发送 3 次；失败记录警告，不伪造成功状态。
- H2 启动、执行动作、收到查询或连接状态变化时主动报告；空闲时也每 30 秒报告一次。
- S3 尚未取得有效报告时每 5 秒查询一次。超过 65 秒没有报告，停止发布新开关状态并重查；
  模板开关可能保留最后一次确认值，应结合 RCP Connectivity 判断可用性。

首次有效报告前不主动发布开关状态，心跳和查询均不覆盖 H2 使能值。
S3 的现有启动流程会复位 H2，此时开关将同步 H2 的 YAML 启动默认值。
正常调度下动作与查询反馈在约 200ms 内排队发送，实际延迟受 OpenThread 工作负载影响。
超时应留出锁等待和通信恢复余量，建议保留 20000ms。
长时间占用 OpenThread 锁也可能触发心跳超时，随后成功心跳会恢复状态。

协议为标准 Spinel HDLC-lite 帧中的 **vendor command 15360 (0x3c00)**，
使用 IID=0、TID=0；反馈采用 `PROP_VALUE_IS` 的 vendor property 15360 通知，
不占用标准 Spinel 请求的事务 ID。H2 在 OpenThread 工作线程中排队发送反馈，
S3 在既有 Spinel 解码路径接收，不另建 UART 读写任务。
不发送文本指令，不允许其他组件另建 UART 读写循环。解码后的完整帧如下：

| 字节 | 内容 |
| --- | --- |
| 0 | `80`：Spinel Header，IID=0、TID=0 |
| 1..2 | `80 78`：15360 的 packed uint 编码 |
| 3..6 | `48 32 4c 45`：ASCII `H2LE` |
| 7 | `02`：协议版本 |
| 8 | 操作：`00` 心跳、`01` 按位设置、`02` 查询 |
| 9 | 更新掩码：bit0 绿灯、bit1 蓝灯；心跳/查询必须为 0 |
| 10 | 目标使能值：bit0 绿灯、bit1 蓝灯；仅更新掩码指定的位；心跳/查询为 0 |
| 11 | Host 通信健康：`01` 正常、`00` 故障；仅心跳更新连接状态 |
| 12..13 | 动作序号 LE16：设置为非零，心跳/查询为 0 |

H2 状态帧的解码结构为 `80 06 80 78` 加以下 9 字节：

| 状态载荷字节 | 内容 |
| --- | --- |
| 0..3 | ASCII `H2LE` |
| 4 | `02`：协议版本 |
| 5 | H2 当前使能掩码，bit0 绿灯、bit1 蓝灯 |
| 6 | H2 当前判定的 Host 连接状态，`00`/`01` |
| 7..8 | 最近执行的设置序号 LE16，H2 启动为 0 |

开关只反映使能位，连接故障强制闪烁不改变蓝灯使能位。
此版本与之前的单向 `H2LE/1` 不兼容，升级时同时更新 H2 与 S3。

外层追加标准 HDLC FCS（CRC-16/X-25，小端），进行标准字节转义并添加 `7e` 边界。
H2 仅接受正确长度、签名、版本、掩码和状态的命令；损坏的 HDLC 帧由 OpenThread 丢弃。
该命令号为本项目约定，不代表通用 Espressif RCP 接口。
自定义 Host 应在完成一次真实 Spinel 往返后发送健康心跳，周期小于超时的三分之一；
禁止在通信线程已故障时仍无条件发送正常心跳。

S3 组件与本定制 H2 固件配套使用；普通官方 RCP 不实现此命令。
本定制 H2 固件需要配套 Host 心跳才能显示连接成功。

## 输出位置

- `h2_rcp/.cache/build/<名称>-<版本>-<指纹>/`：生成源码和构建中间文件，可删除。
- `h2_rcp/output/<名称>-<版本>-<指纹>/`：固件及配置、哈希清单。
- `firmware.factory.bin`：完整合并镜像，从 **0x0** 烧录。
- 独立镜像、实际 `sdkconfig`、生成的 `h2_led_config.h`、`source-manifest.json`、`firmware-manifest.json` 同目录保存。

IO、LED 配置、自定义源码或官方复制源码变化会使用新的指纹目录，避免复用旧产物。
导出前会核对实际 sdkconfig，记录固件 SHA256。
默认只构建，不操作串口。明确需要刷写时使用：

```powershell
python h2_rcp/build_h2.py --flash --port COM5
```

刷写 H2 时让 S3 保持复位或断开两颗芯片的 UART，避免自动恢复打断下载。

## 可选检查

由用户按需执行，不会自动运行：

```powershell
python h2_rcp/build_h2.py --check-config
python h2_rcp/build_h2.py --prepare-only --idf-path "C:\path\to\esp-idf"
```

`--check-config` 不下载环境；`--prepare-only` 不编译，但未指定已有 IDF 时可能安装环境。
固件编译和实机验证由用户执行。本次 LED 改动尚未编译和上板验证。

上板时依次确认：LED 极性；未启动 Host 时蓝灯闪烁；正常心跳时蓝灯常亮；
两颗灯关闭后均熄灭；关闭蓝灯后停止 Host，超过超时仍会闪烁；
恢复通信后蓝灯重新熄灭；重新使能后正常显示；持续通信时绿灯闪烁；
S3 开关跟随 H2 上报而不是动作立即变化；重启后同步 H2 启动值；
无操作时仍每 30 秒收到状态；H2 单独复位后 S3 能更新使能状态。
