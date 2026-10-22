# ACNH 共享字体适配依据

第六次真实启动在 `pl:u` 未实现处停止：`sdk+0x411ebc → sdk+0x4130e4 → sdk+0xc80404`。主模块导入的 `RequestSharedFontLoad` 此前仅在 BOTW 构建注册，导致 ACNH 执行原 SDK 的服务路径；同样尚未覆盖的 `GetSharedFont` 也会打开该服务。

本次按叶子函数接入已有的真实六字体，保持原件只读：先校验已锁定的文件大小和 SHA-256，再解码内存中的 BFTTF 载荷。`BE32(header[0]) XOR 0x7f9a0218` 得到解码参数；`BE32(header[1]) XOR 参数` 是载荷长度，每个载荷 BE32 与该参数异或，结果须具有 TrueType 或 OpenType 的文件头。原 BFTTF 文件和资源清单不变。上游提取脚本已经检查解码后的头，但原宿主加载器没有解码载荷。

原 SDK 的 `GetSharedFont` 返回 bool，参数为 count 输出指针、条目数组、容量及 64 位 LanguageCode。条目大小 0x18，type 位于 0、address 位于 8、size 位于 16。原 SDK 在该字段执行 64 位写入，服务的 32 位大小先作符号扩展。主模块唯一直接导入调用位于 `main+0x31dab5c`，固定传入容量 6，数组位于栈上 `sp+0x48`。成功时 count 返回完整数量，只填充容量允许的条目，容量非正时不写数组。

字体顺序的证据存在限制：[Switchbrew 的服务协议说明](https://switchbrew.org/wiki/Shared_Database_services) 定义 command 5 的参数与返回数组，没有记录各语言的优先顺序。[Ryujinx 的历史实现](https://git.axenov.dev/Museum/ryujinx/src/commit/3e7383b3fdf78d88a25dc2f5d285a847596158f4/Ryujinx.HLE/HOS/Font/SharedFontManager.cs) 可用于核对解码与共享内存形态；[字体服务的历史 command 5 实现](https://git.axenov.dev/Museum/ryujinx/raw/commit/c393cdf8e3775bc95850e4d8c8e4c446b286d3b4/Ryujinx.HLE/OsHle/Services/Pl/ISharedFontManager.cs)按 type 0…5 返回，不能当作 Horizon 所有语言的准确行为。

当前首次启动实验限定实际观测到的 `en-US`，采用明确标注的兼容顺序，其他语言暂时明确拒绝。合成接口/解码测试、真实六字体 UBSan 探针和独立审查均已通过；这不等于游戏文字、排版、缺字及其他语言已验收；这些仍需首帧及后续真实画面验证。
