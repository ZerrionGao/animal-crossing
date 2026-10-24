# ACNH NGC 服务依据

`boot-diag.log` 等真实启动日志观察到 `ngc:u` 的 `Mask` 命令 2：`type=6`、非 domain、inline 数据 40 字节、一个 A 输入缓冲和一个 B 输出缓冲，没有 PID、句柄或其他缓冲区。SDK ABI 将这 40 字节拆为 24 字节有效字段（一个 `u32` 掩码和五个 `u32 option`）加 16 字节尾部 padding；因此服务校验的是完整的 40 字节线参数区，不能误判为 24 字节请求。原协议把 `ngc:u` 命令 2 定义为 `Mask`。

本地 SDK 反汇编载体 `work/acnh/3.0.0/original-analysis/sdk.elf` 的导出索引也保留了 `nn::ngc::ProfanityFilter::MaskProfanityWordsInText` 和 `CheckArgumentsWord`；对应条目见 `sdk-inventory.json:23384`、`sdk-inventory.json:56924`，游戏侧导入见 `main-inventory.json:1973`。这些条目支持 NGC 词库/过滤器确实由 SDK 提供，但不构成本地词库已经存在的证据。

本机已从用户自有 XCI 取出 NgWord2（title `0100000000000823`）。运行时按 `TOTK_NGC_DIR`（缺省再试工作区 `work/research/ngword2-from-xci-20261023/romfs`）加载 gzip 的 `ac_common_*` 与各地区 `not_b`/`b1` 表，用 Aho-Corasick 做子串匹配，命中处写成 `*`，并通过 CMIF 返回替换次数。ASCII 会先折成小写；没有 similar-form / NFKC / 分隔完整词（b2）那几步，因此还不是完整的任天堂过滤器。词库缺失时仍原文照写以免命名中止。不读取、不打印玩家名字。未知和畸形请求仍然失败。

依据：[NGC services](https://switchbrew.org/wiki/NGC_services)。生产探针覆盖已观测的 24+16 字节 Mask、尾部 padding 不足的畸形形状、未知命令和 `GAME_ACNH` 关闭时不注册服务。
