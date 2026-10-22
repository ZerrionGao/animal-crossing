# ACNH 时间初始化中止与适配边界

第十二次启动在地区查询后触发原 SDK Break：reason `0x0`，result `0x74`（日志以十六进制输出）。主模块 `main+0x9817c4` 先调用被宿主替换的 `nn::time::Initialize`，随后 `main+0x9817d4` 调用尚未覆盖的 `AdjustableUserSystemClock::Adjust`；回溯中的 `main+0x9817d8` 是返回地址。

原 SDK `Adjust`（`sdk+0x43ecd8`）先读取 `StandardUserSystemClock::GetSystemClockContext`。后者在 `sdk+0x4415f8` 检查 SDK 全局初始化状态，未初始化时在 `sdk+0x44162c` 返回 `0x74`，随后 Adjust 主动中止。旧宿主 Initialize 只设置本地布尔值，因此混用宿主初始化与原 SDK 状态是首因；跳过 Break 或直接返回成功都不能修复时钟。

本轮对 ACNH 统一公共接口后端，不写 SDK 私有地址：以互斥锁保护初始化引用计数和可调整时钟的锚点。`Adjust()` 是无参数、无有定义 Result 的操作，采样宿主墙钟与连续时钟；`GetCurrentTime(PosixTime*)` 通过 w0 返回 Result，并从同一锚点按连续时间推进。再次 Adjust 才采纳宿主墙钟变化。Finalize 到引用计数零才清空调整状态。连续时钟采用 `mach_continuous_time`，包括睡眠经过的时间。

主模块 25 个时间导入中，旧后端已覆盖 11 个，subsdk0 唯一的用户时间读取也已覆盖。原 SDK 的 6 个未覆盖入口是日期验证、星期计算、日历比较/相减及两个时钟点的纯计算；它们不读时间初始化全局状态。另 8 个入口依赖状态或服务，本轮补 Adjust、其 GetCurrentTime、GetDeviceLocationName、LoadTimeZoneRule 和带规则的 ToCalendarTime。网络时钟精度检查及标准连续时钟点读取仍未覆盖；带规则的 ToPosixTime 已注册明确失败的保护入口，避免把宿主规则传给原 SDK。不能称为完整时间服务支持。

待验收：游戏内实际时钟值与连续推进、跨进程时钟源标识、跨天、系统时间修改、睡眠/前后台、存档时间判断及网络校时真实性。当前共用后端用宿主墙钟构造部分用户/网络快照，不代表已验证 Switch 的独立网络时钟行为。首帧启动与 C 阶段存档时间闭环分开记录。


同一无条件初始化路径在 `main+0x9817f8/0x981808/0x98181c` 依次读取地区名称、加载规则并转换日期，因此三个正向时区接口与时钟后端一起接入。布局按本地 SDK 调用证据和 [libnx time.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/time.h) 核对：LocationName 36 字节，TimeZoneRule 0x4000，CalendarTime 8 字节，CalendarAdditionalInfo 24 字节。

时区采用 [Core Foundation CFTimeZone](https://developer.apple.com/documentation/corefoundation/cftimezone)。规则缓冲区保存版本、名称和有界 TZif 数据副本，不保存宿主指针；转换从副本恢复指定规则，读取偏移、夏令时和缩写，再转换日历。原始 TZif 长度、计数和扩展块在进入 CF 前检查；未知名称、损坏或超大规则明确停止。规则可以整体复制，加载后系统时区变化不会替换已加载数据。此格式仅供 HLE 消费，不能交给原 SDK。

时钟与时区联合 12 项定向测试通过，含首次初始化即读、引用计数、连续时间推进、重新校准、并发，以及 UTC、香港 +08:00、纽约春秋夏令时、规则复制和损坏输入。两轮独立窄审通过。完整项目测试运行 54 项，53 项通过、1 项大镜像测试默认跳过；真实镜像此前已显式运行通过。运行日志为 `work/acnh/tests-time-zone.log`；这些不等同于游戏内时钟验收。
