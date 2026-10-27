# ACNH Mac 原型实施记录

依据：已批准的[研究方案](../research/acnh-port-feasibility.md)。主基座为 MacNativeGames `dfa7ec426f8b37798073a108ede1504c827d8af0`，源码保存在 `third_party/macnativegames`。在现有 main 工作区实施，不创建 worktree，不提交或推送。

项目范围为本机离线 Mac 原型；云备份明确不做。OLSC 仅按离线策略处理游戏的设置查询，不提供云端上传、下载或恢复。

## 范围与门禁

产品目标锁定 ACNH 3.0.0；1.0.0 只用于差分诊断。第一批实现可复核输入、更新资源合并和 CPU 改写；随后接入 Mac 装载器，按真实失败补系统服务与图形调用。所有生成物置于忽略的 `work/acnh/`。游戏/keys 原件只读，正式岛存档不在本阶段使用。用户已指定后续真机为 iPhone 17 Pro Max、iOS 27.2，并有苹果开发者会员；设备连接、签名配置和真机能力仍待 D 阶段现场核验。

- [x] A1：独立 ACNH 提取入口、标题/版本校验，base/update ExeFS。
- [x] A2：固定 hactool BKTR 工具、3.0.0 RomFS 六层 IVFC 校验与索引。
- [x] A3：GNU-only 安全检查修补、三执行候选模块改写、结构/等价验证。
- [x] B1：ACNH 构建与装载表，隔离 TOTK/BOTW 固定地址与游戏补丁，核验 SDK 启动入口。
- [ ] B2：Mac 真实启动与首帧，枚举未实现 SVC/IPC/NVN。
- [x] C：新岛命名、存档、退出重启、跨天与画面/音频验证。
- [ ] D：指定 iPhone 的首次安装启动、输入/存档/前后台/资源验收。

复选框在独立审查与新鲜验证均通过后更新；静态检查、构建成功和真实游戏运行分别记录。A3 若出现无法证明等价的指令，不继续启动该产物，不绕过安全断言。

## 实施步骤

1. 固定基座源码与依赖，在本项目 `.venv` 运行工具；保留 upstream 和第三方许可。
2. 为 ACNH 提供独立资源 adapter，复用上游容器/校验逻辑，输出成功后原子发布。
3. 给改写器补 GNU-only dynsym 计数，不移除 call-window 目标检查；绑定准确 SHA/Build ID，禁止 D2R 私有补丁。
4. 使用 GAME_ACNH 专属编译路径及表生成器。入口、构造函数和实际执行模块均以 ACNH 样本核验，不冒用 BOTW。
5. 对真实运行结果做诊断，只在上一门禁通过后推进下一门禁。

## 当前证据

阶段 A 已经独立审查与新鲜验证通过，详见 [阶段 A 证据](stage-a-evidence.json)。三模块合计 77,584 个指令改写片段完成差分测试，450 个负向对照全部被检测；零有效样本的站点为 0，双异常试验未计为有效。系统调用门由结构检查覆盖，不等于已验证系统服务语义。

阶段 B1 已通过独立审查、21 项单测、正式启动构建及三模块装载探针（4 线程、4,000 次跨模块调用）。独立审查发现的 application ID、资源覆盖层和字体校验问题均已修复。

首次真实启动已执行 SDK 初始化、三模块 DT_INIT 和 `nnMain`，创建 Apple M5 Max Metal 设备及 1920×1080、三缓冲 NVN 交换链；随后因 `nifm:u` 未实现而明确停止。日志为 `work/acnh/3.0.0/boot-first.log`。这证明游戏代码已执行到图形初始化，尚未产生游戏首帧；B2、存档与 iPhone 均未验收。

第二次启动已实际打开 `nifm:u`，随后依次调用 IPC control 3（查询指针缓冲区）和 0（转换为 domain），在不支持 domain 的协议处停止，尚未调用网络查询命令。该次日志 `work/acnh/3.0.0/boot-second.log` 确认显示节拍为 60 Hz 基准、客体 interval 2、宿主周期 33.333 ms；没有连续出帧或游戏计时验收。下一步实现有界 domain 对象路由与会话生命周期。

第三次启动通过了 domain 转换与 GeneralService 创建，随后因缺少 `hid` 服务停止，日志为 `work/acnh/3.0.0/boot-third.log`。IPC 对象实现已有 23 项项目测试和集成构建通过，但仍未产生游戏首帧，B2 保持未完成。会话请求引用已在全局句柄锁内取得，避免与 CloseHandle 并发释放的竞态。

