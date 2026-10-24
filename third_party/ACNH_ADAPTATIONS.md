# ACNH 本地适配说明

基线和许可见 [来源记录](macnativegames.UPSTREAM.json) 及 `macnativegames/` 内保留的许可。以下为本项目相对固定上游的改动；游戏内容、keys 和生成的二进制不包含在源码中。

- `games/totk/third_party/d2r/{patch_nso.py,dynsym.py}`：支持 ACNH 的 GNU-only 动态符号表，仍保留调用窗口目标检查；不启用 D2R 私有游戏补丁。
- `games/totk/runtime/{loader.c,game_version.h,game_patches.c,swkbd_host.c}`：独立 `GAME_ACNH` 分支，使用经过样本核验的启动入口，关闭其他游戏的固定地址补丁、恢复分支和键盘钩子。
- `games/totk/runtime/{game_application_id.h,services.c,fs_host.c}`：程序与存档检查共用 ACNH application ID，避免落入 TOTK 默认值。
- `games/totk/runtime/input.c`：保留客体输入映射，ACNH 不触发上游 1–5 数字键的宿主画面特效切换。数字键 1 连按空格快进对话，登机影片解码器配好后停止。宿主没有 hid 服务和 Npad 共享内存，原 SDK 的 hid detail 层出错后公开接口会中止，因此 ACNH 由宿主接管以下接口：`GetNpadJoyAssignment` 返回各编号记录的分配模式（初始为 Dual）；`SetNpadJoyAssignmentModeDual`/`Single` 和 `MergeSingleJoyAsDualJoy` 只更新该记录；`SwapNpadAssignment` 不移动唯一的手柄；`GetNpadControllerColor` 返回 NoController（`0x4b8ca`），游戏会改用默认颜色；`GetPlayerLedPattern` 使用 Switch 的玩家灯表；`GetSupportedNpadStyleSet` 读回游戏设置的样式集合。白狗弹完吉他后的闪退就是 `GetNpadControllerColor` 触发的 hid `0x58cca`。
- `games/totk/runtime/{applet_host.c,frontend.m,runtime.h}`：ACNH 的退出区间计数、闲置检测扩展状态和消息队列适配；关窗和 Quit 发送退出消息 4，并按 Switch 关闭软件的语义结束进程：游戏在退出请求处理区内就等它离开，之后等宿主存档空闲（最多 15 秒）再 `_exit(0)`，不运行客体收尾；决定结束后不再允许进入处理区。默认窗口是 1280×720 点，游戏画面仍是 1920×1080。右上角只显示帧率，不显示王国之泪的快捷键说明。`GetPseudoDeviceId` 在应用接口对象为空时改为读取持久的 16 字节本地标识，不创建 AM 对象，也不报告真实主机设备号。
- `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c`：ACNH 图形配置分支；使用独立编译宏、对象目录与尊重客体显示间隔的宿主节拍。ACNH 与旷野之息一样不调用 `nvnDeviceSetDepthMode`，因此保留 NVN 缺省近平面 -w。确认肤色时的 `nvnBufferFlushMappedRange` 若按缓存行超出缓冲，只刷新仍在缓冲和内存池内的区间。
- `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`：交换链通道没有深度附件。近平面仍是 -w 时，拷到交换链的全屏绘制 clip z 为 -1，按 Metal 的 0..w 会被整块裁掉；这次绘制改为夹到近平面。游戏若把近平面改成 0，则不再夹。
- `games/totk/runtime/{ipc.c,ipc.h,nifm_offline.c}`：按 ACNH 实际启动请求逐步补齐 IPC 对象协议和离线网络接口；ACNH 按已核验的预置值创建网络请求对象并返回两枚可读事件，写端随最后对象所有者释放；未支持的请求子命令只记录结构后明确失败；未知服务或命令继续明确失败。

- `games/totk/runtime/{romfs.c,romfs_directory.c,romfs_directory.h,fs_host.c}`：ACNH 从已校验镜像构建只读目录索引，逐项核对文件路径、大小和偏移，支持空目录、过滤、分页与关闭；初始化完整完成后发布。

- `games/totk/runtime/shared_font.c`：ACNH 注册真实加载请求与受限字体列表，原文件校验后只解码内存副本；修复首次地址获取的求值顺序问题。en-US 仍是已观察到的 0..5 顺序；zh-Hans 使用公开的简体优先顺序，不是从原服务抓到的优先级。其他语言暂拒绝。

- `games/totk/runtime/friends_offline.c`：仅按实际观测请求创建好友服务子对象；未知好友命令记录形状后明确失败。游戏直接调用的好友名单、屏蔽名单和在线会话声明按离线处理：名单为空，需要等待的异步查询直接失败。尚未提供完整好友功能。

- `games/totk/runtime/bcat_offline.c`：按真实请求创建内容分发服务对象；ACNH 无参 delivery cache 挂载叶子函数返回本地缺失策略 `0x47a`，由原游戏走无条目分支；未知子命令明确失败，不伪造下发缓存或同步成功。`services.c`/`runtime.h` 的符号委托仅对 ACNH 生效。

- `games/totk/runtime/settings_service.c`：复用既有设置服务。ACNH 地区查询沿用 USA1；确认名字时到达的语言查询返回与 `GetDesiredLanguage` 相同的代码。未设置 `TOTK_LANG` 时默认是 zh-Hans。其他设置命令仍明确失败。BOTW 语言设置行为保留。

- `games/totk/runtime/time_host.c`：ACNH 采用有锁引用计数和墙钟/连续时钟锚点。系统时区名称与可复制 TZif 规则支持正向日历转换，也支持把本地日历时间换回时间戳；夏令时空档返回 0 个结果。完整时间服务尚未验收。

