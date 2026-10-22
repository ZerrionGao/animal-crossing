# Mush8 源码研究：对 ACNH 原生 ARM64 移植的启示

> 本篇记录单个参考项目的源码研究，文中“未测试/未知”限定于这一分项。整轮研究随后完成了 ACNH 静态解析和 shader 编译采样，最新结论见[主报告](../acnh-port-feasibility.md)及[二进制画像](../acnh-binary-profile.md)。

研究对象：`c22dev/mush8`，固定 commit `975040ba341efb99c5631292a0266ae26b28d960`。本报告仅依据该 commit 源码；没有运行游戏、构建、测试或安装依赖，也没有读取密钥。文件引用均指向该固定 commit。

## 结论摘要

Mush8 展示的是“保留 Switch ARM64 游戏机器码、在离线阶段修补少量平台相关指令、在宿主侧重建 Horizon 运行时与 NVN 图形 API”的路线，不是传统高层移植。macOS 启动时载入修补后的游戏模块；iOS 因不能依赖 JIT，把修补模块与应用一起做成 Mach-O，并预编译 Metal shader。

这条路线对 ACNH 的最大可借鉴点是工程分层：NSP/NCA/NSO 资源提取、AArch64 离线改写、模块加载与重定位、Horizon IPC/SVC 兼容层、NVN 到 Metal 和 shader AOT。最大的障碍是工程中存在大量 Mario Kart 8 Deluxe 专属数据：更新版本和 module ID、具体补丁、NVN 驱动符号表位置、程序 ID、游戏 SDK 导入符号及 shader pipeline 集合。ACNH 不能直接拿 Mush8 的游戏补丁、驱动表或服务行为照搬。

源码规模约 28,797 行 Rust（`crates/` 下所有 `.rs` 文件的总行数；不是移植工作量估算），其中 `nxshader` 指令解码文件约 5,172 行、MSL 翻译约 2,056 行，patcher 的核心 rewrite 文件 589 行。由此判断，复用通用框架可省去底层基础设施工作，但支持 ACNH 的主要工作更像一个新的、逐项验证的游戏适配工程，而非小型配置移植。

## 实现路径与源码证据

### AArch64 指令改写：x18、系统寄存器、SVC 与分支