IPC 复审发现的描述符对齐与普通参数截断问题已修复，新增特殊头、PID 与 X descriptor 的实际消息测试。当前只验收启动所需的无缓冲 NIFM domain 路径；输入对象、输出 pointer-size table、`CopyFromCurrentDomain` 未实现，不能称为通用 domain 支持。第三次启动只实际走到 cmd5，cmd18/21 尚仅有独立测试证据。

第四次启动接入 Mac 的三个触摸叶子函数后越过触摸初始化，继续创建游戏线程，随后在 `sdk+0x408060` 发生空指针读取。该地址属于 `nn::oe::EnterExitRequestHandlingSection`，原 SDK 从尚未建立的 AM 全局接口取得空 SelfController 后解引用，并非触摸缓冲区越界。触摸窄改已通过独立审查，项目 24 项测试通过；实际触摸读取函数仍未运行到。日志为 `work/acnh/3.0.0/boot-fourth.log`。

退出区间已按真实 `LockExit`/`UnlockExit` 计数语义适配并通过受控范围的独立复审。关窗、菜单 Cmd-Q 和系统 Quit 都会发送退出请求（消息 4），队列满时在消息消费后补投。2026-10-23 起按 Switch 关闭软件的语义结束进程：游戏在退出请求处理区内就等它离开，不在区内则直接结束；结束前另等宿主存档空闲，最多 15 秒。客体收尾（SVC `0x07`、模块 finalizer）不执行，Switch 关闭软件时同样不执行。同日实机验证：关窗时游戏在处理区内完成一次存档提交（22:26:36），离开处理区后进程以存档空闲状态结束，无残留事务标记。用户确认退出重启后读档正常，关闭按钮可正常关闭；随后再次启动，越过读档阶段，没有中止，用户确认关窗前的存档内容都在。用户确认画面、音频、键盘和鼠标操作正常。2026-10-24 用户把系统日期改到 24 日后重新启动，游戏直接读取宿主墙钟（无时间偏移），启动无中止；同日修复设置存档备份时 `nifm` 命令21未实现导致的中止。同日用户确认新一天的游戏内流程正常，设置存档备份不再中止。2026-10-24 独立审查确认上轮应修均已落地，C 关键路径（列用户空槽清零、olsc 备份开关、nifm GetAppletInfo、退出等存档、手柄颜色未连接）无阻断；新鲜全量测试 120 项通过、1 项默认跳过。阶段 C 勾选。这不等于纹理正确、完整可玩或 B2 的未实现清单已完成，详见 [阶段 C 证据](stage-c-evidence.json)。

第五次启动越过退出区间和本地账户初始化，启动 CoreAudio 48 kHz 双声道输出及 REV15 音频渲染器，随后在 `OpenDirectory(content:/Rumble)` 因缺少 RomFS 目录列举而停止；尚未证明实际出声。项目 25 项测试通过，日志 `work/acnh/3.0.0/boot-fifth.log`。下一步按真实 RomFS 元数据接入只读目录枚举，不能用空目录替代实际内容。

第六次启动已成功打开 `content:/Rumble`（33 个直接文件）并在游戏进程中逐项读取，随后因共享字体 `pl:u` 服务未实现而停止。目录实现按真实镜像校验全部 243 个目录、103,396 个文件与运行时索引的一一对应；空目录、过滤、分页和 NoFileSize 已通过合成测试与独立审查，真实镜像探针也通过。完整测试运行 27 项，26 项通过、1 项需显式指定大镜像路径的测试默认跳过（已单独运行通过）。日志 `work/acnh/3.0.0/boot-sixth.log`；B2 仍未完成。

第七次启动已在游戏进程内加载并解码六份系统字体（17,381,312 字节），越过原 `pl:u` 请求失败，随后因 `friend:u` 未实现而停止。当前实际只观察到字体请求/加载，尚未走到字体列表和文字绘制；字体 API 仅提供有明确边界的 en-US 兼容顺序，详见[字体依据](acnh-shared-font-evidence.md)。完整测试运行 28 项，27 项通过、1 项默认跳过；字体窄改独立审查和真实字体探针均通过。日志 `work/acnh/3.0.0/boot-seventh.log`，首帧仍未验收。

第九次启动已实际完成 `friend:u` 命令 0，创建 `IFriendService` 子对象并继续启动线程；尚未观察到好友子命令，随后在 `bcat:u` 打开处停止。生产 IPC 分发测试核验对象返回、路由及关闭后失效，窄范围独立审查通过。完整测试运行 31 项，30 项通过、1 项默认跳过。日志 `work/acnh/3.0.0/boot-ninth.log`；B2 保持未完成。

