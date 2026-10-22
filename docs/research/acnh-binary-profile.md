# ACNH 二进制静态画像（base / update）

日期：2026-10-21。补查修订：上一版先漏解码 RELR，后又发现只在存在 `DT_HASH` 时才解析动态符号，导致 update 的 GNU-only `rtld`/`sdk`/`subsdk0` 被错误记作零符号；这两处“零”都不是事实。本版加入 ELF64 RELR 展开，并用 NSO dynsym header 字段解析无 SysV hash 的符号表。上一版“无 text relocation”当时证据不完整，本版经 RELR 补查后结论成立。NSO 头内分段哈希也已逐段复算。分析对象是已提取的 ExeFS `main`、`rtld`、`sdk`、`subsdk0` 与 `main.npdm`。分析没有读取密钥、启动游戏或改动提取原件。NCA/ExeFS 验证事实来自每版本的 `extraction-manifest.json`：FS header、ExeFS hash table 与全部 ExeFS 数据块通过校验；RSA 签名未验证。以下 ELF/指令信息由提取文件静态解析获得。

## 版本标识边界

由已校验 Control NACP 确认：base 显示版本 `1.0.0`，update 显示版本 `3.0.0`。CNMT 元数据也已做 PFS0 数据块哈希验证：base `version_raw=0`、type 128；update `version_raw=2031616`、type 129，Title ID `01006F8002326800`。CNMT raw 值保留原样，不在这里转换成显示版本。证据见 `work/research/package-profile.json`。

## NSO 与代码段

NSO build ID、每段文件压缩大小/解压后内存大小、解压段 SHA-256、ExeFS 整体哈希匹配结果及完整 MOD0/重定位明细在 [nso-profile.json](../../work/research/profile/nso-profile.json)。表中 `.text` 大小是最直接的代码规模变化指标；所有被解析段均使用 LZ4 压缩。

| 模块 | base build ID（前 16 字节） | base text 内存 | update build ID（前 16 字节） | update text 内存 |
|---|---|---:|---|---:|
| main | `7fc1baff976aeca414520cb89f4616cb` | 35,723,872 B | `5d913cf71eb24cb22c5105b1fa3efd97` | 66,081,456 B |
| rtld | `31a2773ea6928ffe3d32b17b3ad08f54` | 5,728 B | `283033deea3fc0b60104a4790098b9cc` | 9,536 B |
| sdk | `ad3fdfbd62a5f4b082d385ffe420d440` | 5,503,424 B | `a226048605d3597f90cb1067d141cd36` | 6,692,912 B |
| subsdk0 | `5a7ada8dd7447c10aa244944f64d0aff` | 3,582,656 B | `6c6ceace7be94107a8bd440bc2d60704` | 3,501,008 B |

build ID 按 NSO 头中的 32 字节原样记录，表格为便于阅读仅展示前 16 字节；完整值见 JSON。八个 NSO 的 flags 均为 `0x3f`，三个分段哈希标志均启用；对每个模块解压后重算 `.text`、`.rodata`、`.data` SHA-256，均与 NSO 头 `0xa0..0x100` 中对应摘要完全匹配。base 与 update 的 `main`、`rtld`、`sdk`、`subsdk0` 也均匹配 ExeFS 提取清单 SHA-256。base 的 `main` `.text` 为 35,723,872 B、`.rodata` 为 14,718,844 B、`.data` 为 3,284,680 B、BSS 为 7,835,960 B；update 对应为 66,081,456 B、8,507,616 B、4,401,456 B、15,394,512 B。

## MOD0 动态表与导入

四个模块两版均找到 MOD0。RELA/JMPREL 按 AArch64 RELA 解码；并对 `DT_RELRSZ`(35)、`DT_RELR`(36)、`DT_RELRENT`(37) 采用 ELF64 RELR 规则展开：偶数项是直接地址，奇数项的位图表示后续 63 个 8 字节槽。分析对标签完整性、项大小为 8、表范围、目标对齐与范围、位图前置基址和重复目标做了边界检查。所有目标按 NSO 映像偏移归到 `.text`、`.rodata`、`.data` 或 BSS。逐项分类结果和段内目标偏移数组在 JSON；两版八个模块的 RELA/JMPREL 与 RELR 目标都没有落在 `.text` 或 `.rodata`，RELR 目标均在 `.data`。

