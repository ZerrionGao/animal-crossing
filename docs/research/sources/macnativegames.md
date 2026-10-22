# MacNativeGames 源码研究：TOTK/BOTW 原生 ARM64 路线对 ACNH 的参考

> 本篇记录单个参考项目的源码研究，文中“未测试/未知”限定于这一分项。整轮研究随后完成了 ACNH 静态解析和 shader 编译采样，最新结论见[主报告](../acnh-port-feasibility.md)及[二进制画像](../acnh-binary-profile.md)。

**检查对象：** `ly05010419/MacNativeGames`，固定提交 `dfa7ec426f8b37798073a108ede1504c827d8af0`。
**本报告范围：** 仓库源码、文档和其明示的验证范围；没有运行游戏、构建、安装依赖或读取密钥。CodeGraph 显示 `Not initialized`，按只读边界没有初始化，改用针对性 `rg` 与源码行号阅读。
**固定源码链接：** 以下链接均指向该提交，不随默认分支漂移。源码事实、从事实得出的 ACNH 推断、源码未能回答的事项分开标注。

## 结论先行

MacNativeGames 的办法是“原生执行 Switch AArch64 模块 + 针对 OS/硬件边界做二进制改写 + 小型 Horizon 服务宿主 + 自研 NVN 到 Metal 后端”。这不是全指令集模拟，也不是将游戏重编译成常规 Mach-O 应用代码。macOS 运行时把重写后的模块映射到进程地址空间；iOS 先在 Mac 上完成所需的游戏代码改写，再把代码段作为带签名保护的 Mach-O 段嵌入 iOS app，运行时以只读可执行映射取回代码。

对 ACNH 最有复用价值的是改写器、运行时模块装载/重定位、SVC/IPC/Horizon 线程与 TLS 桥、访存与地址空间框架，以及 NVN/Metal 后端的通用机制。必须重新做的是 ACNH 对应版本的代码站点和启动次序、系统服务覆盖、ROMFS/字体/音频/输入适配、NVN 功能实际覆盖与着色器身份/预热数据。该仓库的 BOTW 加入经验说明：同一 Switch 世代能复用很大一部分宿主，但仍要增加独立标题识别、版本指纹、游戏宏/配置、代码补丁、RomFS 格式和着色器清单；BOTW 目前也没有 iOS 构建路径。

## 源码观察

### 1. 构建管线