第十次启动已打开 `bcat:u`，在 domain 根对象上观测到命令 0、8 字节参数及 send-PID 标识，无数据缓冲区；诊断服务明确停止，尚未执行内容同步或缓存查询。完整测试运行 33 项，32 项通过、1 项默认跳过；诊断入口独立审查通过。日志 `work/acnh/3.0.0/boot-tenth.log`。

第十一次启动已创建 `IBcatService`，继续启动更多线程并提出性能模式请求，随后在 `nn::oe::GetRegionCode` 的 `set` 服务打开处停止。尚未观察 BCAT 子命令或同步。工厂窄改通过独立审查，完整测试运行 36 项，35 项通过、1 项默认跳过。日志 `work/acnh/3.0.0/boot-eleventh.log`；下一步复用基座已有地区设置服务，保持英文/USA 诊断基线。

第十二次启动已实际完成 `set` 命令 4（type 6、普通会话、8 字节 CMIF 尾部），随后触发原 SDK Break，reason `0x0`、result `0x74`，最靠近业务的 SDK 返回地址为 `sdk+0x43ed20`，主模块为 `main+0x9817d8`。保留原中止保护，继续定位真实初始化失败，不绕过断言。地区窄改通过独立审查，完整测试运行 42 项，41 项通过、1 项默认跳过。日志 `work/acnh/3.0.0/boot-twelfth.log`；仍未有游戏首帧。

第十三次启动已越过原时间初始化 Break，并继续到 `ngc:u` 服务打开处停止。时钟/时区实现和未支持逆转换的显式保护已独立审查，完整测试运行 54 项，53 项通过、1 项默认跳过。详见[时间依据](acnh-time-evidence.md)，日志 `work/acnh/3.0.0/boot-thirteenth.log`。尚未观察到游戏首帧，B2 保持未完成。

第十四次启动打开 `ngc:u` 后未观察到过滤命令，继续执行 VS/PS 运行时翻译、绘制及两次 NVN Present，随后因 `fsp-srv` 缺失停止。单独捕获运行保留前两帧 PNG，人工查看均为黑色，不能据此验收可识别的游戏首帧。诊断服务只记录请求结构并明确拒绝实际命令，独立审查通过；完整测试运行 56 项，55 项通过、1 项默认跳过。日志 `work/acnh/3.0.0/boot-fourteenth.log` 与 `boot-fourteenth-capture.log`。

第十五次启动已多次挂载独立 deviceSave，目录中尚无游戏保存文件；随后运行至至少第 31 帧，在 `sdk+0x1b01b4` 发生 fault 0 的 SIGSEGV（LR `sdk+0x1b01a4`）。捕获第 1、2、10、30 帧，人工查看均黑，尚未验收可识别游戏首帧。设备存档窄改经独立复核和完整 64 项测试（63 通过、1 默认跳过）；范围与并发限制见[设备存档依据](acnh-device-save-evidence.md)。日志 `work/acnh/3.0.0/boot-fifteenth.log`。

第十五次空指针已定位为 ListAllUsers 落入原 SDK 账号服务；ACNH 映射到持久本地账户后，第十六次运行越过此处，随后在 `olsc:u` 打开处明确停止。账户窄改独立复核通过，完整测试 66 项（65 通过、1 默认跳过）。捕获第 1、2、10、30 帧，经 CoreGraphics 解码 RGB 最小/最大值均为 0，非黑像素数为 0；首帧仍未验收。日志 `work/acnh/3.0.0/boot-sixteenth.log`，像素统计 `frame-sixteenth-stats.jsonl`。

第十七次诊断运行在 `olsc:u` domain 根对象观测到 cmd0 Initialize：type6、object1、token0、8 字节数据、发送PID标识、无缓冲区或copy/move句柄，随后按设计明确停止。完整测试 68 项（67 通过、1 默认跳过），诊断窄改独立审查通过；第 1、2、10、30 帧像素统计仍全部黑色。日志 `work/acnh/3.0.0/boot-seventeenth.log`。离线设置查询仍待适配和实际运行验收。

