# ACNH 设备存档接入依据

第十四次运行在 `sdk+0xc23f4` 打开 `fsp-srv` 失败。公开入口是 `nn::fs::MountDeviceSaveData(char const*)`（SDK `+0xc1930`），由 `main+0x25cf674` 调用；原后端只覆盖按账户 UID 挂载，不能代表 ACNH 的设备级岛屿存档。

本批范围为独立测试目录下的设备存档挂载和持久容量查询；恢复标志、扩容和两个新 Commit 重载先明确失败。普通 Commit 与多挂载或带选项的 Commit 不可混用。完整保存、崩溃恢复、日志事务和退出重启仍属未完成的 C 阶段。

更新 NSP 的 Control NCA（偏移 2960，大小 2,921,472 字节）已通过 FS header 和全部六层 IVFC 校验；未校验 RSA 签名。提取 `control.nacp` 长度 16,384 字节，SHA-256 `0bec90a6199fafa12a697d90e1605f17f91b20ab014208334aa13f66305657f2`。本地产物见 `work/acnh/3.0.0/analysis/control/`。

| 字段 | 偏移 | 字节数 |
| --- | --- | --- |
| DeviceSaveDataSize | 0x3090 | 116391936 |
| DeviceSaveDataJournalSize | 0x3098 | 116391936 |
| DeviceSaveDataSizeMax | 0x3158 | 603979776 |
| DeviceSaveDataJournalSizeMax | 0x3160 | 603979776 |

字段定义参见 [NACP](https://switchbrew.org/wiki/NACP)。[EnsureSaveData](https://switchbrew.org/wiki/Applet_Manager_services#EnsureSaveData) 描述启动器按 control.nacp 预建不存在的存档，因此首次测试 profile 可以按上述初始容量建独立设备存档；已有不明数据不能猜测容量或覆盖。这里的配额声明不等于宿主磁盘剩余空间，也不证明已有真实 Switch 文件系统日志。

已实现 ACNH-only MountDeviceSaveData 与 GetDeviceSaveDataSize。独立挂载根为测试 profile 的 `device-save/01006f8002326000`；48 字节容量元数据位于根外的同名 `.meta`，带版本、应用 ID、容量与校验值。首次确保根为空再发布元数据，重开严格读取；损坏、已有文件缺元数据或元数据存在但根丢失时明确停止。发布检查文件同步、原子 rename 和目录同步结果。

设备挂载沿用 M_SAVE 的写入忙状态。新增路径检查只对设备挂载启用，拒绝当前可观察到的祖先或子路径符号链接；调用旧 ready() 前也检查 profile/temp 等路径，避免清理外部目录。仅承诺单进程受控测试 profile：逐级 lstat 到实际文件操作仍存在 TOCTOU，元数据首次发布也没有跨进程序列化，不能作为对抗并发修改的沙箱或多实例存档后端。

独立审查先发现祖先目录替换的遗漏，修复后复核通过。8 项定向测试和 ACNH/BOTW/无游戏宏的严格语法检查通过；完整测试运行 64 项，63 项通过、1 项默认跳过。日志 `work/acnh/tests-device-save.log`。测试中的普通 Commit 只验证现有忙状态机制，不证明游戏保存事务和崩溃恢复已验收。
