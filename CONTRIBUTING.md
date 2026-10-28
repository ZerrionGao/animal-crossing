# 贡献指南

感谢参与。请先阅读 [README](README.md) 了解平台、输入文件和项目当前范围。

## 开发环境

项目面向 Apple Silicon Mac，需要 Xcode/Metal 工具链、Python 3.11、LLVM、`.NET 10 SDK`、OpenSSL 3 与 Brotli（Homebrew）、`spirv-cross`、`opus` 和 `libvpx`。Python 依赖安装在仓库内：

```fish
uv venv .venv --python 3.11
uv pip install --python .venv/bin/python -r requirements.txt
```

## 提交前检查

```fish
.venv/bin/python -m unittest discover -s tests -v
git diff --check
```

变更涉及 macOS/iOS 运行时、构建或 Metal 图形时，请在 PR 说明中写明实际执行的平台和验证步骤；合成测试、构建成功和真机验收请分别陈述。

## 变更约定

- 运行时与图形后端的共用实现放在 `engine/`；游戏版本、游戏专用适配和平台入口放在 `games/<game>/`。
- 新增依赖前确认许可证，并同步更新 `NOTICE.md`。
- 不提交游戏镜像、密钥、固件、个人存档、生成资源、签名包或其他构建产物。
- 不删除复用代码中的版权声明；重命名或移动文件时保留相应许可证。
- PR 聚焦一个目的，说明行为影响、回退方式和未覆盖的验证范围。