顶层 `build_app.py` 把流程分成四步：提取和校验 ExeFS；解密/合并 RomFS、系统字体并加 overlay；从玩家本地游戏数据提取 NVN 着色器并翻译为 MSL/Metal 库种子；最后链接宿主、组装 app/IPA 和数据包。入口注释也明确要求 XCI/NSP 与本机用户自己的 `prod.keys`，密钥值不打印。[构建步骤与输入约束](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/build_app.py#L1-L35) [Exefs 提取、改写及哈希门禁](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/build_app.py#L428-L468)

支持版本不靠文件名猜：从容器中的 title ID 判定游戏；对已支持游戏版本使用期望散列、NSO build ID/指令检查和生成物散列约束。顶层代码列出 TOTK 与 BOTW title ID，TOTK 版本数据另有多个 expected JSON，BOTW 有独立 expected 文件。[游戏/title ID 映射](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/build_app.py#L40-L58) [TOTK/BOTW 路由与验证入口](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/build_app.py#L1050-L1095) [Makefile 原生链接目标](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/Makefile#L67-L84)

`prepare_modules.py` 生成的 ELF 是分析载体，不是可执行移植物；代码注释明确标注 `analysis carrier only; not a native port or runnable program`。真正的路径是 `patch_nso.py` 从 NSO 解压 text/ro/data/BSS，追加 key page 与 patch area，并输出 `.patched.bin`、fixup、gate 等元数据。[ELF 分析载体说明](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/scripts/prepare_modules.py#L191-L211) [NSO 变换格式与代码](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r/patch_nso.py#L1-L47)

### 2. ARM64 改写、Mach-O 与模块装载

改写器针对 Apple 内核特殊寄存器和 Horizon 语义做定点变换：把 guest x18 放到线程数据槽；把 `tpidr_el0`、`tpidrro_el0` 改为 guest TLS/TLR 槽访问；把 `svc #n` 和 `mrs cntpct_el0` 转为宿主 gate；gate 根据改写指令的返回地址查找原 SVC 号或计时器操作。修改前按 NSO build ID 断言代码字节，避免错误版本静默应用补丁。[改写清单及格式](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r/patch_nso.py#L1-L22) [已登记改写范围](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r/patch_nso.py#L30-L47)

所以这里的“Mach-O”主要是宿主程序的格式及 iOS 的代码承载容器，不代表把 guest 的 NSO 转换成标准 Mach-O dylib。macOS loader 以匿名可写映射装入扁平模块数据，做 guest ELF 风格的相对/符号重定位，然后按 guest 的 text/rodata/data 权限封存；代码里用 `mmap`、`mprotect` 和模块元数据处理，而没有调用 dyld 加载游戏模块。[macOS 模块映射、校验及重定位](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/loader.c#L183-L232) [权限封存](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/loader.c#L245-L260)

iOS 采用不同路径：在 Mac 构建阶段让 runtime 执行补丁，再导出 text；`gen_embed.py` 用汇编 `.incbin` 把客体代码与 patch area 放进 Mach-O 的只读可执行段，把其余模块放进可写段，并对段哈希；签名 app 中的代码段再由 loader 以 `vm_remap` 映射回相对布局。注释明说 iOS 运行时写入的代码不能执行。[Mach-O 段生成及哈希](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/tools/gen_embed.py#L17-L59) [iOS loader 从已签名段映射代码](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/loader.c#L139-L180) [iOS IPA 构建顺序](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/tools/build_ipa.py#L40-L119)

### 3. Guest 地址空间、Horizon SVC、IPC、线程与 TLS

 guest 地址不是任意 host 指针堆。源码将 guest 虚拟地址控制在 `2^39` 以下，因为 HIPC descriptor 只编码 39 位地址；为 Switch 的地址别名区、线程栈和堆分别保留空间。桌面尝试真实 64 GiB alias 区，地址冲突时在地址洞中预留并按需缩小；iOS 的可用虚拟地址更紧，默认从 16 GiB 下探到至少 4 GiB。应用内存池默认对游戏报告 retail 3.2 GiB 左右，iOS entitlement 请求提高内存限额，但这不消除设备的实际物理内存压力。[地址布局和平台差异](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/services.c#L18-L79) [地址洞预留与 alias 区分配](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/services.c#L81-L128) [iOS 增内存 entitlement](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/Sources/TOTK.entitlements#L1-L8)

线程按“一个 guest 线程对应一个 host pthread”实现；线程启动时切到游戏 SDK 提供的 guest 栈，host 服务调用时切到 pthread 的宿主栈。优先级和 core affinity 是运行时内部调度模拟/记账，最终由 Darwin 调度 pthread。源码列举了 Horizon 线程、锁、条件变量、地址等待和事件 SVC；这是明确实现过的子集，不代表完整 Horizon 内核。[线程/SVC 范围与语义说明](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/threads.c#L1-L23) [Create/Start/Sleep 实现](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/threads.c#L617-L690) [host/guest 栈切换](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/thread_enter.S#L1-L24) [host service gate 保存 guest 寄存器和栈](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/gate.S#L5-L23) [gate 的寄存器保存、分派、恢复](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/gate.S#L24-L123)

TLS/TLR/x18 被映射到 Darwin TSD 槽。运行时分配每线程 TLS、约 0x200 字节的 TLR 结构以及 256 KiB signal stack，再核对 Darwin 的只读线程指针区确实能按改写器预期槽位读出 TSD。这里依赖 Apple ABI/运行时细节与固定校验，移植或 OS 更新须重新验证。[线程环境与 Darwin TSD 校验](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/loader.c#L275-L300)

IPC 是进程内 HIPC/CMIF 服务管理器：guest SDK 客户端代码继续运行，消息从当前线程 TLS 的 IPC buffer 取出并原地响应；服务名/命令注册到表中，未知 service/command 明确报错而不是伪成功。当前公开源码中 `ConnectToNamedPort` 只接 `sm:`，之后按名称向本地注册表要 service session；因此每个新游戏遇到的服务都需观察并增加真实 host 行为。[IPC 设计、未实现行为](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/ipc.c#L1-L18) [本地 service manager](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/ipc.c#L166-L209) [SVC 分派顺序和失败边界](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/services.c#L323-L405)

### 4. NVN 到 Metal 4、Ryujinx shader 翻译与 AOT

图形调用不是由 Ryujinx 全机模拟器承接。此仓库包含从 D2R Mobile 派生的 NVN host API 实现和 `nvn_mtl.m`，把达到的 NVN 调用翻译为 Metal 资源、shader、命令编码和提交；GPU 后端大量使用 Metal 4 的 command queue、command allocator、argument table 等。仓库自己的说明把它称为实验性 adapter，并声明“目标版本不意味着 API 全覆盖”、有未实现调用会失败、无完整 game frame 验证的早期状态；README 对后续实际版本的验证情况是另一时点记录，应以目标提交的描述及具体测试证据谨慎界定。[NVN/Metal adapter 边界](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r_gfx/README.md#L1-L16) [Metal 4 命令对象初始化](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L500-L550) [OS 要求](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md#L78-L101)

shader translator 复用 Ryujinx.Graphics.Shader 的 Maxwell machine code 解码/IR 翻译器，经 SPIR-V，再经 SPIRV-Cross 到 MSL；TOTK 的 service 额外补充实际 NVN reflection/query 输入、MSL 修改和资源依赖反射。游戏 shader 来源是玩家自己提供的 RomFS；translator 将 shader 做成种子，覆盖启动包缺省绑定状态及共享清单中记录的变化状态。其目的是在启动/游玩前离线生成，而非要求 iOS 玩的时候即时编译。[转换路径与输入参数](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/tools/totk_xlate/Program.cs#L1-L21) [Maxwell→SPIR-V 翻译入口](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/tools/totk_xlate/Program.cs#L119-L138) [SPIRV-Cross、MSL 与反射生成](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/tools/totk_xlate/Service.cs#L301-L378) [shader seed 来源与平台目标](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/scripts/prepare_shader_seed.py#L1-L20)

发布版提供预先用 .NET NativeAOT 编译的 macOS arm64 translator，构建脚本校验 translator SHA256；这让最终 Mac app 不需要 .NET/Python runtime。此项目的 iOS 包没有携带桌面 shader 翻译服务，未知 variant 的 draw 会被跳过并记录请求，可从设备提取到 Mac 补齐、编 metallib 后推回设备。离线 shader 种子是实际功能覆盖的前置条件，不是仅作首屏加速的可选项。[AOT translator 说明](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/prebuilt/macos-arm64/README.md#L1-L19) [iOS 缺少 shader 时的行为及回填工具](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/tools/fill_shaders.py#L1-L12) [seed shader 清单导出与 iOS app 组装](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/tools/build_ipa.py#L65-L119)

### 5. TOTK/BOTW 游戏 profiles 与新增游戏成本

这里的“profile”不是数据驱动的一张全能配置表，而是编译期/源码中的游戏与版本分支。TOTK 代码地址宏把版本专属偏移分散在 `game_version.h`、`game_patches*.c`、改写器规则和 shader/ROMFS 准备脚本；每个写入点先校验原始指令字节，版本变了需要重新定位、审计并验证。比如 TOTK 的实时帧步进、60 FPS 和高帧率修补均以地址/指令模式为依据。[版本地址头和 BOTW 分支](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/game_version.h#L1-L30) [补丁前做原字节校验及版本门禁](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/game_patches.c#L520-L594)

BOTW 的新增说明显示了成本边界：build_app 中新增 title ID、预期散列和构建分支；运行时用 `GAME_BOTW` 编译不同的游戏地址/启动回调与补丁实现；BOTW 有专用 `game_patches_botw.c`，且没有照搬 TOTK 的地址补丁；shader 容器格式、提取目录与 shader 清单另行配置；BOTW 对共享字体采用从 TOTK build 提供的输入。相同宿主图形/线程/IPC 层被复用，但游戏身份和内容不是“加一行 title ID”即可。[BotW 独立补丁入口](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/game_patches_botw.c#L1-L23) [BOTW shader 格式/目录特例](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/scripts/prepare_shader_seed.py#L1-L13) [README 中 BOTW 支持和未支持范围](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md#L172-L195)

**对 ACNH 的推断：** 即便 ACNH 与 TOTK/BOTW 使用相同 Horizon/ARM64/NVN 生态，完整移植至少要新建 ACNH 的 title/version/data profile：校验固定版本的 `main/sdk/subsdk` 等模块指纹、确认 NSO 重写扫描能处理其代码模式、为新增/不同 SVC 与 IPC 建服务实现、补 ACNH 的启动与系统调用路径、分析其 NVN shader/绑定资源特征、制备独立 shader map/seed，并重新作真实场景验收。它可能复用现成 runtime 大框架，但实际所需服务和 graphics API 触达覆盖只能靠 ACNH 运行轨迹/错误证据确定。

### 6. iOS 内存、JIT 与发行条件风险

源码注释给出约 3.2 GiB guest application pool，iOS 使用大容量连续虚拟地址 reservation，且 entitlement 请求增加应用 memory limit；README 记录 iPhone 14 运行接近 4 GB，并见过繁忙地点几分钟后被系统终止。这意味着 iOS 目标需要验证实际设备内存压力、alias 区可用地址洞、Metal 资源峰值和长时运行，而不能由“能链接/能装入”推断可玩性。[guest pool/虚拟地址布局](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/services.c#L18-L75) [entitlement](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios/Sources/TOTK.entitlements#L1-L8) [iPhone 实际内存观察](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md#L188-L195)

报告中的“iOS 无运行时写入可执行代码”是代码注释给出的限制和本项目的实现策略；未知项是 ACNH 引擎/loader 是否还有其他动态代码、JIT/可执行页需求。要用 ACNH 自己的执行路径确认，不能假设补了 shader AOT 就自动消除所有动态代码问题。Metal 4 的可用性也要与目标 iOS SDK、设备 GPU 能力和部署版本核对；此 checkout 的 README 主要求是 macOS 26+，iOS 使用 Xcode/iOS 26+，并非所有设备均承诺相同 Metal 能力。[平台构建门槛](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md#L78-L125) [Metal 4 后端对象](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m#L500-L550)

### 7. 许可与第三方边界

仓库顶层 MIT 许可覆盖本项目许可范围；Third-party notices 逐项指出嵌入的上游组件和各自许可证：改写器及 NVN/Metal 代码来自 d2r-mobile，含 deko3d 来源布局常量、bcdec；translator 复用了 Ryujinx shader 代码；另有 SPIRV-Cross Apache-2.0、hactool ISC/mbedTLS Apache-2.0、libopus/libvpx BSD。计划复用时应逐目录保留原 notice/license、记录本地改动和构建时拉取的固定版本，重新审查分发物中 NativeAOT/.NET runtime 与工具链组件的许可证义务。[第三方清单](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/THIRD_PARTY_NOTICES.md#L1-L23) [顶层 MIT](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/LICENSE#L1-L21)

游戏数据、密钥、游戏 shader 代码没有提交到仓库；build 从用户本地 dump 产生 app/seed，README 要求用户自己的游戏及 Switch keys，且明确不要分享生成的 app。仓库包含游戏模块 hash、补丁地址及 shader/pipeline 标识表；这些标识表不含 shader 机器码。该报告不作各司法辖区的法律判断，但实现和发行设计应继续把“可复用的兼容层代码”与“受保护的游戏内容/密钥/生成应用”严格分开。[内容及个人 dump 边界](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md#L12-L18)

## ACNH 方案里可引用的复用清单

| 子系统 | 复用价值 | ACNH 必须新增/重审 |
|---|---|---|
| NSO/ARM64 改写器 | 针对 x18、TLS/TLR、SVC、计时器的转换框架；build ID/原始字节断言；patch/gate 产物格式 | ACNH build IDs、扫描覆盖、调用窗口、需重写操作、代码补丁 |
| Runtime 模块装载 | 符号/重定位、权限校验、Mach VM 映射、崩溃和 guest 调用栈诊断 | ACNH module 集合、动态依赖、初始化顺序与 ABI 特例 |
| Guest 内存 | 39 位 HIPC 兼容地址模型、alias/stack/heap、逻辑权限与 host 16 KiB 页折衷 | ACNH 报告/实际 pool、映射要求、峰值、iOS 地址洞/物理内存实测 |
| Horizon host | pthread guest threads、Darwin TSD TLS 桥、SVC 门、HIPC/CMIF 框架 | ACNH startup trace 所需的 SVC、service、command 及准确语义 |
| GPU | NVN 资源/命令到 Metal 4 adapter 和调试探针 | ACNH 用到的完整 NVN API 交集、格式/同步/并发/精度语义与 Metal 验收 |
| Shader | Maxwell→Ryujinx IR/SPIR-V→MSL 工具链和 AOT seed 工程 | ACNH shader 提取格式、bindings/reflection、variant 盘点、iOS 无漏 draw 的覆盖计划 |
| 内容/平台 | 原生输入/音频/文件/窗口集成模式 | ACNH 文件系统/保存格式、字体、视频、声音、控制器/UI 与 iOS 独立产品适配 |

**未知（需后续 ACNH 源码和运行证据）：** ACNH 可用版本及模块散列；是否有不同/新增 Horizon 服务或系统模块；实际是否依赖 JIT 或运行时 shader 编译；NVN 扩展调用及 shader 数量；性能/内存/稳定性目标；Mac 与 iPhone 的目标 OS/GPU/签名能力。此 MacNativeGames checkout 的资料不能替代这些测量。

## 关键固定提交引用

- [README：平台、数据边界与已支持游戏](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/README.md)
- [build_app.py：游戏识别、验证和构建入口](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/build_app.py)
- [patch_nso.py：AArch64 重写](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r/patch_nso.py)
- [loader.c：guest 模块映射与运行时](https://github.com/ly05010419/MacNativeGames/blob/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime/loader.c)
- [services.c / ipc.c / threads.c：Horizon host](https://github.com/ly05010419/MacNativeGames/tree/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/runtime)
- [nvn_mtl.m 与 Shader translator](https://github.com/ly05010419/MacNativeGames/tree/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/third_party/d2r_gfx/gfx/nvn)
- [iOS 段嵌入和 shader AOT](https://github.com/ly05010419/MacNativeGames/tree/dfa7ec426f8b37798073a108ede1504c827d8af0/games/totk/ios)