- `games/totk/runtime/{ngc_service.c,ngc_aho.c}`：确认名字时的 Mask（cmd 2）只接受已观测的请求形状；从本机 NgWord2 加载 `not_b`/`b1` 的 Aho-Corasick 表（格式按 Ryujinx NGC / MIT 核对），命中写成 `*`，返回替换次数。没有 similar-form 与分隔短语（b2）。词库缺失时仍原文照写。日志只记次数和字节数，不记录名字。其他命令仍明确失败。

- `games/totk/runtime/pctl_offline.c`：登机影片结束后的 `IsFreeCommunicationAvailable`（服务命令 1018）返回结果 0，SDK 据此视为可用，随后账号检查走离线。`pctl`、`pctl:s`、`pctl:a`、`pctl:r` 都注册；工厂命令 0/1 创建对象，更大的工厂命令明确失败。服务命令只放行已观测的 1（`Initialize`）、1001（`CheckFreeCommunicationPermission`）和 1018，且要求无参数、无缓冲区、无句柄；其余明确失败。这不是家长控制策略，也不打开在线功能。

- `games/totk/runtime/audout_host.c`：登机影片打开 `audout:u`。List/Open（含 Auto）返回 `AudioTvOutput`、48000 Hz 立体声 PcmInt16；IAudioOut 接受追加、开始、停止、缓冲事件和已释放指针。PCM 经单独的 AudioQueue 播放，打不开队列时仍按缓冲时长释放，避免影片停住。未知命令仍明确失败。

- `games/totk/runtime/fs_host.c`：ACNH 设备存档独立于 UID 存档，按已核验 NACP 持久化初始容量，并拒绝损坏元数据或当前可见的链接路径；宿主接管游戏导入的 `CleanDirectoryRecursively`、`RenameDirectory`、`GetDirectoryEntryCount`（原 SDK 挂载表为空会 Abort；这三项仅 ACNH 构建注册），`CommitOption` 只接受 0 与 ClearRestoreFlag(1)，后者在没有暂存改动时也会原子清除恢复标志；多挂载批量提交与 SetRestoreFlag 仍明确失败；提交、开始暂存或文件操作持有设备存档锁期间，退出判定一律视为存档忙；单进程测试目录不代表完整存档事务或并发沙箱。

- `games/totk/runtime/account.c`：ACNH 的 ListAllUsers 复用持久本地 UID；max<=0 仍返回总数但不写数组，避免落回未初始化的 SDK 账号服务。与原 SDK 一样写满 max 个槽位，空槽为无效 UID（全 0）：读档时游戏用未清零的数组接收，再逐个非零项调用 olsc。ListOpenUsers 只返回当前已打开的本地用户，空槽同样清零。网络账号查询不再把本地句柄里的序号当成对象指针，统一按离线不可用返回。其他账号能力仍逐项验证。

- `games/totk/runtime/olsc_offline.c`：ACNH 云存档服务仅实现本地离线会话初始化，以及持久本地 UID 的备份开关设置（命令 14，读档时游戏会设为关闭）和读取（命令 13），开关只在本进程内记录，其他命令明确失败；日志不输出正文，不报告远端备份/恢复成功。

- `games/totk/runtime/loader.c`：ACNH 非启动版额外仅允许颜色、深度／模板清除两个独立 GPU 探针；正式启动版仍只允许装载探针或经验证的游戏启动。

- `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`：ACNH 首次进入绘制中深度／模板清除时记录有界参数诊断，默认保护门禁保持开启，ACNH 独立探针补纯深度清除后同编码器连续绘制与正负控；覆盖边界见 [GPU 清除依据](../docs/implementation/acnh-gpu-clear-evidence.md)。

- 着色器（`nvn_mtl.m`、`totk_shader_rt.m`、`totk_shader_seed.h`、`games/acnh/runtime/acnh_msl_normalize.c`）：ACNH 现编 MSL 前先经正规化器把纹理数组采样的层号夹到数组大小以内（嵌套采样等不支持的形状按失败处理），并用安全数学与 `preserveInvariance` 编译；正规化失败或着色器缺失时跳过该绘制，不中止进程。ACNH 不使用按原始 MSL 哈希命名的 `shader-libs` 预编译库，也不接受 `TOTK_FAST_MATH`。运行时变体记录近平面模式（`depth_minus_w`）；种子着色器需带匹配的 `shader_policy`，否则拒收并清理旧 metallib。`fps_overlay.c` 在 ACNH 下默认只显示帧率。`ios/tools/{compile_seed.py,build_ipa.py}` 新增正规化器参数，但 iOS 版 ACNH 的着色器链路尚未打通。

- `games/totk/runtime/nifm_offline.c`：ACNH 请求对象的命令11仅原子保存连接确认枚举1..5，命令2返回独立可读事件；命令4按本地离线策略同步失败，并提供命令0/1的非可用状态和失败结果；命令21（GetAppletInfo，联网失败后查找系统错误界面）按同一离线结果失败，原 SDK 据此判定“未解决”而不中止；仅首事件通知，不报告网络请求获准。

ACNH 的构建、资源校验、装载表生成与测试入口位于仓库自己的 `games/acnh/`、`build.py` 和 `tests/`，未覆盖上游的其他游戏构建入口。Mac App 原型由 `games/acnh/app/launcher.m` 与 `scripts/package_macos.py` 生成：打开 GPU 提交和已验证的绘制中清除，节拍与 `make boot` 相同，存档写到 `~/Library/Application Support/MacNativeGames/ACNH`，不含 keys，仅 Ad-hoc 签名。当前运行验收状态见[实施记录](../docs/implementation/acnh-macos-plan.md)。触达但仍非完整 Horizon/NVN 的调用见 [B2 清单](../docs/implementation/acnh-b2-unimplemented-inventory.md)。
