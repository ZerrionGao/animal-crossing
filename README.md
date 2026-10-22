# ACNH Apple Silicon 移植原型

以固定版本 [MacNativeGames](https://github.com/ly05010419/MacNativeGames/tree/dfa7ec426f8b37798073a108ede1504c827d8af0) 为基座，适配用户本地 ACNH 3.0.0，先推进 Mac 离线单机，再推进 iPhone。

当前实现仍在开发；提取、改写、构建或装载探针通过均不代表完整游戏可玩。状态与未完成门禁见[实施记录](docs/implementation/acnh-macos-plan.md)，技术依据见[研究报告](docs/research/acnh-port-feasibility.md)。

## 本地环境

Apple Silicon、macOS/Xcode 26 或更新、Python 3.11+、LLVM（含 llvm-objcopy/llvm-objdump）、opus/libvpx 静态库。本机已经满足这些条件。所有 Python 依赖安装到项目 `.venv`：

```fish
uv venv .venv --python 3.11
uv pip install --python .venv/bin/python -r requirements.txt
```

根目录放置用户自己的 `Animal Crossing New Horizons.xci`、`Animal Crossing New Horizons v3.0.0.nsp`、`prod.keys`、`title.keys`。支持普通文本与 HTML 保存页 keys，错误信息不输出键值。原件只读。大文件、keys、提取产物和编译产物不进入 Git。

## 分步执行

从仓库根目录执行：

```fish
.venv/bin/python build.py prepare
.venv/bin/python build.py rewrite
.venv/bin/python build.py verify --jobs 2
.venv/bin/python build.py runtime
.venv/bin/python build.py probe
.venv/bin/python build.py boot
```

`prepare` 提取 base/update ExeFS，构建固定 hactool 并合并更新 RomFS，验证六层 IVFC 及已锁定版本的哈希。已存在的输出先验证，不直接覆盖。`rewrite` 只改写指定版本三模块，不带 D2R 游戏功能补丁；`verify` 运行结构校验和带负向对照的 Unicorn 差分验证。`runtime/probe` 验证装载器；`boot` 核验验证记录、字体和资源后执行游戏，使用独立的 `work/acnh/3.0.0/test-profile` 存档目录。

当前启动已进入 `nnMain` 并创建 Metal/NVN 交换链，已越过真实 RomFS 目录读取、六份系统字体加载、时钟／时区、本地账户、设备存档挂载和离线云备份设置查询。在诊断环境打开 GPU 提交后，第 600 帧的交换链画面是机场柜台、豆狸兄弟和开场对话，已目视核对。缺省 `make boot` 仍不打开 GPU 提交。可玩性、实际存档和 iPhone 运行尚未验收，具体证据见实施记录。

当前 `boot` 原型的窗口关闭和系统 Quit 只通知游戏、保留窗口；正常退出链仍待实现。

```fish
.venv/bin/python -m unittest discover -s tests -v
```

日志与数据保存在 `work/acnh/`。源码副本和上游许可在 `third_party/macnativegames/`，来源记录在 `third_party/macnativegames.UPSTREAM.json`；本项目后续适配变更单独记录，不修改用户原游戏。
