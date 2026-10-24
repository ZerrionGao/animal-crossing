# B2 未实现 SVC / IPC / NVN 清单

日期：2026-10-24。游戏 ACNH 3.0.0。基座 MacNativeGames `dfa7ec426f8b37798073a108ede1504c827d8af0`。

B2 定义为：真实进程启动并画出首帧，并列出**实际触达**、仍不是完整 Horizon / NVN 语义的调用。本清单不把离线失败当成「已实现任天堂服务」，也不把从未调用的报红桩当成已经踩过的缺口。未知命令仍走 `ipc_unimplemented` / `runtime_fail`，不默认成功。

复选框仍未勾选：须独立审查与新鲜验证后才能勾选 B2。本文件不等于纹理正确、完整可玩或 iPhone；云备份明确不在项目范围。

## 证据范围

| 项 | 路径或结论 |
| --- | --- |
| 首帧 | `work/acnh/3.0.0/frame-clipz-600.png`（机场柜台），日志 `work/acnh/3.0.0/boot-clipz.log` |
| 阶段 C 游玩 | `work/acnh/3.0.0/boot-first-save.log`，至帧 22800，`FAIL` / `not implemented` / `Break` / `SIGSEGV` 均为 0 |
| 历史启动 | `work/acnh/3.0.0/boot*.log`（含首次因缺少服务而明确停止的记录） |
| 运行时失败口 | `runtime_svc` 默认分支、`ipc_unimplemented`、各离线服务 `runtime_fail`、`nvn_unimpl_entry` |
| NVN 槽表 | 运行时「名字 523，已实现 413（语义 408 + 空实现 5），其余 110 个接报红桩」 |

## 1. SVC

全部启动与游玩日志中 **0 次** `unimplemented SVC`。命名端口只见到 `sm:`。未知 `GetInfo` id 未出现。

未实现 SVC 被调到仍会打印编号并 `runtime_fail("system service not implemented")`。当前游玩路径没有撞上这条。

客体退出（SVC `0x07`、模块收尾）按 Switch 关闭软件语义故意不跑，见实施记录；这不是「触达后未实现」，而是关窗时不执行。

## 2. 本次游玩实际打开的 IPC 服务

`boot-first-save.log` 打开过：

`nifm:u`，`audren:u`，`friend:u`，`bcat:u`，`set`，`ngc:u`，`olsc:u`，`pctl`，`pctl:s`，`pctl:a`，`pctl:r`。

登机影片日志另打开过 `audout:u`（本次读档会话未再打开）。

下列历史上 `sm: service "…" not implemented` 的名字，现已改为函数接管，**不再作为 sm 服务打开**：`hid`（输入覆盖）、`pl:u`（共享字体覆盖）、`fsp-srv`（宿主 RomFS / 存档）。再次向 sm 要这些名字仍会明确失败。

## 3. 已触达、但仍不是完整 Horizon 的 IPC

这些命令在真实启动里被调用过。宿主返回离线结果或子集行为，游戏继续跑；不是任天堂在线语义。

| 服务 | 触达命令 | 宿主做法 | 证据 |
| --- | --- | --- | --- |
| `nifm:u` / `IRequest` | 工厂 cmd 5；General cmd 4 `CreateRequest`（preset 2）；IRequest cmd 2 事件句柄、11 确认选项=4、4 Submit、0 状态、1 结果、21 `GetAppletInfo` | 离线失败：Submit 后 `0x8986e`，提交前 `0x26e6e`；只通知第一枚事件；无系统错误界面 | 游玩日志 12 次状态、12 次结果、6 次 Submit、1 次 GetAppletInfo |
| `nifm` General | cmd 18 连接状态、cmd 21 是否批准请求 | 无网卡 / 不批准 | 代码路径；游玩中随备份设置走到 GetAppletInfo |
| `friend:u` | cmd 0 创建 `IFriendService`；随后 `nn::friends` 名单 / 屏蔽 / 在线会话声明 | 名单为空；需等待的在线操作为 `0xc79` | `[acnh-friend] 离线，好友名单为空` |
| `bcat:u` | cmd 0 创建服务对象；`MountDeliveryCacheStorage` | 本地无分发缓存，`0x47a` | 游玩日志 |
| `olsc:u` | cmd 0 初始化；cmd 14 关备份；cmd 13 读开关 | 只记本进程，无云备份 | 游玩日志「kept on this Mac only」 |
| `ngc:u` | cmd 2 Mask | `not_b` / `b1` 子串替换为 `*`；无 similar-form、无分隔短语 b2、无 NFKC | 命名路径打开 `ngc:u`；词库子集见适配说明 |
| `pctl*` | 工厂 cmd 1；服务 cmd 1、1018、1001 | 结果 0，SDK 视为可通信；不是家长控制策略 | 游玩日志；本机 App 进岛后 cmd 1 |
| `set` | cmd 4 地区、cmd 0 语言 | 地区固定 USA1；语言与 `GetDesiredLanguage` 相同，默认 zh-Hans | 游玩日志 cmd 4 |
| `audout:u` | List/Open、追加、开始、停止、缓冲事件 | 登机影片 PCM；未知命令仍失败 | `boot-layer-*.log` 等登机日志 |
| `account`（函数覆盖） | `ListAllUsers` / 网络账号 | 空槽清零；网络账号离线不可用 | 阶段 C 读档路径 |
| `hid`（函数覆盖） | `GetNpadControllerColor` 等 | 无控制器 `0x4b8ca`，默认颜色 | 游玩日志 no-controller |