`main` 的 base RELA/JMPREL 类型计数：ABS64 5,614、GLOB_DAT 34、JUMP_SLOT 874、RELATIVE 298,521；update：ABS64 6,556、GLOB_DAT 30、JUMP_SLOT 982。另有 RELR：base 四模块均无 RELR；update `main` 展开 373,220 项、`rtld` 39 项、`sdk` 15,005 项、`subsdk0` 3,424 项，全部目标归入 `.data`（`.text`/`.rodata` 为 0）。update `main` RELA/JMPREL 有 7,568 项，也全部目标位于 `.data`；因此之前仅根据 RELA/JMPREL 得出的无 text relocation 判断虽经本次证实为真，但前一版没有覆盖 RELR，现已补齐证据。各模块完整分类和偏移见 JSON。base `main` 动态依赖为 `multimedia.nss`、`nnSdk.nss`；update 为 `multimedia.nss`、`nnSdk.nss`。其中 依赖名按 basename 输出，不暴露 NSO stringtable 中的路径前缀。

`nvnBootstrapLoader` 在 base/update 的 `main` 与 `subsdk0` 导入表中均可定位；base/update `sdk` 导出 `nvnBootstrapLoader`、`nvnBootstrapLoaderInternal`、`nvnDevtoolsBootstrap` 等符号，update sdk 还导出 `nvnInstallGlobalDebugCallback` 和 `nvnPrivate0`。`subsdk0` 另有 34 个名称含 `Nvn`/`nvn` 的导出，包含 mangled Android native-buffer 辅助函数；名称命中本身不证明这些是 NVN 图形 API。base/update 的符号表总数：main 941/1,068，rtld 20/3，sdk 25,042/23,034，subsdk0 11,514/10,110。完整命中名称、导入数量、符号表计数和计数来源见 JSON。能确认 bootstrap 导入/导出入口存在，但不能据静态名称确认具体调用链、实际 API 覆盖或运行时行为。

## GNU-only 动态符号表与解析器兼容性

update `rtld`、`sdk`、`subsdk0` 没有 `DT_HASH`，但存在 `DT_GNU_HASH`（tag `0x6ffffef5`）；主程序仍同时有 `DT_HASH` 与 `DT_GNU_HASH`。因此若只读取 `DT_HASH`，三个辅助模块的符号数/导出会被误报为 0。补充解析使用 NSO 头偏移 `0x98`/`0x9c` 的 dynsym 相对偏移与大小，校验 `DT_SYMTAB` 地址和 24 字节 ELF64 符号项边界后计算精确数量；`0x90`/`0x94` 是 dynstr 字段。update 三个模块得到符号总数分别为 3、23,034、10,110；完整表计数与 source 字段在 JSON。

兼容性对照需按具体文件区分：当前 MNG `games/totk/scripts/prepare_modules.py:73-79` 并非无条件索引 `tags[4]`，它有 NSO header dynsym fallback；同一文件 `prepare_runtime.py:50-56` 也有 GNU-only fallback。因此这两处符号计数本身不应报告为缺口。残留的有条件缺口是 MNG 的 `games/totk/third_party/d2r/patch_nso.py:774`：进入 `if calls:` 的 call-window 检查时直接读取 `tags[4]`，若 GNU-only 模块触发该路径会失败；Mush8 `/tmp/acnh-port-research-20261021/mush8/crates/nxfs/src/module.rs:109` 则要求 `DT_HASH` 非零。若用这些路径分析/加载该 update，仍需补 GNU-only 兼容。

## RELR 实现对照

MacNativeGames 的 `/tmp/acnh-port-research-20261021/MacNativeGames/games/totk/third_party/d2r/relr.py:9` 提供有边界检查的 `decode_relr`，覆盖 tag/entry size 校验、表范围、地址对齐、重复目标、隐式 addend 与基址/位图游标展开；测试在同仓 `tests/test_relr.py`。Mush8 的 `/tmp/acnh-port-research-20261021/mush8/crates/nxfs/src/module.rs:88` 对 tag 35/36 直接报 `RELR relocations are not supported`，且该解析器没有 tag 37。因此 Mush8 当前解析器会拒绝这些更新模块，必须补齐 RELR 支持后才能使用；不能忽略该拒绝继续生成清单。

