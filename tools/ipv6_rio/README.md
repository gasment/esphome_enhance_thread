# DSM Thread RA 路由学习

补充 DSM 缺失的 IPv6 Route Information Option（RIO，RA 选项 24）处理。无需修改 S3、C6 固件或固定 OMR 前缀。工具只依赖 Linux 上的 Python 3.7+ 标准库和 `ip` 命令，需要 root。HA 使用 host 网络时直接共享宿主学习到的路由。

## 先确认内核

`5.10.55+` 版本号本身不能证明 Synology 是否启用了 RIO。执行：

```sh
sysctl net.ipv6.conf.ovs_eth2.accept_ra
sysctl net.ipv6.conf.ovs_eth2.accept_ra_rt_info_max_plen
```

若第二项存在，优先使用内核原生学习：

```sh
sysctl -w net.ipv6.conf.ovs_eth2.accept_ra=2
sysctl -w net.ipv6.conf.ovs_eth2.accept_ra_rtr_pref=1
sysctl -w net.ipv6.conf.ovs_eth2.accept_ra_rt_info_max_plen=64
```

等待新的 S3 RA，检查是否获得 OMR 路由。若仍不生效，继续检查抓包、防火墙、内核配置；不要仅凭版本判断。当前设备反馈第二项不存在，可使用下面的用户态工具。它不会修改上述 sysctl，也不接管地址自动配置、PIO 或默认路由。

## 先观察，再启用

将整个 `tools/dsm_ra/` 复制到 NAS 的固定目录，例如 `/volume1/scripts/thread-ra/`。下面路径仅为示例，请替换成实际路径。

```sh
command -v python3
command -v ip
python3 /volume1/scripts/thread-ra/learn_routes.py --interface ovs_eth2
```

默认只观察，不添加、删除路由。会发送 Router Solicitation 请求 RA，预期日志：

```text
RA fe80::a6cb:8fff:fee2:a980: fda1:d068:b97e:1::/64 preference=0 lifetime=1800
```

地址、优先级和有效期以实际通告为准。若没有日志，增加 `--debug`，并在另一个 SSH 窗口抓包（DSM 未安装 tcpdump 时先跳过）：

```sh
tcpdump -ni ovs_eth2 -vv 'icmp6 and ip6[40] == 134'
```

如只有 PIO、没有 RIO，本工具也不能凭空推导 OMR 路由。RA 的 Router Lifetime 为 0 不影响其中独立的 RIO 有效期。

确认收到后按 Ctrl+C，以正式模式运行：

```sh
python3 /volume1/scripts/thread-ra/learn_routes.py --interface ovs_eth2 --apply
```

可选 `--router fe80::a6cb:8fff:fee2:a980` 仅接受指定 BR，也支持重复指定多个 BR；不指定则自动接受接口上有效的 RA，替换 S3 时无需修改地址。默认只学习 `fc00::/7` 内的 `/64` RIO，适用于当前 ULA Thread OMR，不学习公网前缀、默认路由、PIO。源地址白名单不是身份认证。

## 验证

保留现有 SSH 窗口，在另一个窗口执行：

```sh
ip -6 route show table main proto 186
ip -6 route get fda1:d068:b97e:1:e0cc:ac26:106c:94c0
ping6 -c 4 fda1:d068:b97e:1:e0cc:ac26:106c:94c0
```

确认 `proto 186` 的动态路由存在后，可删除之前手工添加的那一条（此前命令默认 metric 1024）：

```sh
ip -6 route del fda1:d068:b97e:1::/64 via fe80::a6cb:8fff:fee2:a980 dev ovs_eth2 table main metric 1024
```

删除后再检查 `route get` 和 ping，确认流量使用动态路由。工具按高/中/低优先级使用 metric 2048/3072/4096，已有 metric 1024 的手工路由优先，因此不删除它无法验证实际切换。

## DSM 自动运行

先完成前台验证。在 DSM「控制面板 → 任务计划」创建：

1. 「触发的任务 → 用户定义的脚本」，用户 root，事件为开机。
2. 脚本如下，Python 路径使用 `command -v python3` 得到的绝对路径替换示例。

```sh
mkdir -p /volume1/scripts/thread-ra/logs
nohup /usr/local/bin/python3 -u /volume1/scripts/thread-ra/learn_routes.py --interface ovs_eth2 --apply >> /volume1/scripts/thread-ra/logs/learner.log 2>&1 < /dev/null &
```

再创建每 5 分钟运行一次的 root 计划任务，使用相同脚本，作为异常退出后的自动重启。每个接口只有一个正式实例；重复启动会立即退出。开机网卡暂未就绪也由定时任务重试。观察模式不占用正式实例锁。

日志统一在 `logs/learner.log`，请在 DSM 上配置轮转或定期清理；周期任务重复启动会增加一条“already runs”日志。这个目录不应放在 Web 公共访问目录。

## 生命周期与边界

- 校验 RA 类型、code、链路本地源地址、接收接口、Hop Limit 255、ICMPv6 校验和和选项长度。
- 每个前缀、每个通告路由器独立保存有效期；刷新延长，lifetime=0 立即撤销，未再次出现的 RIO 保留至到期。
- 同前缀多个 BR 优先选择高 preference；相同时按下一跳地址稳定选择。被选 BR 的 RIO 到期/撤销后选择其他仍有效的 BR。不会主动探测 BR 的端到端健康；故障切换取决于 RIO 有效期，不是完整的内核 NUD 路由选择实现。
- 路由写入 main 表，下一跳自动取 RA 源地址，出口绑定 ovs_eth2。当前 DSM 的 source rule 查询专用表失败后会回落 main，无需修改规则。可用 `--table` 改表。
- 专用协议编号 **186**，只供本工具使用；正式启动和正常退出会清理所选表、接口下该编号的路由，不删除其他协议的路由。不要在同一接口/表上复用该编号。首次安装使用 `add`，遇到同 metric 冲突报错，不覆盖其他来源路由。
- 每条路由都设置内核 `expires`。即使被 kill -9，仍会到期删除；超长/无限 RIO 使用最长一天、每小时续期的内核租期。如果 DSM 的 `ip`/内核不支持 `expires`，工具报错，不降级成永久路由。
- 停止：先禁用开机及周期任务，找到实际进程后 `kill <PID>`，正常退出会清理路由。异常残留也可定向清理：

```sh
ip -6 route flush table main dev ovs_eth2 proto 186
```

这只补齐单播可达性；mDNS 服务代理仍由 S3 完成，不会因此跨网段转发 mDNS。

## 本地检查

不访问真实网卡、不修改路由、不编译固件：

```powershell
python -B -m unittest discover -s tools/dsm_ra -p "test_*.py"
```

协议依据：[RFC 4191](https://www.rfc-editor.org/info/rfc4191/) 和 [RFC 4861](https://www.rfc-editor.org/info/rfc4861/)。
