# ACNH OLSC 离线接入依据

第十六次运行在 `olsc:u` 连接处停止；第十七次诊断确认首个业务请求是 cmd0 Initialize：type6、domain 根对象1、token0、数据8字节、发送PID标识、无缓冲区及copy/move句柄。诊断不输出正文或PID值，日志 `work/acnh/3.0.0/boot-seventeenth.log`。

[OLSC 协议](https://switchbrew.org/wiki/OLSC_services) 将该服务定义为在线存档云备份服务。原游戏构包进一步核实：cmd0 的8字节是传入零值的占位数据；随后的 cmd13 GetSaveDataBackupSetting 输入16字节 Uid，不发送PID或缓冲区，成功输出1字节设置值。游戏只在该值为1时调用 cmd14 将设置关闭；本路径没有备份上传或恢复调用。

本项目当前只实现本地离线会话：cmd0 严格核验零参数并初始化当前会话，cmd13 必须在初始化后且匹配持久本地 UID，返回1字节0，表示本地云备份未启用。重复的合法初始化在本地幂等处理，并非声明官方服务的重复调用语义。未知 UID、请求形状及其他命令明确失败。

该结果不查询或代表真实 Nintendo 账号、许可或云端存档；没有配置真实云备份，也不提供上传、下载、恢复、删除或设置写入成功。不得将这两个离线入口描述为云存档功能已经可用。

独立复核已通过。共享会话克隆的初始化状态使用原子读写，最后引用关闭才释放；4 项生产 IPC 定向测试覆盖初始化顺序、重开会话、严格请求形状、本地 UID、单字节响应和失败隐私。完整测试运行 70 项，69 项通过、1 项默认跳过，日志 `work/acnh/tests-olsc-offline.log`；第十八次运行已在游戏进程中实际通过 cmd0 和 cmd13；后者确认为 type6、domain 根对象1、token0、16字节 Uid、无PID、缓冲区或句柄。随后继续执行更多图形调用，在独立的深度／模板清除 GPU 保护门禁处停止，日志 `work/acnh/3.0.0/boot-eighteenth.log`。
