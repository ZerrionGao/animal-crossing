# 《集合啦！动物森友会》Apple Silicon 移植实验

[简体中文](README.md) | [English](README.en.md)

这是一个把 Nintendo Switch 版《集合啦！动物森友会》3.0.0 适配到 Apple Silicon Mac 与 iPhone 的个人实验项目。当前重点是离线单机运行，不是完整模拟器，也不提供游戏文件、固件或密钥。

## 项目缘起

我先是看到 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 项目，开始好奇：既然其他 Switch 游戏能尝试移植，那家里孩子最喜欢的《集合啦！动物森友会》有没有可能？于是我动手做了这个实验。

开发过程中，我参考并复用了 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 和 [MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的工程成果。感谢两边的开发者把研究和代码开源出来，让后来者有机会站在这些成果上继续探索。具体复用组件、来源和许可证见 [NOTICE.md](NOTICE.md)。

## 当前状态

- 目标版本：ACNH 3.0.0；目前只有 Apple Silicon Mac 和 iPhone 的实验性构建路径。
- macOS：已有离线原型和可复现的构建、模块验证流程，部分真实游戏服务与图形路径仍未完成。
- iPhone：可以构建应用包；真机输入、存档恢复、前后台恢复和长时间运行等验收尚未全部完成。
- 联机：不支持 Nintendo Online、好友服务或本地无线联机。

## 开始前准备

### 1. 电脑与开发工具

需要 Apple Silicon Mac、macOS 26 或更新版本，以及 Xcode 26 或更新版本的完整 Metal 工具链。仅安装 Xcode Command Line Tools 不够；从 [Apple Xcode 页面](https://developer.apple.com/xcode/)安装 Xcode，首次启动并完成许可确认。之后检查命令行能找到 Xcode：

```fish
xcode-select -p
xcrun --find metal
```

安装 [Homebrew](https://brew.sh/) 后，在终端安装 Mac 构建依赖：

```fish
brew install uv llvm dotnet spirv-cross opus libvpx openssl@3 brotli
```

其中 `dotnet` 提供 .NET 10 SDK；着色器翻译器首次构建时需要联网下载 .NET 依赖。`opus` 和 `libvpx` 用于音频、视频支持。

如果还要构建 iPhone 包，再安装：

```fish
brew install cmake xcodegen
```

iPhone 真机签名和安装还需要 Xcode 中已登录的 Apple 开发者团队，以及目标设备的签名配置。unsigned IPA 只用于本机构建和资源检查，不能直接安装。

### 2. 游戏输入文件

请自行准备并仅在本机使用合法取得的文件。默认把以下文件放在仓库根目录，也就是 `build.py` 所在的位置：

```text
animal-crossing/
├── Animal Crossing New Horizons.xci
├── Animal Crossing New Horizons v3.0.0.nsp
├── prod.keys
└── title.keys
```

XCI 应包含游戏本体，NSP 应是 3.0.0 更新包。密钥文件支持常见的纯文本格式。文件名不同时，可用 `--base`、`--update` 和重复的 `--keys` 参数指定实际位置，例如：

```fish
.venv/bin/python build.py \
  --base "/本机路径/Animal Crossing New Horizons.xci" \
  --update "/本机路径/Animal Crossing New Horizons v3.0.0.nsp" \
  --keys "/本机路径/prod.keys" \
  --keys "/本机路径/title.keys" \
  prepare
```

不要把游戏文件、密钥、提取资源或存档提交到 GitHub，也不要把它们粘贴或上传到聊天工具。

### 3. 下载项目并安装 Python 依赖

从 GitHub 下载源码压缩包，或克隆仓库后，在终端进入项目根目录：

```fish
git clone https://github.com/ZerrionGao/animal-crossing.git
cd animal-crossing
```

然后安装 Python 依赖：

```fish
uv venv .venv --python 3.11
uv pip install --python .venv/bin/python -r requirements.txt
```

这些命令会在项目内创建 Python 环境，不会修改系统 Python。

## 构建 macOS 原型

请在项目根目录按顺序运行。每一步都要成功后再继续；如果某步报错，保留完整错误信息并先排查，不要跳过验证直接启动。

```fish
.venv/bin/python build.py prepare
.venv/bin/python build.py rewrite
.venv/bin/python build.py --jobs 2 verify
make -C games/acnh analyze
.venv/bin/python build.py runtime
.venv/bin/python build.py probe
```

这些步骤会依次提取并校验游戏资源、改写三个游戏模块、运行结构和差分验证、检查启动入口与构造函数、编译 Mac 运行时并执行装载探针。**验证通过只说明这些构建门禁通过，不代表游戏完整可玩。**

要生成 Mac `.app`，还需要本机 NgWord2 数据。使用自己的 XCI 提取到本地工作目录：

```fish
.venv/bin/python games/acnh/scripts/ngc_assets.py \
  --source "Animal Crossing New Horizons.xci" \
  --keys prod.keys \
  --keys title.keys \
  --output work/acnh/3.0.0/ngc
.venv/bin/python build.py package
```

App 生成在 `work/acnh/3.0.0/Animal Crossing New Horizons.app`。产物、提取文件、日志和着色器缓存都留在被 Git 忽略的 `work/` 目录。`package` 只负责打包，不会自动启动游戏。

`boot` 会实际启动本机原型并访问游戏资源，只建议在上述验证通过后用于诊断；它可能因尚未实现的 Switch 服务而停止。运行记录在 `work/acnh/3.0.0/`。

## 构建 iPhone 包

先完成上面的 Python 环境、游戏资源提取和 NgWord2 准备。然后生成本机预检用的 unsigned IPA：

```fish
.venv/bin/python games/acnh/ios/tools/build_ipa.py --unsigned \
  --output work/acnh/3.0.0/ACNH-iPhone-unsigned.ipa
```

要签名安装到真机，需要完成 Xcode 开发者账户和设备配置，再提供已登录团队 ID：

```fish
.venv/bin/python games/acnh/ios/tools/build_ipa.py \
  --team-id "替换成已登录团队的 Team ID" \
  --device "替换成已连接设备的 ID" \
  --output work/acnh/3.0.0/ACNH-iPhone.ipa
```

不要把团队凭据、设备私有信息或签名材料发给 AI agent。签名成功也不等于真机功能验收通过。

## 给 AI agent 的构建路径

如果你使用本机 AI 编程工具，可以把下面这段指令连同项目目录交给它。**XCI、NSP、密钥、存档和生成资源必须留在本机；不要让 agent 上传、分享或打印这些文件的内容。**

```text
请阅读仓库 README.md，并在本机项目根目录按“构建 macOS 原型”逐步执行。
先确认默认输入文件路径是否存在；不要手动打开、打印或上传密钥、游戏文件、存档和生成资源，允许本机项目命令按构建流程读取。
依次运行 README“构建 macOS 原型”中的 prepare、rewrite、verify、analyze、runtime、probe 命令；每一步成功后再运行下一步。
如果命令失败就停止，报告失败命令、脱敏后的错误和日志路径，不跳过门禁、不自行修改源码来绕过失败。
需要生成 .app 时，先确认本机 NgWord2 数据已经放在 work/acnh/3.0.0/ngc，再执行 README 中的提取与 package 命令。
不要运行 boot，除非我另外要求启动游戏。最终逐项报告通过/失败的命令，并说明构建通过不代表完整可玩或真机验收通过。
```

如果游戏文件名不同，可告诉 agent 文件路径用于填写命令；不要把文件本身交给云端模型。

## 已知问题

- 目前只锁定并验证 ACNH 3.0.0，其他游戏版本、Intel Mac 和其他芯片平台不在支持范围。
- Mac 已经能显示机场画面，也有游玩记录；但还有一些系统和图形功能不完整，相关检查也未全部完成，所以不能说整体验收通过。游戏如果调用尚未支持的图形功能，可能会中止。
- 项目只面向离线单机，不实现网络连接、好友、云存档或本地无线联机。
- iPhone 原型仍需继续验证人物/岛屿命名输入、存档读回与恢复、前后台切换、资源完整性和长时间运行。
- iPhone 长时间运行仍有发热问题，现有观察还不能量化温度或功耗改善。
- NgWord2 资源准备目前要求完整的 54 项清单；已核对的本机 XCI 缺少 `ac_similar_form_nx` 与 `table_similar_form_nx`，严格提取会停止，因此该输入下 `.app`/iPhone 打包暂时无法走完。不要用空文件或占位数据绕过校验。
- 构建需要开发工具链，并会从本机游戏输入生成较大的资源和着色器文件；首次构建会下载依赖，耗时和磁盘占用取决于本机环境。

## 后续计划

接下来会继续补齐服务与图形路径，排查性能和发热，完善稳定性，并评估联机能力的可行边界。也希望探索 Android 和 HarmonyOS 等新终端；这些方向目前都没有实现或时间承诺，需先评估系统服务、GPU 图形后端、签名和性能条件。

## 致谢与联系

感谢 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 和 [MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的作者及贡献者，也感谢所有愿意阅读、测试和反馈的开源朋友。

欢迎通过小红书联系我：**Zerrion**。

项目自有代码采用 [MIT License](LICENSE)；复用组件的原始版权和许可见 [NOTICE.md](NOTICE.md)。请自行确认本机游戏输入材料的取得与使用权限。