`nxpatch` 是对 Switch NSO 的离线改写器。分类器用指令字段粗筛可能涉及 x18 的指令，再通过 Capstone 读写寄存器信息确认；x18 site 分类为读写、分支、PC-relative、写 SP 或 exclusive 窗口等类别。[`classify.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/classify.rs#L31-L113)

改写器把来宾 x18 和 TPIDR 数据保存在 per-thread context，通过 `TPIDRRO_EL0` 找到宿主为来宾线程准备的上下文槽；短 trampoline 保存 scratch 寄存器，重写涉及 x18 的指令后再恢复。分支指令在 trampoline 内重新定位；PC-relative `ADR/ADRP` 和整数 literal load 会按新 PC 重新编码。遇到无法安全识别/编码的指令会报错，而不是静默继续。[`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L283-L339) [`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L454-L555) [`asm.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/asm.rs#L72-L160)

SVC 指令改写为一个 trampoline：把立即数放进 x16，再经 keypage 中的 SVC gate 调入宿主 dispatch，返回原指令后续地址。被识别的系统寄存器包括 TPIDR_EL0、TPIDRRO_EL0、计数器和 CTR_EL0；未处理的系统寄存器会使 rewrite 失败。exclusive 读写序列最多向前搜索 15 条指令寻找首个 store-exclusive；若窗口中有需要改写的指令，就把窗口作为一整个区域搬到 trampoline。对未命中的分支 x18 跳转表，代码要求存在已知地址/长度的 jump-table 条目，否则明确报错。[`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L353-L429) [`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L475-L588)

分支处理有明确边界：条件分支可在 trampoline 里重定位；普通无条件分支若落在改写区域会报 unsupported；`br x18` 仅支持列在常量表中的已知 jump table。literal SIMD load 也会被拒绝。故 ACNH 的指令分布须先扫描，再以真实二进制运行离线分类/重写，并对新增指令类别编写等价性验证，不能假设只改寄存器名即可。[`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L465-L480) [`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L525-L545)

项目提供 `nxpatch verify`：用 Unicorn 对原始和改写片段进行多组输入状态比较，并报告通过、未测试（原始代码各次都 fault）与失败；这是一种很适合复用的安全网，但本次没有执行它，也不代表对目标游戏的完整语义证明。[`main.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/main.rs#L88-L114) [`equiv.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/equiv.rs#L47-L66)

### iOS 无 JIT：静态 Mach-O 嵌入与重定位约束

iOS 构建把 `main`、`subsdk0`、`sdk` 三个已修补模块按 16 KiB 页对齐切分成 RX（代码）、RW（数据/keypage）和 RX（trampoline）Mach-O sections，再通过 `.incbin` 把模块字节直接嵌入最终应用。链接时固定这些 section 地址以满足来宾地址布局。构建脚本解析 ELF 风格 dynamic relocations，并拒绝落在 RX 区域的 relocation，因为这些代码页在 iOS 上不可写。[`build.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/build.rs#L6-L61)

运行时 loader 若模块已嵌入，就从 Mach-O section 的地址直接使用模块；否则 macOS 路径分配来宾地址空间并复制模块。loader 处理 RELATIVE、ABS64、GLOB_DAT、JUMP_SLOT relocation，未知类型报错；随后按代码、只读区域、数据、keypage 和 trampoline 设权限并刷新指令缓存。[`loader.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/loader.rs#L78-L110) [`loader.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/loader.rs#L159-L207) [`loader.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/loader.rs#L247-L294)

这说明 iOS 无 JIT 不等于必须重编译成另一套高层代码：本项目是在构建期把机器码放进签名应用，由宿主 loader 做数据重定位并映射受控的 guest address space。ACNH 适配前必须检查其模块 relocation 是否都能留在可写区；任何 text relocation、需运行时生成代码或跳转范围超限，都会成为阻碍。是否适用于目标 iOS 版本/签名权限，须在目标设备另行验证；源码存在扩展虚拟地址与增加内存 entitlement，IPA sideload 配置则不含前者，不能据源码推断所有设备/签名情形都可用。[`Mush8.entitlements`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/ios/Mush8.entitlements#L1-L8) [`Mush8-sideload.entitlements`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/ios/Mush8-sideload.entitlements#L1-L8)

### Horizon 服务、SVC 和文件系统

`nxhost` 自建 SVC dispatch、线程/内存/同步原语与 guest context；每个 SVC 立即数被送到该 dispatch。这里是系统调用兼容层，不是 Nintendo OS 的完整实现。[`svc.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/svc.rs#L9-L44) [`guest.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/guest.rs#L90-L150)

服务注册按 Horizon 服务名分派，例如 `fsp-srv`、`hid`、`audren:u`、`time:u`、`vi:u`、`nifm:u`、`bsd:u` 等；没覆盖的服务名退化到通用 `Stub`。个别服务命令也只有固定成功值或默认值，因此适配另一个游戏必须依赖该游戏实际发起的 IPC/命令日志逐项补齐，服务名映射本身不构成兼容证据。[`services/mod.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/mod.rs#L28-L57)

文件服务将 RomFS、保存数据、SDMC、DLC 与系统数据映射为宿主存储；系统数据可从本地 NSP 或用户提供的 Firmware NCA 查找。游戏启动路径明确加载 `main`、`subsdk0`、`sdk`，并查找固定的 `nn::init::Start` 符号，因此 ACNH 若有不同模块集合/入口、额外 SDK 模块、系统标题数据或 save 语义，需要独立确定和适配。[`services/fs.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/fs.rs#L19-L27) [`services/fs.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/fs.rs#L101-L129) [`main.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/main.rs#L208-L275)

### NVN → Metal 与 shader AOT

图形宿主通过目标 SDK 的 NVN 导出符号表建立 `nvn*` 函数名到地址映射。源码中的 driver table 只声明一个固定 module ID，并硬编码符号表、函数表和格式表偏移；初始化遇到未知 SDK build 会失败。该部分强烈绑定游戏随包 SDK 版本，不能直接用于 ACNH。[`nvn/mod.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/nvn/mod.rs#L16-L57)

NVN 接口的一部分映射为宿主实现，未实现入口会进入 stub 并记录调用情况。命令缓冲、纹理、渲染状态等进入 Metal draw/backend；shader 代码由 `nxshader` 解码/转成 MSL，宿主以 Metal 编译并创建函数。在线编译代码仍存在，但 iOS 构建路径用 pipeline 清单运行 `nxhost --precompile`，调用 `xcrun metal`/iOS SDK 产出 `.metallib`，再把 metallib 和索引打进 app；这减少设备端的 shader 编译与卡顿；CPU 代码签名限制与 Metal GPU 编译是不同问题。[`nvn/mod.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/nvn/mod.rs#L63-L92) [`draw/shaders.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/nvn/draw/shaders.rs#L228-L270) [`make-ios.sh`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/make-ios.sh#L42-L63) [`shbatch.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxshader/src/bin/shbatch.rs#L17-L37)

shader 集合既可从游戏文件递归扫描提取，也可从运行时 NVN program dump 收集；`data/pipelines.tsv` 是具体程序的预编译变体清单，不是通用 Switch shader 列表。ACNH 需要建立自己的 pipeline 覆盖集，包含图形选项/渲染目标变体，并统计无法翻译或 Metal 编译失败的样本。[`shextract.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxshader/src/bin/shextract.rs#L6-L28) [`objects.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/nvn/objects.rs#L368-L430) [`pipelines.tsv`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/data/pipelines.tsv#L1-L12)

## 与 ACNH 的复用判断

| 子系统 | 可复用基础 | 需要重新适配/当前未知 |
| --- | --- | --- |
| NSP/NCA/ExeFS/RomFS/NSO 工具 | `nxfs` 的容器、解密、NSO 解析和 RomFS patching 能作为工具层候选 | ACNH 实际 title/update/DLC 内容布局、目标固件/key generation、模块数量及压缩/relocation 特征未在本仓库确认 |
| AArch64 改写器 | x18/TLS、系统寄存器、SVC 入口、部分分支/PC-relative 改写和 Unicorn 差分验证框架 | ACNH 指令样本分类结果未知；其特有不支持指令、x18 跳转表和 exclusive 窗口均需新增分析和验证 |
| iOS Mach-O 静态嵌入 | 固定地址的 RX/RW/RX section 布局和 text relocation 门禁是可复用设计 | ACNH 模块总量、布局、text reloc、地址空间、签名 entitlement 及目标设备内存需求未知 |
| Horizon HLE | IPC/SVC/线程/内存/宿主文件/输入/音频/网络接口骨架可参考 | 服务调用覆盖、错误码、事件、时间/存档/输入语义须基于 ACNH 实际执行轨迹新增；不能把 stub 返回成功当兼容 |
| NVN/Metal | NVN 对象/命令流到 Metal 的抽象、纹理和状态处理、shader IR/编译管线具有最大通用价值 | 目前 NVN driver table 与若干地址/格式假设是 MK8D SDK 构建专属；ACNH 的 SDK 导出与功能调用需要新表/实现 |
| Game patches/启动 | 离线按预期原指令校验再 patch 的写法可复用 | `rewrite.rs` 的 Mario Kart patch、程序 ID、入口符号及版本处理必须重做 |

## 版本硬编码与适配边界

- 项目明确要求 Mario Kart 8 Deluxe update 4.0.1；CLI 默认更新文件名 `upd4-0-1.nsp`，启动提示和应用信息也写死 4.0.1。[`README.md`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/README.md#L12-L20) [`main.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/main.rs#L82-L105) [`am.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/am.rs#L108-L124)
- `nxpatch` 内的 module ID 和偏移补丁明确是 `MK8DX_401`；改写前逐字比对原指令，预期代码不同就拒绝。这是保护目标版本的好范式，但内容完全不可直接复用于 ACNH。[`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L20-L40) [`rewrite.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxpatch/src/rewrite.rs#L341-L352)
- 图形 driver lookup 以一个 SDK module ID 和若干表偏移固定定位，且 unknown ID 直接失败；版本升级或不同游戏 SDK 都须重建。[`nvn/mod.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/nvn/mod.rs#L26-L56)
- `PROGRAM_ID`、DLC ID 派生和服务版本字符串也是 MK8D 语境；切换游戏时需逐项排查而不是只替换游戏文件。[`svc.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/svc.rs#L9-L10) [`aoc.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/aoc.rs#L1-L18) [`am.rs`](https://github.com/c22dev/mush8/blob/975040ba341efb99c5631292a0266ae26b28d960/crates/nxhost/src/services/am.rs#L108-L124)

## 适配推进建议与停止条件

1. **先做目标二进制可行性盘点**：对合法取得的 ACNH base/update，提取模块清单与 module ID，统计 x18/SVC/sysreg/exclusive/跳表、text relocation、动态 relocation 类型、entry point 和内存布局。若存在大量不可定位的代码 relocation、无法安全离线改写的指令形态或 guest 地址空间超出 iOS Mach-O 可安排范围，先停在 macOS 原生实验，不进入 iOS 打包路线。
2. **把 nxpatch 做成独立适配阶段**：先输出分类报告，再实现最小增量规则；每种新增规则必须有单元/差分等价验证。检查未测试项和所有失败项，不能仅以“能构建/能进入主菜单”判定正确。
3. **明确 guest runtime 的游戏需求**：在 macOS 实验机加载模块，记录未实现导入、Horizon service/command、SVC、文件访问和线程行为，按实际需要补服务。对任何默认返回 0 的 stub 建立调用清单；对启动、存档、安全/网络相关路径尤其不能把 stub 静默视作成功。
4. **单独建立 ACNH graphics 适配**：确认 ACNH 使用的 NVN driver export、导出表布局和 pipeline；用真实 shader 扫描/运行时采集生成覆盖清单，统计转换失败、MSL 编译失败及渲染差异。先桌面 Metal，再 iOS AOT。
5. **最后做 iOS 固化与设备验收**：只有模块布局/重定位、HLE 关键服务、Metal shader 覆盖和内存需求在 macOS 阶段稳定后，才生成静态 Mach-O 并做签名、启动、长时间负载、生命周期/触控手柄、存档读写的真机验收。

## 明确未知

本次只研究 Mush8 源码，不持有或解析 ACNH 二进制；因此不确定 ACNH 的实际 NSO 模块数量、模块间接口、x18 及系统寄存器指令分布、text relocation、是否使用与 Mush8 相同的 NVN 导出机制、shader 数量与翻译覆盖率、Horizon service/command 足迹、存档格式和 iPhone 内存峰值。上述项目应由目标游戏样本与运行轨迹决定，不能从 Mush8 的 README 或 Mario Kart 8 Deluxe 经验推断。