`IFriendService` / `IBcatService` 的 **IPC 子命令** 仍是 `runtime_fail`。后期游戏改走 SDK 函数覆盖，游玩日志不再打到这些子命令。再打到仍会停。

## 4. 历史上触达、现已接上的 IPC（不再是当前停止点）

启动日志里曾经 `sm:` 或 `command … not implemented`，后来按观测形状接上。保留以免把「曾经缺」和「现在仍缺」混在一起。

| 当时停止点 | 日志 | 现状 |
| --- | --- | --- |
| `nifm:u` 未注册 | `boot-first.log` | 已注册离线 |
| IPC domain / control cmd 0 | 早期 `boot-*.log` | domain 可用 |
| `hid` | `boot-third.log` | 函数覆盖，不注册 sm |
| `pl:u` | `boot-sixth.log` | 共享字体覆盖 |
| `friend:u` | `boot-ninth.log` | 工厂 cmd 0 + 离线名单 |
| `bcat:u` | 第九次之后 | 工厂 + 缓存缺失 `0x47a` |
| `set` | 早期 | cmd 0/4 |
| `ngc:u` | `boot-thirteenth.log` | Mask cmd 2 |
| `olsc:u` 及后来的 cmd 14 | 第十八次前后；读档备份崩溃 | cmd 0/13/14 离线 |
| RomFS 列目录 | 一次 `RomFS directory listing not implemented` | 已建只读索引 |
| `nifm` IRequest cmd 2 / 11 / 4 / 21 | `boot-twentieth` … 备份设置 | 见上一节离线策略 |
| `pctl*` | 登机影片结束；本机 App 进岛 | 1/1001/1018 |

## 5. 已触达的 NVN / Metal 缺口

空设备会加载 main 的 523 个 NVN 名字。被调到的未实现桩会打印 `NVN 未实现被调用` 并中止。首帧与阶段 C 游玩 **0 次** 该行。

### 5.1 调用了、但宿主语义不完整

| 项 | 触达情况 | 宿主做法 |
| --- | --- | --- |
| 交换链 clip z | 首帧：近平面 -w，六个顶点 clip z = -1，按 Metal 0..w 会整块裁掉 | 无深度附件的交换链绘制把 clip z 夹到近平面；游戏若改近平面为 0 则不再夹。游玩日志仍打印该行 |
| 预排布贴图不在内存池 | `boot-first-save.log` 第 310 行一次【红】 | 回退 CPU 解排布 |
| `nvnColorStateSetLogicOp` | 初始化与前 3 帧已调用 | 记录状态。Metal 不实现非 COPY 逻辑运算 / 非 ALWAYS alpha test，那种绘制会去掉该测试。四份登机/分层日志打印过 `logic op 3 / alpha test 7`；本次读档会话未再打印 |
| `nvnCommandBufferSetShaderScratchMemory` | 前 3 帧调用表中出现 | **空实现**（`g_nops`），不计 GPU 语义 |
| 驱动着色器缓存 | 至帧 22800：`drv-cache hit=0 miss=31211 verified=0` | 未命中、未核对 |
| 贴图计划 | `texture-plan enabled=1 … verified=0` | 有选择，无 GPU 结果核对 |
| 回放丢弃 | 至帧 22800：`clear-render-disabled=27825 cond-disabled=27825` | 条件渲染 / 清除通道关闭时跳过绘制 |
| `shader-libs` | ACNH 构建跳过按原始 MSL 哈希命名的预编译库 | 正规化失败则跳过该绘制；本次游玩日志未见「跳过该绘制」 |
| `nvnBufferFlushMappedRange` 截断 | 确认肤色时区间可超出缓冲 | 只刷新仍在缓冲和池内的一段。本轮 boot 日志未再打印截断行 |
| `nvnDeviceSetDepthMode` | 静态会解析；运行与旷野之息一样未调用 setter | 保持 NVN 缺省近平面 -w |

