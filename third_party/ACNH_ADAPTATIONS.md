# ACNH 本地适配说明

基线和许可见 [来源记录](macnativegames.UPSTREAM.json) 及 `macnativegames/` 内保留的许可。以下为本项目相对固定上游的改动；游戏内容、keys 和生成的二进制不包含在源码中。

- `games/totk/third_party/d2r/{patch_nso.py,dynsym.py}`：支持 ACNH 的 GNU-only 动态符号表，仍保留调用窗口目标检查；不启用 D2R 私有游戏补丁。
- `games/totk/runtime/{loader.c,game_version.h,game_patches.c,swkbd_host.c}`：独立 `GAME_ACNH` 分支，使用经过样本核验的启动入口，关闭其他游戏的固定地址补丁、恢复分支和键盘钩子。
- `games/totk/runtime/{game_application_id.h,services.c,fs_host.c}`：程序与存档检查共用 ACNH application ID，避免落入 TOTK 默认值。
- `games/totk/runtime/input.c`：保留客体输入映射，ACNH 不触发上游 1–5 数字键的宿主画面特效切换。`GetNpadJoyAssignment` 对玩家、Other 和 Handheld 返回 Dual，避免空的 Npad 共享内存槽触发原 SDK 中止。
- `games/totk/runtime/{applet_host.c,frontend.m,runtime.h}`：ACNH 的退出区间计数、闲置检测扩展状态和消息队列适配；当前原型只通知退出并保留窗口，尚未实现正常退出链。`GetPseudoDeviceId` 在应用接口对象为空时改为读取持久的 16 字节本地标识，不创建 AM 对象，也不报告真实主机设备号。
- `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c`：ACNH 图形配置分支；使用独立编译宏、对象目录与尊重客体显示间隔的宿主节拍。ACNH 与旷野之息一样不调用 `nvnDeviceSetDepthMode`，因此保留 NVN 缺省近平面 -w。
- `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`：交换链通道没有深度附件。近平面仍是 -w 时，拷到交换链的全屏绘制 clip z 为 -1，按 Metal 的 0..w 会被整块裁掉；这次绘制改为夹到近平面。游戏若把近平面改成 0，则不再夹。
- `games/totk/runtime/{ipc.c,ipc.h,nifm_offline.c}`：按 ACNH 实际启动请求逐步补齐 IPC 对象协议和离线网络接口；ACNH 按已核验的预置值创建网络请求对象并返回两枚可读事件，写端随最后对象所有者释放；未支持的请求子命令只记录结构后明确失败；未知服务或命令继续明确失败。

- `games/totk/runtime/{romfs.c,romfs_directory.c,romfs_directory.h,fs_host.c}`：ACNH 从已校验镜像构建只读目录索引，逐项核对文件路径、大小和偏移，支持空目录、过滤、分页与关闭；初始化完整完成后发布。

- `games/totk/runtime/shared_font.c`：ACNH 注册真实加载请求与受限 en-US 字体列表，原文件校验后只解码内存副本；修复首次地址获取的求值顺序问题。字体选择顺序仍是明确标注的兼容实现，其他语言暂拒绝。

- `games/totk/runtime/friends_offline.c`：仅按实际观测请求创建好友服务子对象；未知好友命令记录形状后明确失败，尚未提供完整好友功能。

- `games/totk/runtime/bcat_offline.c`：按真实请求创建内容分发服务对象；ACNH 无参 delivery cache 挂载叶子函数返回本地缺失策略 `0x47a`，由原游戏走无条目分支；未知子命令明确失败，不伪造下发缓存或同步成功。`services.c`/`runtime.h` 的符号委托仅对 ACNH 生效。

- `games/totk/runtime/settings_service.c`：复用既有设置服务，ACNH 只启用地区查询并沿用 USA1 诊断值；BOTW 语言设置行为保留。

- `games/totk/runtime/time_host.c`：ACNH 采用有锁引用计数和墙钟/连续时钟锚点，系统时区名称与可复制 TZif 规则支持正向日历转换；逆转换明确停止，完整时间服务尚未验收。

- `games/totk/runtime/ngc_service.c`：ACNH 词语过滤服务目前只允许创建诊断会话；任何实际命令仅记录请求结构后明确失败，不输出文本或伪造过滤结果。

- `games/totk/runtime/fs_host.c`：ACNH 设备存档独立于 UID 存档，按已核验 NACP 持久化初始容量，并拒绝损坏元数据或当前可见的链接路径；新增恢复、扩容与提交重载仍明确失败，单进程测试目录不代表完整存档事务或并发沙箱。

- `games/totk/runtime/account.c`：ACNH 的 ListAllUsers 复用持久本地 UID；max<=0 仍返回总数但不写数组，避免落回未初始化的 SDK 账号服务。其他账号能力仍逐项验证。

- `games/totk/runtime/olsc_offline.c`：ACNH 云存档服务仅实现本地离线会话初始化和持久本地 UID 的“未启用”查询，其他命令明确失败；日志不输出正文，不报告远端备份/恢复成功。

- `games/totk/runtime/loader.c`：ACNH 非启动版额外仅允许颜色、深度／模板清除两个独立 GPU 探针；正式启动版仍只允许装载探针或经验证的游戏启动。

- `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`：ACNH 首次进入绘制中深度／模板清除时记录有界参数诊断，默认保护门禁保持开启，ACNH 独立探针补纯深度清除后同编码器连续绘制与正负控；覆盖边界见 [GPU 清除依据](../docs/implementation/acnh-gpu-clear-evidence.md)。

- `games/totk/runtime/nifm_offline.c`：ACNH 请求对象的命令11仅原子保存连接确认枚举1..5，命令2返回独立可读事件；命令4按本地离线策略同步失败，并提供命令0/1的非可用状态和失败结果；仅首事件通知，不报告网络请求获准。

ACNH 的构建、资源校验、装载表生成与测试入口位于仓库自己的 `games/acnh/`、`build.py` 和 `tests/`，未覆盖上游的其他游戏构建入口。当前运行验收状态见[实施记录](../docs/implementation/acnh-macos-plan.md)。