第十八次运行已实际通过 OLSC 离线初始化及本地设置查询，随后触发图形后端的 `TOTK_GPU_MIDCLEAR` 保护，在执行绘制中的深度／模板清除前明确停止。完整测试 70 项（69 通过、1 默认跳过），OLSC 窄改独立审查通过。第 1、2、10、30、60 帧像素统计全部为黑色。下一步先运行现有颜色与深度／模板清除的独立 GPU 正负控，再决定是否启用对应路径；B2 保持未完成。日志 `work/acnh/3.0.0/boot-eighteenth.log`。

第十九次保持 GPU 门禁开启，记录首次需绘制中清除的实际参数：768×768 Depth32Float、左上512×512 scissor、只写深度1，此时没有活动编码器。现有两组独立 GPU 正控共15,360项检查及两组负控均符合预期；尚缺该纯深度格式和清除后继续绘制的覆盖，详见[GPU 清除依据](acnh-gpu-clear-evidence.md)。首例日志独立复核通过，捕获画面仍全黑，B2 保持未完成。

第二十次在仅本轮环境启用已验证的 GPU 清除路径后，实际越过原门禁，继续更多着色器翻译与绘制；随后在 NIFM 命令4（type6、4字节、无缓冲区）明确停止。新增同编码器清除后绘制探针2,955,264项检查、颜色探针9,216项以及三组负控均符合预期，独立复核通过。第1、2、10、30、60帧仍全黑；默认启动保护未改，B2 保持未完成。日志 `work/acnh/3.0.0/boot-twentieth.log`。

针对第二十次的 NIFM 命令 4，已按 SDK 二进制与协议资料接入 `RequirementPreset=2` 的 `IRequest` 对象创建，并以生产 IPC 分发链测试创建、路由及关闭；未观测到的子命令仍明确失败。证据与范围见 [NIFM 请求创建依据](acnh-nifm-evidence.md)。尚待下一次真实启动确认子命令及后续行为，NIFM 功能和 B2 均未完成。

第二十一次已实际通过 NIFM CreateRequest，随后在 IRequest 对象3收到命令2（type6、domain、token0、无参数/PID/缓冲区/句柄），按诊断设计停止。创建窄改经真实调用顺序修正与独立复核，完整71项测试（70通过、1默认跳过）。补充捕获第100、105、106帧后，全部截图仍为 RGB0；尚未出现可识别首帧。下一步按实际协议接入两个可读事件及服务写端的释放，不伪造请求获准或联网成功。日志 `work/acnh/3.0.0/boot-twenty-first.log`。

第二十二次已实际通过 IRequest 命令2，SDK取得两枚独立可读事件，随后在命令11（type6、domain对象3、token0、1字节、无PID/缓冲区/句柄）停止。事件所有权与锁顺序经独立复核，带copy handle响应的结果码测试偏移缺口已修复并以非零Result合成样本防回归；完整72项测试（71通过、1默认跳过），包含生产线程事件5,000次回收循环。当前截图到第100帧仍RGB0，B2未完成。日志 `work/acnh/3.0.0/boot-twenty-second.log`。

第二十三次已实际通过 IRequest 命令11，参数4（Required），随后在命令4 Submit（type6、domain对象3、token0、无参数/PID/缓冲区/句柄）停止。连接确认选项仅以原子字段保存，有效枚举1..5；没有提交或触发事件。独立审查发现并修复负控仅凭拒绝日志可能假阳性的问题，完整72项测试（71通过、1默认跳过）。捕获画面像素仍全部为黑色，B2保持未完成。日志 `work/acnh/3.0.0/boot-twenty-third.log`。

第二十四次实际通过网络 Submit、两轮状态和结果查询，SDK读到state1和本地离线策略Result `0x8986e`后继续运行，未进入OnHold等待；随后在另一处 `fsp-srv` 打开处停止（SDK `+0x265380`，main `+0x3b0a0e4`）。离线生命周期经独立审查，完整72项测试（71通过、1默认跳过）。捕获第1、2、10、30、60、100、105、106、110、120帧全部RGB0，B2仍未完成。日志 `work/acnh/3.0.0/boot-twenty-fourth.log`。

第二十五次已通过 BCAT 本地 delivery cache 缺失路径，挂载接口返回 `0x47a` 后继续运行至至少第150帧；随后在 `sdk+0x408894` 空指针读取发生 SIGSEGV（main最近调用者 `+0x721720`）。无参挂载窄改经独立审查，完整73项测试（72通过、1默认跳过）。第1至150帧的11张捕获图均RGB0，仍未验收首帧。日志 `work/acnh/3.0.0/boot-twenty-fifth.log`。

