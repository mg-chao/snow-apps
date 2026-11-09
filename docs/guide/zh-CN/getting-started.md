# Snow Shot 安装与截图快速上手

[全部指南](README.md) · [English](../getting-started.md)

Snow Shot 将截图、标注、本地 OCR、贴图和录屏集中在一个应用中。
不确定选哪个版本时，可先选择完整版；需要更小安装包时再考虑 Mini。

## 选择安装包

从[官方 GitHub Releases](https://github.com/mg-chao/snow-apps/releases/latest)
或[官网下载选择页](https://snowshot.top/zh/download.html)下载。

| 电脑或使用需求 | 选择的包 |
| --- | --- |
| Intel、AMD x64 处理器的 Windows 电脑 | `windows-x64` |
| ARM64 处理器的 Windows 电脑 | `windows-arm64` |
| Apple 芯片 Mac | `macos-arm64.dmg` |
| Intel Mac | 完整版 `macos-x86_64.dmg` |
| 无网络环境安装 Windows 版 | 完整版 `offline.exe` |
| 不需要安装向导的 Windows 版 | 完整解压 `portable.zip` |

Windows 可在“设置 → 系统 → 系统信息”中查看系统类型；macOS 可在“关于本机”中查看芯片或处理器。
macOS 安装包要求系统版本 15 或更新。Mini 支持两种 Windows 架构和 Apple 芯片 Mac，
不提供 Intel Mac 版本或离线安装包。

Windows 完整版离线包包含默认 OCR 资源。在线包和 Mini 部分组件在首次使用时下载。
Mini 不含表格、二维码识别，需要文字识别时可在设置中启用手动文字识别。

包管理器命令见[项目首页](../../../README.zh-CN.md#windows-包管理器)。
通过包管理器安装的应用，建议继续使用对应包管理器更新。

## 截图与标注

1. 启动 Snow Shot。macOS 首次使用时按提示允许屏幕录制；修改权限后如提示重启应用，请重新启动。
2. 按 **F1**，选择需要截取的区域或窗口。
3. 使用截图工具栏添加箭头、形状、文字、高亮或模糊标注。
4. 检查结果，再通过工具栏复制、保存或贴图。

只想快速框选并复制时使用 **Ctrl+F1**（macOS 为 **Command+F1**）。需要先展开菜单再截图时，可在应用快捷键设置中配置延时截图动作。

## 快捷键没有反应

默认截图键为 **F1**，剪贴板贴图为 **F3**。某些键盘需要配合 **Fn** 才能发送功能键，
其他应用也可能占用相同组合键；遇到冲突时，可在 Snow Shot 快捷键设置中改为其他按键。

文字识别、滚动截图等动作可以单独配置全局快捷键。显示“未设置”的动作没有绑定按键，
并非每一个工具栏功能都预设了快捷键。已有的自定义绑定优先于文档中的默认值。

## 历史记录与隐私

在截图历史中查找之前的画面，并按需要配置保留时长和存储位置。
截图历史可能保留导出裁剪范围之外的原始屏幕图像；删除导出的图片不一定同时删除历史记录。

截图、标注和标准 OCR 在本机运行。在线翻译、已配置的 AI 连接等功能有单独的数据处理方式，
启用前可阅读[隐私政策](https://snowshot.top/zh/privacy.html)。

## 继续使用

- [滚动长截图](scrolling-screenshots.md)
- [离线 OCR 文字识别](offline-ocr.md)
- [桌面贴图与录屏](pin-and-record.md)

如出现黑屏或内容缺失，先检查系统屏幕权限、被截取的应用和显示配置。
[反馈问题](https://github.com/mg-chao/snow-apps/issues)时请附上这些信息及复现步骤。
