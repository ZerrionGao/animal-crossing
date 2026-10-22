# ACNH 内容分发服务依据

第九次启动停在 `bcat:u` 的 SM 打开处。主程序 `main+0x3b0a4a4` 调用 `nn::bcat::Initialize`；原 SDK `sdk+0x265310` 经全局初始化 `sdk+0x26b1c0`，先打开服务，再调用 `CreateBcatService`。当前测试配置没有下发缓存。

第十次启动实际捕获 domain 根命令 0：type 6、object ID 1、token 0，8 字节参数、send-PID 标识，无缓冲区或复制/移动句柄。原 SDK 的 `CreateBcatService` 在调用处传零值 PID 占位；HIPC PID descriptor 是另一字段，当前宿主 IPC 不模拟内核 PID 注入，不应把二者相等作为校验或身份凭据。

依据[BCAT 协议](https://switchbrew.org/wiki/BCAT_services)，命令 0 只创建 `IBcatService`；命令成功并不表示已联网或已有内容。初始化中没有必经命令 1 的静态证据，因此当前只接实际观察到的命令 0，子命令继续记录请求形状并明确失败。

原 SDK 的 `RequestSyncDeliveryCache`（`sdk+0x2656c0`）在 IPC 前调用 `nn::nifm::GetClientId` 和 `nn::nifm::IsAnyInternetRequestAccepted` 做联网许可检查，失败分支 `sdk+0x265778` 返回 `0x107a`。该值与[固定版本 BCAT ResultCode](https://git.axenov.dev/Museum/ryujinx/raw/commit/264438ff19898bcb8f8fc16dc9243bf9f4eba072/src/Ryujinx.HLE/HOS/Services/Bcat/ResultCode.cs)中的 module 122、description 8 `InternetRequestDenied` 一致。优先保留原 SDK 的这条失败路径；若后续实际调用同步子命令，再按准确离线错误语义处理，不能采用兼容项目中的假完成成功桩。

尚未验收：内容同步、缓存目录/文件读、进度事件和更新内容。空缓存与游戏原始 RomFS 是两种不同资源，不能互相替代。

第十一次启动已实际完成命令 0 并继续到地区设置查询；尚未观察 BCAT 子命令。对象创建、域路由和关闭后失效的生产 IPC 测试及独立审查均通过。

第二十四次启动进一步定位到 `main+0x3b0a0e0` 调用无参 `nn::bcat::MountDeliveryCacheStorage()`（原 SDK `sdk+0x265370`）。同次 `fsp-srv` 访问日志只是 SDK 初始化期间的文件系统访问日志设置，不表示游戏需要 mount 文件系统服务，也不构成下发 BCAT 缓存的证据。原游戏分支只对 `0x67a` 执行锁重试；`0x47a`（module 122、description 2，NotFound）直接进入调用方 `main+0x1b813d8` 的无条目清理路径，不读取目录或文件。因此 ACNH 只为该无参 leaf 提供本地 absence policy：若没有本地 delivery-cache 支持，返回 `0x47a` 并仅首次记录一条“本地缓存不可用”日志；不创建 mount、目录或文件，也不报告成功。这个宿主策略采用游戏已有 NotFound 分支能处理的结果值，但不是经真实 Horizon 验证的回包。没有实现缓存同步、内容枚举或文件读取。

第二十五次启动已实际命中挂载叶子函数并返回 `0x47a`，继续至第150帧后在另一处 SDK 空指针读取崩溃。日志 `work/acnh/3.0.0/boot-twenty-fifth.log`；它验证游戏接受当前缺缓存分支，不等于已支持缓存挂载、读取或同步。
