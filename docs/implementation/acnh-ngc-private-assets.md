# ACNH NgWord2 私有资源准备

本工具只处理用户本机自有 XCI 更新分区中的 NgWord2 NCA、单个 NCA、包含该 NCA 的本机目录，或已解密的 RomFS 目录。NCA 必须经解密 header 确认 title ID 为 `0100000000000823`，content type 为 Data（4）或 PublicData（5）。不会下载词库，也不会把词条、文件正文、密钥或密钥路径写进日志和清单。

```sh
python3 games/acnh/scripts/ngc_assets.py \
  --source /本机路径/owned-game.xci \
  --keys /本机路径/prod.keys \
  --output work/acnh/ngc-private
```

`--source` 也可指向单个 NCA、含 `.nca` 或 Ryujinx `00` 文件的目录，或解密后的 RomFS 目录。XCI 模式只在 update 分区全量检查 NCA header 后按 title ID 选择目标，避免漏掉 content type 为 5 的 PublicData。NCA 输入会核对 NCA3、title ID、Data/PublicData 类型、头部长度、FS header 哈希和六层 IVFC；上游校验不验证 NCA RSA 签名。解密 RomFS 目录不携带可证明来源的 NCA 头，清单会标成 `decrypted-romfs-unverified-title-and-integrity`，不能据此确认它来自 NgWord2。

工具只接收 `version.dat`、三份 common 资源、索引 0 至 15 的三类区域资源，以及 `ac_similar_form_nx`、`table_similar_form_nx`，共 54 项。名字和版本格式依据固定 Ryujinx [`ContentsReader.cs`](https://git.axenov.dev/Museum/ryujinx/src/commit/33ba1703158564c2c3564fa329fd2e630f8a8e95/src/Ryujinx.Horizon/Sdk/Ngc/Detail/ContentsReader.cs)（MIT，commit `33ba1703158564c2c3564fa329fd2e630f8a8e95`）：文件名无扩展名，`version.dat` 必须为 4 字节大端无符号整数。所有 `_nx` 文件必须是完整的单成员 gzip 流；每项解压不超过 64 MiB，合计输出不超过 256 MiB。原 gzip 文件保留，旁边写出 `.decompressed` 文件；清单记录压缩和解压后的大小及 SHA-256。缺文件、重复名、未知文件、路径层级不符、损坏 gzip、越界和符号链接均失败。输出目录必须不存在，且通过临时目录原子发布。

当前本机 XCI 的已验证清单包含 52 项，缺少 `ac_similar_form_nx` 和 `table_similar_form_nx`；严格入口会报告缺项并失败，不发布部分产物。运行时从该 RomFS 目录加载已有的 gzip 表并解析 Aho-Corasick，不依赖那两份 similar-form。准备工具仍不解析词条正文，也不把词库写入 Git。