其余 4 个空实现（`nvnCommandBufferSetPatchSize`、`WaitSync`、`WaitEvent`、`nvnWindowFinalize`）未出现在初始化与前 3 帧调用表。更晚的帧只打函数个数，不能断言有没有调用；若从报红桩改成空实现，被调用也不会停。

### 5.2 已加载、本轮从未调用的 110 个报红桩

运行时计数为 110。源码 `NVN`/`NVN_NOPROTO` 与名字表相减得到 109 个名字（swnvn 段 408 项，与源码宏差 1，不影响「均未被调用」：调用任一报红桩都会中止）。按前缀分组：

**设备 / 队列**

- `nvnDeviceBuilderGetFlags`
- `nvnDeviceSetIntermediateShaderCache`
- `nvnDeviceGenerateDebugDomainId`
- `nvnDeviceGetWindowOriginMode`
- `nvnDeviceGetDepthMode`
- `nvnDeviceApplyDeferredFinalizes`
- `nvnDeviceWalkDebugDatabase`
- `nvnDeviceSetAutomaticCheckpointsEnable`
- `nvnDeviceSetAutomaticCheckpointsSamplingInterval`
- `nvnDeviceSetAutomaticCheckpointsByDebugGroup`
- `nvnDeviceSetCheckpointCallStacksEnable`
- `nvnDeviceSetCheckpointMaxCallStacksDepth`
- `nvnQueueGetTotalCommandMemoryUsed`
- `nvnQueueGetTotalControlMemoryUsed`
- `nvnQueueGetTotalComputeMemoryUsed`
- `nvnQueueResetMemoryUsageCounts`
- `nvnQueueWaitSync`
- `nvnQueueBuilderSetQueuePriority`
- `nvnQueueBuilderGetQueuePriority`
- `nvnQueueBuilderGetDevice`
- `nvnQueueBuilderGetFlags`
- `nvnQueueBuilderGetCommandMemorySize`
- `nvnQueueBuilderGetComputeMemorySize`
- `nvnQueueBuilderGetControlMemorySize`
- `nvnQueueBuilderGetCommandFlushThreshold`
- `nvnQueueBuilderGetMemorySize`
- `nvnQueueBuilderGetMemory`

**程序 / 池 / 缓冲 / 贴图**

- `nvnProgramSetSampleShading`
- `nvnProgramSetSubroutineLinkage`
- `nvnMemoryPoolBuilderGetDevice`
- `nvnMemoryPoolBuilderGetMemory`
- `nvnMemoryPoolBuilderGetSize`
- `nvnMemoryPoolBuilderGetFlags`
- `nvnTexturePoolRegisterImage`
- `nvnTexturePoolGetMemoryPool`
- `nvnTexturePoolGetMemoryOffset`
- `nvnTexturePoolGetSize`
- `nvnSamplerPoolRegisterSamplerBuilder`
- `nvnSamplerPoolGetMemoryPool`
- `nvnSamplerPoolGetMemoryOffset`
- `nvnSamplerPoolGetSize`
- `nvnBufferBuilderGetDevice`
- `nvnBufferBuilderGetMemoryPool`
- `nvnBufferBuilderGetMemoryOffset`
- `nvnBufferBuilderGetSize`
- `nvnBufferGetDebugID`
- `nvnTextureBuilderSetGLTextureName`
- `nvnTextureBuilderGetPackagedTextureLayout`
- `nvnTextureBuilderGetSparseTileLayout`
- `nvnTextureBuilderGetGLTextureName`
- `nvnTextureBuilderGetRawStorageClass`
- `nvnTextureGetSparseTileLayout`

**混合 / 多重采样**

