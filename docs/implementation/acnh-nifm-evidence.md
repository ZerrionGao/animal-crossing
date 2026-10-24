# ACNH NIFM 请求创建依据

第二十次启动在 `work/acnh/3.0.0/boot-twentieth.log` 中实际观察到 `nifm:u` 命令 4：type 6、4 字节参数、无缓冲区，随后因命令未实现而停止。原 SDK 的 `NetworkConnectionC2`（`sdk+0x402960`）读取 `sdk+0x98e350` 的 `02 00 00 00`，`RequestC2`（`sdk+0x402adc`）再将该值传入 `IRequest` 构造流程，支持将 `RequirementPreset` 判定为 2。

协议资料将 `IGeneralService::CreateRequest` 列为命令 4，参数为 32 位 `RequirementPreset`，返回 `IRequest` 对象；libnx 对应客户端也以一个输出对象调用命令 4：[Switchbrew NIFM 服务](https://switchbrew.org/wiki/Network_Interface_services)、[libnx nifm.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/nifm.c)。

当前 ACNH 实现只接受经 IPC domain 正常路由到 `IGeneralService` 的 type 6、命令 4、恰好 4 字节且无 PID、句柄或缓冲区的请求，并且只接受预置值 2；对象 ID 由 IPC 层解析，不在服务处理器中固定。它为 `nifm:IRequest` 创建两个未触发系统事件。命令 2 每次返回这两个事件各自的新只读句柄；请求形状不符时先失败，不分配读句柄。IRequest 最后一个 IPC 所有者释放时关闭两个写端，已返回的读端仍按运行时引用计数存活。

生产 IPC 分发测试按真实顺序覆盖静态服务转 domain、根对象命令 5 创建 `IGeneralService`、其子对象命令 4 创建 `IRequest`，以及命令 2 的重复读取、独立句柄、无触发、对象关闭后读端仍存活和关闭后 ID 失效。错误输入覆盖未知预置值、PID、消息类型、数据长度、domain token 与描述符；命令 2 错误形状不分配新读句柄。链接生产 `threads.c` 的事件探针检查两种端点关闭顺序、等待/触发/重置和 5,000 次回收循环。这组测试单独验证事件句柄获取与回收。

第二十二次实际启动已取到两枚可读事件，继续至尚未实现的命令11（1字节参数）停止。该次运行只完成句柄获取，未提交、获准或完成联网，也未主动触发 NIFM 事件。

第二十二次启动的 SDK 调用链还确认了命令 11 的参数语义：`main+0x377b6ec` 将 `false` 传给 `SetLocalNetworkMode`；SDK `+0x401014..+0x40101c` 将 `false` 映射为 `ConnectionConfirmationOption::Required`（4），`true` 映射为 `Prohibited`（1）；SDK 代理 `+0xd2bc8` 以单字节调用 `IRequest` 命令 11。协议枚举 1–5 依次为 Prohibited、NotRequired、Preferred、Required、Forced，0 为 Invalid，见 [Switchbrew NIFM 服务](https://switchbrew.org/wiki/Network_Interface_services#ConnectionConfirmationOption)。

ACNH 适配只接受 type 6、domain、token 0、恰好 1 字节且无 PID、句柄或缓冲区的命令 11，并保存有效的 1–5 选项；0 和大于 5 的值明确失败。选项以原子字段保存，避免 IPC 克隆会话共享同一请求对象时发生数据竞争。设置选项不会提交请求、改变网络请求进度、创建句柄或触发事件。

第二十三次启动（`work/acnh/3.0.0/boot-twenty-third.log`）观察到 `IRequest` 命令 4 `Submit`，原 SDK 静态调用链显示，其后立即查询命令 0 `GetRequestState` 和命令 1 `GetResult`；只在状态为 2 时等待。ACNH 离线策略仅在确认选项为 4（Required）时接受无参数的命令 4：同步标记请求已提交、触发首枚事件，并以 `0x8986e`（网络不可用）失败；重复提交同样失败并再次触发首枚事件。命令 0 始终返回状态 1，不伪造请求获准；命令 1 在提交前返回 SDK 已缓存的 `0x26e6e` 哨兵值，提交后返回 `0x8986e`。协议状态定义见 [Switchbrew NIFM 服务](https://switchbrew.org/wiki/Network_Interface_services)，`0x8986e` 的网络不可用含义见 [Switchbrew 错误码](https://switchbrew.org/wiki/Error_codes)。这些结果码是本地离线兼容策略；启动日志没有证明真实 Switch 对应调用会返回相同结果。其他确认选项与三条命令的畸形请求均明确失败；cmd2 仍可重取句柄，既有首事件读句柄会观察到触发，第二事件保持未触发。当前 IPC 不支持通过协议克隆 `IRequest` domain 子对象，因此生产分发测试验证两个请求 owner 的状态互不污染；共享所有者的克隆生命周期由现有通用 owner 测试覆盖，不能据此声称已实测克隆 `IRequest`。未支持的其他子命令仍明确失败；没有实现联网、等待态、已接受态或套接字操作。

第二十四次启动已实际通过命令4、0、1，并读到状态1和失败结果 `0x8986e` 后继续执行，未卡在等待中；随后停止于另一处 `fsp-srv` 服务打开。日志 `work/acnh/3.0.0/boot-twenty-fourth.log`，此结果验证当前游戏路径接受该离线策略，不证明真实 Horizon 精确回包或完整 NIFM 支持。

2026-10-24 设置存档备份时，游戏在 Submit / GetResult 之后调用 `IRequest` 命令 21 `GetAppletInfo`（联网失败后查找系统错误界面）。原 SDK `HandleNetworkRequestErrorResult`（`sdk+0x402a40`）在该命令失败时返回“未解决”，不中止。宿主没有系统网络界面，按同一离线结果失败：提交前 `0x26e6e`，提交后 `0x8986e`。请求形状为 type 6、domain、token 0、4 字节主题、一个 B 描述符；其余形状明确失败。生产 IPC 分发测试覆盖提交前后的结果码和畸形请求。同日实机日志 `work/acnh/3.0.0/boot-first-save.log` 记录 `GetAppletInfo: no system applet offline (0x8986e)`，用户确认设置备份不再中止。这不等于云备份可用。
