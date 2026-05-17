# 开发文档

本文档面向维护者和插件开发者，描述本地开发、调试和打包流程。验证命令如何选择，以 `docs/testing.md` 为准。

## 适用范围

- 调试 `client/` 或 `server_cpp/`
- 临时覆盖插件内置 server
- 生成可分发 `.vsix`

普通用户通常不需要阅读本文档。

## Client 开发

- 源码：`client/src/`
- 编译输出：`client/out/`
- 常用命令：`npm run compile`
- VS Code 调试入口：`.vscode/launch.json`
- 编辑器壳层事实文档：`docs/client-editor-features.md`

推荐调试入口：

- 稳定启动：
  - `Launch Client (G66 Workspace)`
  - `Launch Client (Repo Workspace)`
  - `Launch Client (Open NSF Sample)`
- 热更新开发：
  - `Launch Client (G66 Workspace, Watch)`

约定：

- 默认 `F5` 对应稳定启动入口，并会先执行 `npm: compile`。
- watch 入口适合连续修改 `client/` 时长期运行。
- 如果只是快速拉起调试，优先使用稳定入口。

## Server 开发

- 源码：`server_cpp/src/`
- 资源：`server_cpp/resources/`
- 推荐构建目录：`server_cpp/build`

常用命令：

```powershell
cmake -S .\server_cpp -B .\server_cpp\build -G "MinGW Makefiles"
cmake --build .\server_cpp\build
```

Windows 上当前要求 `clang++` 至少为 20。如果 PATH 上默认命中的版本过旧，应在 configure 时显式追加：

```powershell
-D CMAKE_CXX_COMPILER=<llvm-mingw clang++.exe>
```

如果链接阶段提示 `nsf_lsp.exe` 被占用，先关闭运行中的 VS Code 扩展宿主或测试进程，再重新构建。

## 内置 Server 与覆盖

插件安装包内自带 C++ server。client 启动优先级：

1. 插件内置 server
2. `nsf.serverPath` 指定的外部 server

`nsf.serverPath` 仅用于开发和调试。需要验证本地 server 时：

1. 构建 `server_cpp/build/nsf_lsp.exe`。
2. 在 VS Code 设置中配置 `nsf.serverPath`。
3. 执行 `NSF: Restart LSP Server`。

## 设置分层

普通用户设置位于 `NSF` 分组：

- include 路径
- shader 扩展名
- 预处理宏 preset
- defines
- inlay hints
- semantic tokens
- `nsf.diagnostics.mode`

高级设置位于 `NSF (Advanced)` 分组：

- `nsf.serverPath`

`nsf.preprocessorMacros` 是用户可见的完整预处理宏 preset 表，支持字符串、数字、布尔 replacement。扩展首次会把内置 preset 写入工作区设置；之后用户直接编辑或删除条目即可改变有效宏表。它在 `nsf.defines` 之前生效，源码 `#define/#undef` 仍然优先级最高。

上线版只保留稳定且有明确用途的高级 override。历史内部调试、实验或细粒度 diagnostics 设置不再作为支持的用户入口。

## 诊断模式

当前推荐通过 `nsf.diagnostics.mode` 控制诊断强度：

- `basic`: 保守诊断，降低后台和昂贵规则开销。
- `balanced`: 默认诊断，保留真实 mismatch/error 和主要语义诊断，但不显示合法且高噪的隐式转换风险 warning。
- `full`: 完整诊断，在 `balanced` 基础上显示 truncation、narrowing、floating-integral、signedness 和 boolean conversion 等隐式转换风险 warning，用于源码审核和专项治理。

client 会把 mode 归一化成 server 可理解的 diagnostics 配置；server 也能直接解析 `diagnostics.mode`。

## 打包

正式打包命令：

```powershell
npm run package:vsix
```

该命令会：

1. 编译 TypeScript 客户端。
2. 自动解析可用的 Clang 20+ 编译器，优先使用 `NSF_PACKAGE_CXX_COMPILER`，其次探测 `C:\Software\llvm-mingw-*`，最后回退到 PATH 中的 `clang++`。
3. 在独立的 `server_cpp/build_vsix` 中构建干净 release server。
4. 用临时 staging 目录收集运行时最小文件集。
5. 把 `nsf_lsp.exe` 依赖的常见 MinGW 运行时 DLL 一并放到 server 同目录。
6. 生成正式 `.vsix`。

不建议直接在仓库根目录运行 `npx vsce package`，避免把构建中间件、测试依赖或历史残留卷进包里。

## 更新本文档

以下变化必须同步更新本文档：

- 内置 server 或 `nsf.serverPath` 优先级规则变化
- 本地调试入口变化
- 设置分层或诊断 mode 语义变化
- 打包命令、编译器解析或 staging 内容变化
