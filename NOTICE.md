# 版权与第三方组件

根目录 [LICENSE](LICENSE) 适用于本项目自有代码。下列复用组件按各自许可证分发，原有版权声明仍适用于对应文件。

| 目录 | 组件及来源 | 许可 |
|---|---|---|
| `engine/runtime/`、`engine/tools/game-data-extractors/`、`engine/tools/gen_embed.py`、`games/acnh/scripts/hactool.py` | MacNativeGames，固定版本 `dfa7ec426f8b37798073a108ede1504c827d8af0`，经 ACNH 适配 | MIT；见 `licenses/runtime-engine-MIT.txt` |
| `engine/tools/nso-rewriter/` | d2r-mobile 的 NSO 改写与验证工具，按 ACNH 需要保留部分模块 | MIT；见 `licenses/components/nso-rewriter-MIT.txt` |
| `engine/graphics/` | NVN/Metal 图形与纹理代码 | MIT、zlib；见 `licenses/components/nvn-metal-MIT.txt`、`licenses/components/deko3d-zlib.txt` |
| `engine/vendor/bcdec/` | bcdec | MIT 或公有领域；见该目录的 `LICENSE` |
| `engine/vendor/ryujinx/` | Ryujinx 图形着色器模块 | MIT；见该目录的 `LICENSE.txt` |
| `engine/tools/shader-translator/` | 着色器服务及对 Ryujinx 的适配 | MIT；见 `licenses/components/shader-translator-MIT.txt` |
| `engine/runtime/audio_*.c` | Ryujinx.Audio 的音频算法实现 | MIT；见 `licenses/components/ryujinx-audio-MIT.txt` |

MacNativeGames 固定版本记录见 [`licenses/runtime-engine-source.json`](licenses/runtime-engine-source.json)。目录重组不改变各组件的许可或版权归属。构建时获取的 SPIRV-Cross、hactool、libopus 和 libvpx 仍受其各自许可证约束；其许可证随对应依赖源或系统包提供。

本仓库没有 ACNH 游戏镜像、系统密钥或固件。构建产物由使用者在本机使用自己的输入生成。