第二十六次定位该空指针为 `nn::oe::GetPseudoDeviceId`：它调用 `nn::am::GetApplicationFunctions()`，应用接口全局为空后读取虚表 `+0xe8`。ACNH 改为写入持久的 16 字节本地标识，不创建 AM 对象。启动已加载该标识，随后在 `nn::hid::GetNpadJoyAssignment` 触发原 SDK Break，Result `0x58cca`（hid 模块 202、描述 710）。该公开接口在共享内存槽为空时中止。完整 79 项测试（78 通过、1 默认跳过）。第 1 至 150 帧的 11 张捕获图均 RGB0。日志 `work/acnh/3.0.0/boot-twenty-sixth.log`。

第二十七次对玩家、Other 和 Handheld 的 Joy 分配返回 Dual。实际命中 id `0x0`。进程进入稳定绘制循环并产生非零音频，诊断时限于约第 4800 帧停止，没有新的 SDK Break。完整 80 项测试（79 通过、1 默认跳过）。第 1 至 600 帧的 16 张捕获图仍全部 RGB0，更晚的帧没有截图，可识别首帧仍未验收。日志 `work/acnh/3.0.0/boot-twenty-seventh.log`。

第二十八次起在诊断环境打开 `TOTK_GPU_ALLOW_SUBMIT=1`（Makefile 的缺省启动仍不打开；这是为了避开此前会触发内核恐慌的提交）。GPU 时间从 0 变为约 7 ms，离屏颜色目标在第 600 帧已有机场柜台画面，但交换链截图仍全黑。拷到交换链的那次绘制采样这张画面，六个顶点的 clip z 都是 -1。NVN 缺省近平面是 -w，Metal 只保留 0 到 w，整块三角形被裁掉，窗口停在清除色。交换链通道没有深度附件，这次绘制改为把 clip z 夹到近平面。

夹近平面之后，第 600 帧的交换链截图是机场柜台、豆狸兄弟和对话「Good afternoon! We're so excited to have」，已对照画面目视核对。诊断时限停在约第 2400 帧，没有新的 SDK Break。日志 `work/acnh/3.0.0/boot-clipz.log`，截图 `work/acnh/3.0.0/frame-clipz-600.png`。触达的未实现 SVC/IPC/NVN 已整理为 [B2 清单](acnh-b2-unimplemented-inventory.md)；复选框须独立审查与新鲜验证后才能勾选。

## iPhone D 阶段进度（未验收）

