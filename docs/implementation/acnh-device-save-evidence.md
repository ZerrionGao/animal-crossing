# ACNH 设备存档实现与验证

ACNH 在 `sdk+0xc23f4` 打开 `fsp-srv` 失败的历史证据指向 `nn::fs::MountDeviceSaveData(char const*)`（SDK `+0xc1930`，由 `main+0x25cf674` 调用）。这是设备级岛屿存档，与账户 UID 存档分开。更新 NSP 的 Control NCA 已通过 FS header 与六层 IVFC 校验，未验证 RSA 签名；`control.nacp` 的 SHA-256 为 `0bec90a6199fafa12a697d90e1605f17f91b20ab014208334aa13f66305657f2`，本地产物在 `work/acnh/3.0.0/analysis/control/`。

| NACP 字段 | 偏移 | 字节数 |
| --- | --- | --- |
| DeviceSaveDataSize | 0x3090 | 116391936 |
| DeviceSaveDataJournalSize | 0x3098 | 116391936 |
| DeviceSaveDataSizeMax | 0x3158 | 603979776 |
| DeviceSaveDataJournalSizeMax | 0x3160 | 603979776 |

字段定义见 [NACP](https://switchbrew.org/wiki/NACP)；[EnsureSaveData](https://switchbrew.org/wiki/Applet_Manager_services#EnsureSaveData) 说明启动器为声明存档的标题预建缺失存档。宿主实现保留初始容量 `0x6f00000`、上限 `0x24000000` 和元数据校验，但容量字段并非磁盘配额。

设备存档按物理 store 共享事务状态。`device-save/01006f8002326000` 是槽 0，隐藏的 `.01006f8002326000.slot1` 是槽 1；同名 `.meta` 为 56 字节 v2 元数据，保存当前活动槽、容量、恢复标记和校验值；`.01006f8002326000.txn` 是带校验的持久事务标记。旧 48 字节 v1 元数据在确认活动目录和第二槽安全后升级。任何缺槽、坏元数据、坏标记或符号链接均明确失败，已有不明文件不会被当成空存档覆盖。

首次写入前清空但保留非活动槽目录，从活动槽复制并同步复制结果，再原子发布事务标记；随后文件、删除、重命名和目录操作都落在 staging 槽。重复挂载共享这个槽；跨代仍打开的只读文件和目录句柄在读取时返回错误。提交拒绝未关闭的设备存档写句柄，只递归同步该槽的普通文件与目录，包括已经关闭的文件；`F_FULLFSYNC` 仅在 `EINVAL`、`ENOTSUP`、`ENOTTY` 时回退 `fsync`。全部同步通过后，原子替换 `.meta` 的 `active_slot` 是唯一提交点，随后清理标记。提交前退出保留完整旧树；提交点后退出保留完整新树。

重启若标记指向尚未提交的槽，则持久设置 `restore_pending=true` 并继续使用旧槽；若指向已提交槽，仅补清理标记。恢复标记不会因读取而清除，下一次成功提交才清除。这是宿主策略，不声称精确复刻 Horizon。元数据发布后的目录同步或标记清理失败会冻结新写入，后续 `Commit` 或卸载后重新挂载可继续完成收尾，避免覆盖旧槽。挂载分类到路径操作，以及 `Commit` 分类到分支执行期间会阻止卸载和重挂；卸载完成后，挂载名可由设备/非设备类型复用。`CommitOption` 接受 `u32=0` 与 `1`（ClearRestoreFlag，游戏在 `main+0x273c9e0` 使用；宿主每次成功提交本就清除恢复标记）；值 2（SetRestoreFlag）等其他值及多于一个挂载名的批量提交明确失败且不部分提交。`CleanDirectoryRecursively`（游戏存档步骤 `main+0x2734de0` 清空整个设备存档根）、`RenameDirectory`、`GetDirectoryEntryCount` 也由宿主实现：此前它们落入原 SDK，SDK 挂载表为空，`IsAbortNeeded` 后经 `AbortImpl` 触发 Break。清空根目录同样先进入 staging 槽，提交前活动槽不变。`ExtendDeviceSaveData` 只在无事务、无设备写句柄时做上限内扩容。普通账户存档仍沿用原有路径，设备提交不会清掉账户存档的写入忙状态。

2026-10-23 的定向验证：先用生产入口探针复现“提交前活动目录被写”的旧问题，再修复到通过。`python3 -m unittest tests.test_acnh_device_save` 通过 18 项测试；其中故障矩阵覆盖清槽后与复制后退出、复制 ENOSPC、文件/目录同步、写入同步、标记/元数据 rename 与目录同步、进程在提交点前后 `_exit`，并比较重启后的完整目录树。另覆盖发布后同步失败的进程内重试、卸载后重挂恢复及新写拒绝、挂载类型交换竞态、超过 128 个顺序挂载名及闲置名称跨类型复用、重复挂载、跨代句柄、普通存档隔离、v1 升级、坏标记、坏槽和符号链接。生产宏 `GAME_ACNH` 的 `xcrun clang -std=c11 -Wall -Wextra -Werror -fsyntax-only` 通过；测试故障点仅在 `ACNH_SAVE_TESTING` 编译。

这些验证是独立测试 profile 上的进程退出与注入故障测试。独立代码审查已通过，并复跑 18 项测试、ACNH/TOTK/BOTW 三套生产宏编译、挂载竞态与重挂恢复各 10 次。尚未做断电或真实游戏内多轮保存验收。单进程受控 profile 是当前边界：逐级路径检查与操作之间仍可能被外部并发改动，不能将此实现当作对抗恶意同机进程的沙箱或多实例存档服务。
