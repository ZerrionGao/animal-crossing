# ACNH 伪设备标识与 Joy 分配

第二十五次启动在 `sdk+0x408894` 读取空地址。该指令属于 `nn::oe::GetPseudoDeviceId(nn::util::Uuid*)`（`sdk+0x408880`，64 字节）。函数先调用 `nn::am::GetApplicationFunctions()`；该函数读取 `0xBA19D0` 的应用接口指针，指针为空时直接返回。公开接口随即解引用返回值的虚表 `+0xe8`。`main+0x72171c` 通过过程链接项调用它，输出指针在 x19，不是空指针。

原函数成功时写入 16 字节 `nn::util::Uuid` 后返回，失败时走 SDK 中止，不把 Result 交还给调用方。ACNH 只替换这个叶子：在 `TOTK_PROFILE_DIR/pseudo-device-id` 保存 16 字节标识，空文件新建，已有内容原样返回，全零或长度不对则失败。它不创建 `IApplicationFunctions`，也不表示这是主机真实伪设备号。

第二十六次已加载该标识，然后在 `nn::hid::GetNpadJoyAssignment` 发生 SDK Break，Result `0x58cca`。公开函数接受玩家 0–7、`0x10` 和 Handheld `0x20`；内部 `GetNpadJoyAssignment` 在共享内存槽为空时返回 `0x58aca + 0x200`。公开函数把任何非零结果都中止。当前宿主没有填充这些槽，但已有一个完整手柄。因此该叶子对上述标识返回 Dual（0）。第二十七次实际查询的是 id `0x0`。

第二十七次之后没有新的服务中止。音频回调出现非零采样，帧循环持续到诊断时限的约第 4800 帧。第 1、2、10、30、60、100、105、106、110、120、150、180、240、300、450、600 帧的捕获图 RGB 最小/最大值均为 0。600 帧之后没有截图，不能据此判断更晚画面。可识别首帧、可玩性和存档仍未验收。

2026-10-23，首晚睡觉时白狗弹完吉他后，原 SDK 又一次中止，Result 仍是 `0x58cca`。这次来自 `nn::hid::GetNpadControllerColor`，调用点是 `main+0x8b5a6c`，前面紧接着在 `main+0x8b5a2c` 调用了 `SwapNpadAssignment`。按 `-DGAME_ACNH` 预处理后的覆盖表核对，主模块导入的 hid 公开接口中，还有 8 个 detail 函数没有宿主实现，而且失败就会中止：`SetNpadJoyAssignmentModeDual`、`SetNpadJoyAssignmentModeSingle` 的两个重载、`MergeSingleJoyAsDualJoy`、`SwapNpadAssignment`、`GetNpadControllerColor`、`GetPlayerLedPattern`、`GetSupportedNpadStyleSet`。这些现在都由宿主在 detail 层接管，公开接口的编号校验仍由原 SDK 执行。`GetNpadControllerColor` 返回 NoController（`0x4b8ca`）：公开接口原样返回这个结果，游戏改用默认颜色。`GetNpadJoyAssignment` 改为返回各编号记录的模式。验证覆盖 `tests/joy_assignment_probe.c` 和完整回归测试，游戏内这一段还需要实际重放确认。
