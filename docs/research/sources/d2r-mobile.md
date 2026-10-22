# d2r-mobile 源码研究：对 ACNH 原生兼容移植的参考价值

> 本篇记录单个参考项目的源码研究，文中“未测试/未知”限定于这一分项。整轮研究随后完成了 ACNH 静态解析和 shader 编译采样，最新结论见[主报告](../acnh-port-feasibility.md)及[二进制画像](../acnh-binary-profile.md)。

研究对象：`jhaizhou-ops/d2r-mobile`，固定 commit [`53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53`](https://github.com/jhaizhou-ops/d2r-mobile/tree/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53)。以下路径行号均对应该 commit。CodeGraph `status` 显示 `Not initialized`；遵照要求没有初始化或生成索引，改用仓库 README、相关实现和构建脚本进行定点检查。只读研究；未运行游戏、未安装依赖、未修改源码，也未读取密钥。

## 结论先行

D2R Mobile 的核心技术选择是“复用 Switch ARM64 游戏代码，重写其宿主操作环境”，不是逐帧模拟主机 CPU。该模式成立的关键前提是游戏二进制与 Apple 芯片同为 AArch64，并且只需处理有限的保留寄存器、线程区域、系统调用和计时器差异。它的通用价值在于改写器、宿主服务层、GPU API 后端、预构建代码签名、输入适配、生命周期和验证体系的分层方法；实际改写规则、NVN 函数实现、D2R 内存地址和触屏交互都绑定到特定游戏/版本，不能直接搬到 ACNH。

对 ACNH 的主要未知项是：目标版本是否为 ARM64 NSO/同类可执行模块、实际用到哪些 Horizon SVC 与系统服务、图形接口/着色器格式和资源布局、运行时是否接受 iOS 的内存/签名限制，以及触屏操作是否能映射到原游戏行为。不能仅凭“同为 Switch 游戏”推定 D2R 的改写规则或 NVN 实现适用。

## 1. 构建、取材与执行模型

D2R 以 `main`、`sdk`、`subsdk0` 三个 NSO 模块为输入。仓库明确说明：与 iPhone 同指令集，因此大量游戏机器码可以原样在 Apple 芯片执行；主要工作是替换 Horizon 依赖。[`dm/README.zh-CN.md` 第 18–38 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L18-L38)

`setup.sh` / `dm/tools/extract_game.sh` 面向用户自有游戏文件，提取 ExeFS 与 RomFS；`patch_nso.py` 离线处理模块，工具说明列出输入、输出映像、布局、门表等产物。[`docs/GAME_FILES.zh-CN.md` 第 3–16、26–50 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/docs/GAME_FILES.zh-CN.md#L3-L16) [`dm/tools/README.zh-CN.md`（工具索引）](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/README.zh-CN.md) `dm/host/Makefile` 编译宿主与图形层；iOS 侧 `run.sh build` 生成 Xcode 项目、嵌入模块和打包运行时资源。[`dm/gfx/README.zh-CN.md` 第 104–113 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/README.zh-CN.md#L104-L113) [`dm/ios/README.md` 第 18–26 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L18-L26)

iOS 不能像 Mac 宿主那样运行时写入代码页再执行。项目因此在构建时把改写后的 `main`、`sdk` 以汇编段嵌入签名 App；iOS loader 只映射并检查这些签名段，不复制或修改代码页权限。此处“静态 Mach-O”应准确理解为：**NSO 代码映像作为 App 主可执行文件中的签名代码段随构建嵌入**，不是把原游戏静态链接成一个普通 Mach-O 目标文件，也不是运行时 JIT。[`dm/ios/README.md` 第 18–26 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L18-L26) 生成器为 [`dm/ios/tools/gen_embed.py`](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/tools/gen_embed.py)；host 源文件/框架则由 `gen_build.py` 从实际展开的 Makefile 编译命令生成 Xcode 配置，减少两套构建清单漂移。[`dm/ios/README.md` 第 20–26 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L20-L26)

## 2. ARM64、x18/TLS、SVC 与装载器

改写器处理四类跨内核差异：`x18` 读写映射到每线程槽；`tpidr_el0` / `tpidrro_el0` 读写/读取改映射到宿主维护的线程槽；`svc #n` 转进宿主门；读取 `cntpct_el0` 也由宿主门提供计时结果。对应解释见 [`dm/README.zh-CN.md` 第 22–36 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L22-L36)，实现器头注释见 [`dm/tools/patch_nso.py` 第 1–21 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/patch_nso.py#L1-L21)。实现对 x18 指令解码后替换寄存器字段并进行反汇编复核（第 304–316 行）；生成的改写块读取 `tpidrro_el0` 作为线程槽基址、执行替代指令并回写槽（第 329–367 行）。SVC 与系统计数器指令被扫描并登记为门调用，宿主以门返回位置匹配 `gates.tsv`（第 411–431、528–546、592–603 行）。[`patch_nso.py` 改写逻辑](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/patch_nso.py#L304-L367)

改写有版本边界和检查机制：个别 CUTS 按 NSO build ID 登记，先断言原字节一致，否则中止；另外通过生成物记录 sites/cuts/gates。[CUTS 表定义](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/patch_nso.py#L46-L79)、[写出产物表与构建器指纹](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/patch_nso.py#L844-L877)；不能把 D2R 的站点偏移、CUTS、OBS 或私有 svc 号用于 ACNH。 [`dm/README.zh-CN.md` 第 31–38 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L31-L38)

Loader 把改写模块映射、注册线程槽和门、处理跨模块重定位并设置段权限；每个游戏线程是 pthread，SDK `nn::os` 上层线程/锁/事件逻辑继续运行，底下 SVC 由宿主模拟。[`dm/README.zh-CN.md` 第 48–66 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L48-L66) `hle_thread.c` 明示每客体线程一个宿主 pthread、跑 SDK 给的客体栈，覆盖线程、仲裁锁、条件变量、地址等待及事件等 SVC；等待机制以一把全局锁和每线程条件变量实现，注释称先求正确、后量性能。[`dm/host/hle_thread.c` 第 1–20、39–66 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/host/hle_thread.c#L1-L20)

## 3. Horizon 服务、线程、文件、时间与音频

宿主把依赖拆为三类：内核 SVC、可直通的 libc、SDK 系统服务接口替身。系统服务不通过原 Switch IPC 服务进程，而是在 SDK 导入符号处按名称接管；`HLE` 补缺失符号、`HLE_FORCE` 强制覆盖 SDK 实现且要求写明理由。未实现导入会落到可诊断桩；联网、好友、商城被明确降级为离线不可用。[`dm/README.zh-CN.md` 第 58–68、148–171 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L58-L68)

几个服务的层次可作移植样板，但需以 ACNH 导入表和运行行为重新界定：

- 文件系统 `hle_fs.c` 将游戏 mount 名映射到宿主目录；RomFS overlay 先查差分文件，未命中回落原游戏资源，且存档事务有故障注入回归。[`dm/README.zh-CN.md` 第 159–170 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L159-L170) [`dm/ios/README.md` 第 88–97 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L88-L97)
- 时间服务以宿主系统时钟满足离线单机；`DM_FIXED_TIME` 只用于确定性核对，并针对特定建角调用点固定种子。该例也显示：即使 API 相同，个别调用点语义依赖游戏分析。[`dm/host/hle_time.c` 第 1–23、30–54 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/host/hle_time.c#L1-L23)
- 音频服务以 CoreAudio AudioUnit 输出 PCM，队列由播放端消费并将已释放 buffer 通过事件通知游戏；静音测试 sink 按实时节奏消费缓冲以保留时序。iOS 默认真声，Mac 命令行缺省静音。[`dm/host/hle_audio.c` 第 1–48 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/host/hle_audio.c#L1-L48)
- 线程、同步、文件、时间、音频等是平台替身；D2R 的额外模式（秘境、仓库、物品便利功能等）另在 `dm_*.c`，且通过游戏原映像偏移接入，明确属于版本定制。[`dm/host/README.zh-CN.md` 第 5–12、28–36、100–105 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/host/README.zh-CN.md#L5-L12)

## 4. NVN → Metal 与离线着色器

D2R 通过 `nvnBootstrapLoader` 动态取得图形函数，宿主接管该入口，记录 NVN 命令，再在提交时回放成 Metal。文档记载游戏实际调用点涉及 216 个函数（203 个实现、13 个空实现）；其余查询地址对应报红桩。这种调用清单、显式缺口和语义实现方式可借鉴。[`dm/gfx/README.zh-CN.md` 第 11–26 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/README.zh-CN.md#L11-L26)

NVN 内存池复用游戏分配内存建零拷贝 MTLBuffer，GPU/CPU 地址一致，因此要求统一内存；块线性 Tegra 贴图上传前解排布；对象同步、事件和计数器等对外可见完成状态等 Metal 命令真正完成后再发布。渲染管线哈希缓存并在启动后台预热。[`dm/gfx/README.zh-CN.md` 第 18–28 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/README.zh-CN.md#L18-L28) 对象/命令映射的底层约定见 [`dm/gfx/nvn_metal_mapping.zh-CN.md`](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/nvn_metal_mapping.zh-CN.md)。

着色器离线管线为 Maxwell 指令 → Ryujinx 翻 SPIR-V → SPIRV-Cross 转 MSL → Apple Metal 编译器生成 metallib；若构建主机无 Metal 编译器，则将 MSL 交手机编译，系统缓存结果。资源反射、翻译输入事实、链版本哈希和预热清单一起版本控制/校验。[`dm/gfx/README.zh-CN.md` 第 30–57 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/README.zh-CN.md#L30-L57)

对 ACNH 的可借鉴处是“先建立目标图形 API 的完整调用与资源语义盘点，再做最小后端，编译离线 shader、缓存管线并以画面差分回归”。NVN→Metal 代码是否复用，取决于 ACNH 确实使用兼容 NVN 行为、shader 可被相同 Maxwell 翻译链处理、并满足地址/资源约定；目前这些都是未知，不能按 D2R 的实现数或支持清单估算工程量。项目自身也记录了未支持调用与已知空操作，说明声明函数表不能替代真实调用覆盖证据。[`dm/gfx/nvn_metal_mapping.zh-CN.md`](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/nvn_metal_mapping.zh-CN.md)

## 5. D2R 专属 Hook / Patch 与可分层复用

`patch_nso.py` 同时容纳通用平台改写（寄存器/TLS、SVC、计时）和严格限定 build ID 的 CUTS、OBS、游戏功能门。例如把 D2R 内部函数调用替换为宿主私有 SVC，处理掉落音效、出生点、秘境出口、佣兵装备等，规则注释带具体 `main+offset` 与原因。[`dm/tools/patch_nso.py` 第 46–80、133–150、663–764 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/tools/patch_nso.py#L46-L80) [`dm/host/README.zh-CN.md` 第 28–105 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/host/README.zh-CN.md#L28-L105)

建议将它理解为“同一个构建工具中并列放了通用改写和产品玩法 Hook”，而不是 D2R 私有补丁可移植。ACNH 适配应先单独抽象一个与游戏无关的 ARM64/Horizon 基础层，另外建立目标版本专属的导入映射、游戏内 Hook、UI/保存点和证据清单；所有位置改动要有版本标识、原始指令断言、改写前后差分与回滚路径。D2R 自己也有三种规则产物，验证规则域可去除私有门，说明基础兼容层与私有功能有意识地区分。[`dm/README.zh-CN.md` 第 40–46 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/README.zh-CN.md#L40-L46)

## 6. iOS 输入、生命周期、存档与内存

输入链分为 UIKit 采集、纯 C 手势状态机、host 输入接口发布。状态机区分摇杆、技能、腰带、UI，识别点击/双击/长按/拖拽，输出原游戏能识别的触屏/手柄输入。不同原版 UI 场景的处理由游戏自身 UI 消息驱动，而不是根据画面猜测。[`dm/ios/README.md` 第 64–86 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L64-L86) [`dm/ios/Sources/dm_touch_controls.c` 第 1–40 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/Sources/dm_touch_controls.c#L1-L40)

后台行为是显式协调而非简单暂停：失活时取消触摸/手柄状态并请求下一拍存档；入后台时暂停音频、冲刷云同步，局内最多等待 2.5 秒看到存档提交并静默丢帧，随后冻结客体时钟并等待 GPU 在途任务完成；回前台恢复 GPU、时钟与音频。其动机和处理见 [`front_ios.m` 第 1085–1104、1143–1185 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/Sources/front_ios.m#L1085-L1104)。实际存档事务、文件系统安全提交的细节应继续查看 `dm/host/hle_fs.c`，这里不把后台等待机制误称为完整存档实现。[`front_ios.m` 后台保存流程](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/Sources/front_ios.m#L1143-L1185)

启动还处理 iOS 的文件描述符默认上限、异步预加载、64 MB 游戏线程栈、资源完整性校验和缺少资源时的原生界面；App manifest 申请持续运行/扩展虚拟地址/增大内存限制等能力。[`dm/ios/README.md` 第 30–60 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L30-L60) [`dm/ios/project.yml`](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/project.yml) 这不是“内存需求已解决”的通用证据：项目设备表记录 4 GB 设备仍可能被系统终止，而较新设备数据只代表 D2R 的运行集。[`README.zh-CN.md` 设备表](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/README.zh-CN.md#L90-L115)

## 7. 性能与验证

项目把性能归因工具放在仓库内：图形层有逐帧调用计时、慢帧拆分、GPU pass 计时、通道附件读写量；iOS 工具有启动分段、设备 GPU 时间、热状态/帧时间与 Instruments 记录脚本。[^1] `dm/ios/README.md` 还列有设备上的启动、热量、GPU 和采样工具；其公开首页的 FPS、功耗数字属于项目自己的实测声明，不是本次研究复测结果。[`dm/gfx/README.zh-CN.md` 第 136–149 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/gfx/README.zh-CN.md#L136-L149) [`dm/ios/README.md` 第 271–283 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L271-L283)

验证体系包含改写前后等价检查、存档对拍、图像门禁、服务/图形单测、触屏脚本以及 Mac 和真机 smoke；真机验收仍需真实设备和自备数据。研究任务未运行任何测试或游戏，因此本报告只作源码结构观察，不声称构建成功或设备运行复现。[`dm/verify/README.zh-CN.md`](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/verify/README.zh-CN.md) [`dm/ios/README.md` 第 196–259 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/ios/README.md#L196-L259)

[^1]: 这里对工具用途的结论来自文档描述；本次没有执行工具，也未独立核验 README 中记录的设备测量。

## 8. 许可证、游戏内容与第三方代码

仓库根部软件许可证是 MIT；需在分发的完整或实质部分保留版权与许可声明。[`LICENSE` 第 1–21 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/LICENSE#L1-L21) 第三方组件分别保留自己的许可证：bcdec（MIT/Unlicense）、FLIP（BSD-3-Clause）、libdeflate（MIT）、Ryujinx shader 部分（MIT），另有 NVN 声明头 MIT、deko3d 排布算法 zlib。[`dm/third_party/README.zh-CN.md` 第 5–17 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/dm/third_party/README.zh-CN.md#L5-L17) [`docs/DEPENDENCIES.zh-CN.md` 第 55–67 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/docs/DEPENDENCIES.zh-CN.md#L55-L67)

项目明确不分发游戏程序、美术、音频、文字、数据表、着色器或存档，并列出公开仓库中含有的必要派生事实（shader 接口事实、NVN 函数名/位置、游戏内部偏移等）。有 7 处离线改写标明来自社区 LFS Mod Offline Fix，作者/首发来源未能确认。[`docs/DERIVED.zh-CN.md` 第 5–39 行](https://github.com/jhaizhou-ops/d2r-mobile/blob/53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53/docs/DERIVED.zh-CN.md#L5-L39) 因此许可证研究应分开核：兼容层源代码的 MIT、每个第三方组件的许可证、ACNH 自有游戏内容/逆向派生数据的权利边界、以及任何来源不明的既有补丁。D2R 项目采用的法律/发行判断不能自动推定适用于任天堂或 ACNH。

## 对 ACNH 的迁移判断表

| 部分 | 可复用的经验 | 不能直接复用 / 需要先证实 |
|---|---|---|
| CPU 执行 | ARM64 静态分析、精确指令改写、版本指纹、原字节断言、签名代码段嵌入 | ACNH 模块格式、x18/TLS/SVC 使用点和补丁地址必须重新测；任一 ABI/指令假设不满足就不适用 |
| Horizon 仿真 | 按导入符号分层替代服务；缺口显式报错；线程同步按服务协议实现 | ACNH 初始化的服务集与 IPC/共享内存行为未知，D2R 的 stub/简化实现不构成覆盖证明 |
| 图形 | 记录 API 语义、离线转换 shader、缓存 PSO、逐帧/GPU 归因 | NVN 函数行为、D2R Maxwell shader 反射、贴图和帧缓冲假设需按 ACNH 引擎重新验证 |
| 游戏 Hook | build ID + 原字节约束 + 独立可选功能门的工程纪律 | `main+offset`、私有 svc 和功能代码完全是 D2R 特定，不迁移到 ACNH |
| iOS 前端 | 多触点状态机、映射到原始触屏/手柄输入、生命周期和存档调度 | ACNH 对触控/手柄的交互逻辑、界面、画布比例和记忆占用需要单独逆向和实机验收 |
| 验证 | 结构验证、差分执行、存档对拍、屏幕基准、真机 smoke 和性能采样分开 | D2R 真值数据与数值不代表 ACNH 的正确性、性能或设备门槛 |

## 证据边界

本报告观察的是一个固定源码 commit 和其文档/实现。文档中的用户体验、设备覆盖、帧率与功耗数字没有在本任务中复现；源码组织只能说明作者实现了什么结构，不能证明其对 ACNH 的兼容性或权利判断。下一阶段若要评估 ACNH，应取得明确目标版本的合法自有转储并先做只读格式、导入、SVC、线程 TLS、图形 API/着色器和运行时内存画像，再决定是否值得做最小启动宿主原型。
