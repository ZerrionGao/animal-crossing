# ACNH 好友服务启动诊断

第七次启动在 `friend:u` 的 SM 打开处停止。主程序 `main+0x3af7098` 经 PLT 调用 `_ZN2nn7friends10InitializeEv`；`main+0x3af709c` 是返回地址。原 SDK 链为 `friends::Initialize`（`sdk+0x3afe00`）→内部初始化（`sdk+0x3b3670`）→服务打开（调用点 `sdk+0x3b3dec`）。

这段日志只证明游戏要求好友系统初始化，没有证明它已经查询好友列表、订阅通知或要求联网。不能据此把一组假成功函数注册为“好友支持”。

下一步采用仅在 ACNH 编入的诊断服务：允许 SM 创建会话，记录命令及缓冲区形状，遇到未实现命令明确失败。普通会话/domain 的生命周期仍由既有 IPC 层处理。取得实际请求后，再依据[好友服务协议](https://switchbrew.org/wiki/Friend_services)和原 SDK 精确 ABI 逐步实现本地离线范围；需要事件时必须同时核验句柄所有权、复制与关闭。

在真实游戏查询前，空好友列表、通知事件与在线状态都不能计为已验证功能。

第八次启动已实际打开服务并观察到 domain 根对象上的命令 0：type 6、object ID 1、数据区 0 字节，无缓冲区和句柄参数。诊断入口按设计明确停止。下一次仅补 `IServiceCreator` 命令 0 的 `IFriendService` 对象创建；未知子命令继续失败，不把这一阶段算作好友功能完成。
