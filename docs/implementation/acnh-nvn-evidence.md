# ACNH 3.0.0 NVN 版本与深度模式证据

本文记录 `work/acnh/3.0.0/original-analysis/main.elf` 的静态反汇编证据。该 ELF 是原始 `main` 的分析载体；Build ID 为 `5d913cf71eb24cb22c5105b1fa3efd97`，SHA-256 为 `0b7c72ef7812f820c508550d75fd2ec8ae51b54c971e3be8f372d3651b7d8524`。

## NVN API 版本

`main+0x24929c` 的 `ADRP x1` 与 `main+0x2492a0` 的 `ADD x1, x1, #0x501` 将字符串地址设为 `0x3fad501`。原始 ELF 中该地址的字符串为 `nvnDeviceGetInteger`；后续经函数指针调用并把结果存入 bootstrap 函数表。

版本检查位于 `main+0x37b65b8..0x37b65e4`：两次间接调用分别以 `w1=0` 和 `w1=1` 查询 major 与 minor；随后 `main+0x37b65ec` 执行 `CMP w8, #0x37`，`main+0x37b65f0` 在不等时跳至 `main+0x37b6f1c`；`main+0x37b65f8` 执行 `CMP w8, #0xf`，`main+0x37b65fc` 在小于时跳至同一拒绝路径。因此该版本要求 major 55、minor 至少 15。`hle_nvn.c` 的 ACNH `g_devinfo[0]`、`g_devinfo[1]` 据此返回 55 和 15。

## 深度模式

`main+0x249398..0x2493ac` 将字符串地址设为 `0x3f83d2c`，原始 ELF 中该字符串为 `nvnDeviceSetDepthMode`；下一组 `main+0x2493bc..0x2493c0` 把解析到的函数指针写入函数表槽 `0x4b60d20`。这证明 ACNH 会解析该 NVN 接口，尚未证明实际调用时序或调用次数。

不要把 `main+0x37c157c..0x37c1594` 的 `mov w1, #1` 解释为深度模式设置：该处通过不同的函数表槽 `0x4b60d18` 间接调用，属于 `nvnDeviceGetTexelFetchHandle` 路径。深度枚举值 1 对应 `NVN_DEPTH_MODE_NEAR_IS_ZERO`，但现有证据不足以据此认定 ACNH 实际设置了该值。当前 `hle_nvn.c` 的深度模式初始值保持原样；游戏实际调用 setter 时仍由 setter 更新状态。