- `nvnBlendStateGetAdvancedMode`
- `nvnBlendStateGetAdvancedOverlap`
- `nvnBlendStateGetAdvancedPremultipliedSrc`
- `nvnBlendStateGetAdvancedNormalizedDst`
- `nvnMultisampleStateGetAlphaToCoverageDither`
- `nvnMultisampleStateSetCoverageModulationMode`
- `nvnMultisampleStateGetCoverageModulationMode`
- `nvnMultisampleStateSetCoverageToColorEnable`
- `nvnMultisampleStateGetCoverageToColorEnable`
- `nvnMultisampleStateSetCoverageToColorOutput`
- `nvnMultisampleStateGetCoverageToColorOutput`
- `nvnMultisampleStateGetSampleLocationsEnable`
- `nvnMultisampleStateGetSampleLocationsGrid`
- `nvnMultisampleStateSetSampleLocationsGridEnable`
- `nvnMultisampleStateGetSampleLocationsGridEnable`

**命令缓冲（含间接绘制、变换反馈、降采样）**

- `nvnCommandBufferBindTransformFeedbackBuffer`
- `nvnCommandBufferBindTransformFeedbackBuffers`
- `nvnCommandBufferSetInnerTessellationLevels`
- `nvnCommandBufferSetOuterTessellationLevels`
- `nvnCommandBufferBeginTransformFeedback`
- `nvnCommandBufferEndTransformFeedback`
- `nvnCommandBufferPauseTransformFeedback`
- `nvnCommandBufferResumeTransformFeedback`
- `nvnCommandBufferDrawTransformFeedback`
- `nvnCommandBufferDrawArraysIndirect`
- `nvnCommandBufferDrawElementsIndirect`
- `nvnCommandBufferMultiDrawArraysIndirectCount`
- `nvnCommandBufferMultiDrawElementsIndirectCount`
- `nvnCommandBufferClearColori`
- `nvnCommandBufferClearColorui`
- `nvnCommandBufferDispatchComputeIndirect`
- `nvnCommandBufferSetViewportSwizzles`
- `nvnCommandBufferClearBuffer`
- `nvnCommandBufferClearTexturei`
- `nvnCommandBufferReportValue`
- `nvnCommandBufferDownsample`
- `nvnCommandBufferTiledDownsample`
- `nvnCommandBufferDownsampleTextureView`
- `nvnCommandBufferTiledDownsampleTextureView`
- `nvnCommandBufferFenceSync`
- `nvnCommandBufferSetCopyImageStride`
- `nvnCommandBufferGetCopyRowStride`
- `nvnCommandBufferGetCopyImageStride`
- `nvnCommandBufferDrawTexture`
- `nvnCommandBufferSetProgramSubroutines`
- `nvnCommandBufferBindCoverageModulationTable`
- `nvnCommandBufferResolveDepthBuffer`
- `nvnCommandBufferSetColorReductionEnable`
- `nvnCommandBufferSetColorReductionThresholds`
- `nvnCommandBufferSetStencilCullCriteria`

**同步 / 事件 / GL 互操作**

- `nvnSyncInitializeFromFencedGLSync`
- `nvnSyncCreateGLSync`
- `nvnEventBuilderGetStorage`
- `nvnEventBuilderGetMemoryPool`
- `nvnEventBuilderGetMemoryOffset`
- `nvnEventGetMemoryPool`
- `nvnEventGetMemoryOffset`

间接绘制、降采样、深度边界测试已从空实现里拿掉，真调用会停在报红桩。本轮未调用。

## 6. 若再触达会明确失败的口（尚未在当前游玩打到）

- 未注册的 sm 服务名
- 非 `sm:` 的命名端口
- 未知 SVC / 未知 `GetInfo` id
- `nifm` 未列出的命令或非 preset 2 / 非确认选项 4
- `olsc` 除 0/13/14 以外
- `ngc` 除 Mask cmd 2 以外
- `pctl` 工厂 cmd>1，服务除 1/1001/1018 以外
- `set` 除 cmd 0/4 以外
- `friend:u` / `bcat:u` 的 IPC 子命令
- `audout:u` / `audren:u` 未列出的命令
- 上述 110 个 NVN 报红桩

## 7. 本清单不覆盖

纹理像素正确性、完整可玩和 iPhone 不在本清单覆盖范围；云备份明确不在项目范围。本机离线 Mac App 原型见 `work/acnh/3.0.0/Animal Crossing New Horizons.app`，不等于上架包。NgWord2 的 similar-form 与 b2 未接线，不在本次游玩的 IPC 失败日志里，但 Mask 过滤仍是子集。
