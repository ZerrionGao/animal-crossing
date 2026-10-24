# ACNH 家长控制服务依据

`work/acnh/3.0.0/boot-diag.log` 和 `boot-depthkey.log` 只观察到普通 `pctl` 的 `IParentalControlServiceFactory` 命令 1：`type=6`、非 domain、8 字节输入、无缓冲区和句柄；随后观察到 `IParentalControlService` 命令 1018：`type=6`、非 domain、0 字节输入、无缓冲区和句柄。启动时也会尝试打开 `pctl:s`、`pctl:a`、`pctl:r`，这些特权会话上没有观察到业务命令；四个端口都注册，避免 SDK 因打不开端口而中止。

工厂命令 1 是 `CreateServiceWithoutInitialize`。本机 App 进岛后（约第 900 帧、有键盘输入）会再调服务命令 1 `Initialize`：`type=6`、domain、0 字节输入、无缓冲区和句柄。原协议无额外输出，结果 0 即可。未接这条命令时进程以 `IPC command not implemented` 退出，与读档无关。

原协议把命令 1018 定义为 `IsFreeCommunicationAvailable`。这份 SDK 只比较返回值是否为 0，不读输出布尔：0 表示空闲通信可用。登机影片结束后游戏会调用它，接着用账号检查走离线。让命令 1018 中止进程时，影片一播完窗口就关掉。因此 `pctl`、`pctl:s`、`pctl:a`、`pctl:r` 都注册，工厂命令 0/1 创建服务对象，服务命令 1/1001/1018 返回结果 0。这不是已核验的家长控制策略，也没有打开在线功能。工厂命令大于 1、以及未列出的服务命令仍明确失败。

依据：[Parental Control services](https://switchbrew.org/wiki/Parental_Control_services)。生产探针覆盖已观测 factory/service 形状、未知命令、畸形输入、四个端口均可打开，以及 `GAME_ACNH` 关闭时不注册服务。