## NPDM 权限与 SVC / 系统寄存器指令扫描

hactool `--json` 原始权限输出： [base-npdm.json](../../work/research/profile/base-npdm.json)、[update-npdm.json](../../work/research/profile/update-npdm.json)。两版均报告 `is_64_bit: true`、`address_space_type: 3`、主线程栈 `0x100000`、线程优先级 44、默认 CPU 0、允许 CPU 0–2 和优先级 28–59。此处保留 address-space 原始枚举值 3，不对其映射到未核验的位宽语义。

两版都列出 SVC 访问能力。update 相对 base 新增 `svcMapPhysicalMemory (0x2c)` 与 `svcUnmapPhysicalMemory (0x2d)`。update NPDM 的 Service Access Control 相对 base 新增 `ectx:aw`、`mnpp:app`、`ngc:u`、`ngct:u`、`notif:a`、`olsc:u`；此处仅报告授权列表差异，不能据此认定代码实际使用对应服务。

未安装 capstone/lz4 Python 包；按任务约定使用系统 `/opt/homebrew/lib/liblz4.dylib` 通过 ctypes 解压。对每个模块 `.text` 做了 4 字节对齐的 AArch64 指令字线性编码扫描：base `main` 命中 36 个 SVC 编码、2 个 MRS、3,735 个 MSR/SYS 编码；update 分别 36、4、101,560。计数包含嵌入数据、跳转表、不可达代码和其他非指令字，属于上界候选值，不能解释为可达指令、系统调用实际使用数或调用覆盖。更不能仅凭扫描结果判断运行时调用路径。各模块计数见 JSON。

## iOS 移植门禁相关硬事实

- 程序是 AArch64 64 位，且 NPDM 声明 `address_space_type: 3`；iOS 目标进程 ABI/内存模型与平台能力是否能承载该程序，静态画像无法证明。
- 两版主程序均有 `nvnBootstrapLoader` 导入，且 NPDM 声明 `nvdrv` 服务访问；这是 Nintendo NVN/NVIDIA 驱动路径相关硬事实，足以要求后续图形后端专项评估，但不能据此断言整个游戏只能靠某一种实现。
- NPDM 声明 Switch 系统服务、文件系统权限与一组 SVC 权限。iOS 缺少这些系统服务的直接等价项，实际调用和可替代性需通过代码可达路径或运行时证据验证；静态本轮没有计算覆盖。
- `main` update `.text` 比 base 增大约 30.36 MB；这说明不能只按旧版代码体量估计移植工作量。资源、脚本和玩法数据不在本次二进制画像范围。
- 以上不是 iOS 构建、签名、运行或兼容性测试结果；未生成移植代码，也未做真实运行时验收。

## 可复核证据

- 汇总及原始 NSO/MOD0/符号/重定位/线性指令候选 JSON：`work/research/profile/nso-profile.json`
- 可复用的只读分析器：`work/research/profile/analyze_nso.py`
- base/update NPDM JSON：`work/research/profile/base-npdm.json`、`work/research/profile/update-npdm.json`
- Control NACP/CNMT 版本及内容哈希证据：`work/research/package-profile.json`
- 提取完整性和 ExeFS 文件哈希清单：`work/research/base/extraction-manifest.json`、`work/research/update/extraction-manifest.json`
- 解析方法参考：`/tmp/acnh-port-research-20261021/d2r-mobile/dm/verify/nsoimg.py` 与 `/tmp/acnh-port-research-20261021/mush8/crates/nxfs/src/module.rs`

本轮另用 MNG 固定提交的 `decode_relr` 对实际 3.0.0 四模块交叉检查，391,688 个目标逐项与独立解析器一致，且上游隐式 addend 边界检查通过。上界采用每模块 data+BSS 末端；这不是 loader 的布局/执行验收。记录见 [汇总证据](evidence/acnh-static-summary.json)，本地脚本为 `work/research/profile/check_upstream_relr.py`。