- 已增加 `games/acnh/ios` 的 UIKit/Metal 工程、ACNH 专用 iOS 构建脚本，以及验证版 loader 的 `export-ios` 模式。
- iOS arm64 Opus/libvpx 依赖、loader 导出段和 906 个 ACNH Metal 着色器库均已生成；Xcode unsigned iOS 构建通过。完整 unsigned IPA 为 `work/acnh/3.0.0/ACNH-iPhone-unsigned.ipa`，SHA-256 `83a7a81948dcd23583d2529476a983735660a414204a7e6a0deec2180b74fb7b`，包含本机 3.0.0 RomFS、字体、NgWord2 资源和着色器。
- Xcode 自动签名分别以本机两个 Apple Development 团队重试，均因 Xcode 未登录该团队且没有 `org.macnativegames.acnh` 描述文件而失败。unsigned IPA 不可安装；D 阶段的签名安装、首次启动、触控/手柄、存档、前后台恢复和资源验收仍未完成。
- 开发者账户配置后，使用 `games/acnh/ios/tools/build_ipa.py --team-id <已登录团队 ID>` 生成设备签名 IPA，再按 D 门禁逐项真机验证。D 复选框保持未勾选。
- 2026-10-25 独立审查后，资源改为 Xcode folder resource 并由 Xcode 最终签名；打包脚本不再复制资源后重签，多团队时必须指定 `--team-id`。运行时已连接 NgWord2 资源路径，存档/缓存移入私有 `Application Support`，触控失焦会清零；后台存档等待有 task/epoch/generation 保护并通过最新 arm64 unsigned 构建。构建产物 `work/acnh/3.0.0/build/ios-derived-unsigned-review/Build/Products/Release-iphoneos/ACNH.app` 已核对包含 RomFS 和 NgWord2 common 表；仅有 `UIRequiresFullScreen` 弃用警告，未签名、未安装。
- 首次人物名/岛名输入仍待验证：ACNH main 没有 `swkbd`/applet 动态导入，不能仅凭 SDK 存在 `ShowKeyboard` 就安全接入；需在真实新岛命名路径捕获调用并核实 ABI。D 保持未勾选。
- 2026-10-25 已使用团队 `XLC3W3C265` 完成设备签名和 iPhone 更新安装。修正 iOS profile 相对路径后，首次启动进入移居套餐柜台对话，约 20 秒后进程仍存活，日志确认 `deviceSave` 挂载。命名、实际存档回验及前后台/锁屏恢复尚未完成。证据：`work/acnh/3.0.0/ios-package-logs/device-after-profile-relative-first-launch-20261025T0305Z.{log,png}`。
- 同日已合并 AppIcon 与保持原比例的柔化背景填边；本机 Metal 离屏检查 3/3、Release 构建、签名及 IPA CRC 校验通过。签名包 `work/acnh/3.0.0/ACNH-iPhone-signed-XLC3W3C265-20261025T0526Z-icon-backdrop.ipa` 已更新安装，真机主屏确认动森图标，柜台对话画面确认左右柔化暗化填边、前景完整、屏幕手柄可见。截图和运行日志在 `work/acnh/3.0.0/ios-ui-checks/{icon-home.png,game-backdrop.png,device-backdrop.log}`。独立审查未发现功能性缺陷；Metal4 的上下留边、非零 crop、MetalFX 组合尚无自动化回归覆盖，不能由本次横屏画面推定通过。
- 2026-10-25 按用户要求暂停画面调整，优先排查发热。旧包 Metal 跟踪约 16.7 秒内有 10,949 次约 1 ms 的额外 Compute 事件，累计 10.934 秒，与默认启用的 GPU 保活自旋核及日志相互印证；该核不产生游戏数据。ACNH iOS 入口现默认设置 `TOTK_GPU_KEEPALIVE=0`，保留显式 `=1` 对照，同时接通系统热状态读数（0 正常、1 Fair、2 Serious、3 Critical，非摄氏度）。没有改变画面、分辨率或目标帧率。独立审查、Release 构建、签名及 IPA CRC 通过；`work/acnh/3.0.0/ACNH-iPhone-signed-XLC3W3C265-20261025T0623Z-heat.ipa` 已更新安装，正常启动的 Metal 跟踪确认上述保活事件为 0，柜台稳态仍约 60 FPS。
- 发热证据在 `work/acnh/3.0.0/ios-heat-checks/`：`keepalive-metal-comparison-summary.{json,md}`、`no-keepalive-cpu-profile-summary.{json,md}`、`candidate-runtime-validation.json` 和原始 trace/日志。CPU 采样未证实可安全删除的忙等热点，不能仅凭线程条目数缩池。拔线后最初 5 分钟的 65 个 300 帧窗口，其帧时间 p50 中位数为 16.67 ms，热状态一度由 Serious 回落到 Fair；约 8 分钟时又回到 Serious，用户仍报告明显发热，因此第一轮未完成发热验收。玩家已从柜台推进入岛，后段绘制量增大，不能把整段当作固定场景对照。Power Profiler 保存失败，没有瓦特或摄氏度证据，不能量化降温或节能收益。下一步核对 HLE 的 60 Hz 逻辑时钟与 Metal 显示链实际 120 Hz 的节拍基准；在确认进度可安全保存和重启前，不执行启动参数对照。D 整体验收保持未完成。
- 节拍复核已通过本机源码探针复现不一致：物理 120 Hz、mode 1、游戏 interval=2 时，显示链两拍为 16.67 ms，但 HLE 周期为 33.33 ms；显式 `DM_PRESENT_HZ=60` 后两者一致。见 `work/acnh/3.0.0/ios-heat-checks/clock_policy_probe.{py,log}`。这只验证源码节拍模型，不证明真机游戏速度、输入、音画或降温结果；尚未永久修改节拍默认值，也未重启当前入岛进程。

2026-10-23 恢复现场后发现后续未提交改动及户外集合截图；旧诊断上下文不再作为当前停止点。保存事务和服务误报成功问题已修复。阶段 C 于 2026-10-24 勾选，见 [阶段 C 证据](stage-c-evidence.json)。B2 清单已写出，复选框未勾选。2026-10-24 打出本机离线 Mac App 原型：`work/acnh/3.0.0/Animal Crossing New Horizons.app`，Ad-hoc 签名，`--check` 通过；不含 keys，不上架。打开 App 进岛后因 `IParentalControlService` 命令 1（`Initialize`）未实现退出，与读档无关；已按空请求形状返回结果 0 并重新打包。D（iPhone）当时仍未开始；当前进度见上节。
