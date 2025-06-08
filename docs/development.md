#### 开发文档

GUI 框架使用 [Tauri](https://tauri.app/)
Web 开发框架使用 [Next.js](https://nextjs.org/)
UI 组件库使用 [Ant Design](https://ant.design/)

##### 开发环境

| 软件                                         | 版本       |
| -------------------------------------------- | ---------- |
| 操作系统                                     | Windows 11 |
| [Node.js](https://nodejs.org)                | 20.11.0    |
| [pnpm](https://pnpm.io/)                     | 10.0.0     |
| [yarn](https://classic.yarnpkg.com/lang/en/) | 1.22.22    |
| [rust](https://rust-lang.org)                | 1.84.1     |
| [cargo](https://rust-lang.org)               | 1.84.1     |


##### 运行项目

在安装 Snow Shot 的项目依赖前，还需要手动准备一下 [@mg-chao/excalidraw](https://github.com/mg-chao/excalidraw) 项目作为项目的依赖。

因为 Snow Shot 需要自定义 Excalidraw 非常多的功能，Excalidraw 的官方组件远远没法满足 Snow Shot 的支持。

将 @mg-chao/excalidraw 项目 clone 到 Snow Shot 的同级目录后，并 checkout 到 `custom/master`，使用一下命令安装依赖。

```bash
yarn i
```

Excalidraw 使用 yarn 作为包管理器，这点和 Snow Shot 不同。

接着回到 Snow Shot 目录，运行命令以构建 Excalidraw。

```bash
pnpm update:excalidraw
```

准备好上述环境后，首先需要安装前端的依赖

```bash
pnpm i
```

安装完成后，使用以下命令运行项目

```bash
pnpm tauri dev
```

