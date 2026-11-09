# Snow Shot — 免费开源截图、OCR 与录屏工具

[English](README.md) | 简体中文

<img src="docs/imgs/app-icon.png" alt="Snow Shot 应用图标" width="96" height="96">

**Snow Shot（SnowShot）是一款面向 Windows 和 macOS 的免费开源截图工具**。
从截图标注、滚动长截图，到离线 OCR 文字识别、桌面贴图和屏幕录制，
在同一个应用中完成捕捉、整理和分享。

[**下载最新版本**](https://github.com/mg-chao/snow-apps/releases/latest)
· [快速上手](docs/guide/zh-CN/getting-started.md)
· [使用指南](docs/guide/zh-CN/README.md)
· [官方网站](https://snowshot.top/zh/)
· [反馈问题](https://github.com/mg-chao/snow-apps/issues)

这里是由 [mg-chao](https://github.com/mg-chao) 维护的 **Snow Shot 官方 GitHub 仓库**。
仓库名为 **Snow Apps**，同时包含 Snow Image Viewer 看图工具和共享库。
原仓库地址 `mg-chao/snow-shot` 已重定向到这里。**官方网站为 snowshot.top**。

## 功能

| 使用场景 | Snow Shot 提供的功能 | 使用指南 |
| --- | --- | --- |
| 反馈问题、说明重点 | 区域、窗口、全屏和延时截图；箭头、形状、文字、高亮与模糊标注 | [截图与标注](docs/guide/zh-CN/getting-started.md) |
| 保存长网页、聊天记录 | 滚动截图、拼接预览 | [滚动长截图](docs/guide/zh-CN/scrolling-screenshots.md) |
| 从图片复制文字 | 本地 OCR；完整版支持表格和二维码识别 | [离线 OCR 文字识别](docs/guide/zh-CN/offline-ocr.md) |
| 对照资料工作 | 将截图、剪贴板内容固定到其他窗口上方 | [桌面贴图](docs/guide/zh-CN/pin-and-record.md#将参考内容固定到屏幕) |
| 演示操作过程 | 屏幕录制，可配置音频与光标展示 | [录屏](docs/guide/zh-CN/pin-and-record.md#录制一段操作演示) |
| 找回以前的截图 | 本地截图历史、搜索和保留策略 | [历史记录与隐私](docs/guide/zh-CN/getting-started.md#历史记录与隐私) |

## 下载与版本选择

优先从 [GitHub Releases](https://github.com/mg-chao/snow-apps/releases/latest) 获取发布包。
国内下载可使用[官网下载页](https://snowshot.top/zh/download.html)提供的 Gitee 分流。

| 系统 | 完整版 | Mini |
| --- | --- | --- |
| Windows x64 | 在线安装包、离线安装包、便携 ZIP | 在线安装包、便携 ZIP |
| Windows ARM64 | 原生在线安装包、离线安装包、便携 ZIP | 原生在线安装包、便携 ZIP |
| macOS 15+ · Apple 芯片 | DMG、Homebrew | DMG |
| macOS 15+ · Intel | DMG、Homebrew | 暂无 |

- **完整版**：适合需要文字、表格和二维码识别的用户。Windows 离线安装包包含默认 OCR 资源。
- **Mini**：保留截图、标注、贴图和录屏等能力，文字识别资源按需下载；不包含表格、二维码识别，也不提供离线安装包。
- **便携版**：解压后运行，适合不需要安装向导的场景。不要只解压单个可执行文件。

Windows 用户也可以从[微软应用商店](https://apps.microsoft.com/detail/9NRTJ1GBX69V)安装。

### Windows 包管理器

通过 WinGet 安装完整版离线包：

```powershell
winget install --exact --id mg-chao.snow-shot --source winget
```

通过 Scoop 官方 Extras 安装便携版：

```powershell
scoop bucket add extras
scoop install extras/snowshot
```

Scoop 更新前请退出 Snow Shot，使用 `scoop update snowshot` 更新。
分发渠道和版本同步方式见[发布说明](docs/snow-shot-releases.md)。

### macOS 安装

Apple 芯片和 Intel Mac 均需 macOS 15 或更新版本。安装完整版：

```sh
brew update
brew install --cask mg-chao/tap/snow-shot
```

也可下载对应架构的 DMG。首次启动按提示授权屏幕录制；录音等功能需要对应权限。
签名、权限和迁移说明见 [macOS 安装文档](docs-macos-build.md#homebrew-installation)。

## 第一次截图

1. 启动 Snow Shot，按 **F1**，选择区域或窗口。
2. 在工具栏添加箭头、文字、高亮或模糊标注。
3. 复制、保存或贴图。**Ctrl+F1**（macOS 为 **Command+F1**） 可在确认选区后直接复制；**F3** 可将剪贴板内容贴到屏幕。

以上为默认快捷键，可在应用快捷键设置中修改。部分键盘需要配合 **Fn** 使用功能键。
完整操作步骤见[快速上手](docs/guide/zh-CN/getting-started.md)。

## 常见问题

### 是否免费、是否开源？

Snow Shot 免费，源代码使用 **GPL-3.0-or-later** 许可证。
Snow Apps 是多许可证仓库，共享库和第三方组件保留各自许可证，详见 [LICENSE.md](LICENSE.md)
和[英文 README 的许可证表](README.md#open-source-licenses)。

### OCR 是否需要联网？

标准文字识别在本机执行，首次使用前需要具备模型资源。
无网络环境建议使用 Windows 完整版离线安装包。在线翻译和可选 AI 功能有独立的网络、服务商要求，
不能把本地 OCR 与在线功能混为一谈。见[离线 OCR 指南](docs/guide/zh-CN/offline-ocr.md)
和[隐私政策](https://snowshot.top/zh/privacy.html)。

### 支持 Linux 吗？

当前官方桌面安装包面向 Windows 和 macOS，暂无官方 Linux 安装包。

### 为什么仓库叫 snow-apps？如何识别官方版本？

本仓库同时维护 Snow Shot、Snow Image Viewer 和相关共享库。
请使用 **mg-chao/snow-apps** 的 Releases 或 **snowshot.top** 提供的下载入口。
旧 fork 和第三方下载站可能仍在介绍历史版本。

## 开发与贡献

构建和测试约定见 [AGENTS.md](AGENTS.md)，平台说明见
[Windows ARM64](docs-windows-arm64-build.md) 和 [macOS](docs-macos-build.md)。
Rust OCR 子模块的维护步骤见[英文 README](README.md#maintain-the-ocr-submodule)。

仅审查修复问题的 PR，不直接接受新增功能或功能增强的 PR。如有功能需求，请先在社区发起讨论。
提交前请阅读[贡献指南](CONTRIBUTING.md)。

[问题反馈](https://github.com/mg-chao/snow-apps/issues)
· [更新日志](https://github.com/mg-chao/snow-apps/releases)
· [官方社区入口](https://snowshot.top/zh/about.html)
