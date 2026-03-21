# 开发文档

本文档面向维护者和插件开发者，描述当前推荐的本地开发、调试和打包流程。

## 目标读者

- 调试 `client/` 或 `server_cpp/` 的开发者
- 需要替换插件内置 server 的维护者
- 需要生成可分发 `.vsix` 的发布者

普通用户通常不需要阅读本文档。

## 开发入口

### 客户端

- 源码：`client/src/extension.ts`
- 编译输出：`client/out/*.js`
- 常用命令：`npm run compile`

### 服务端

- 源码：`server_cpp/src/`
- 资源：`server_cpp/resources/`
- 推荐干净构建目录：`server_cpp/build`
- 常用命令：
  - `cmake -S .\\server_cpp -B .\\server_cpp\\build -G "MinGW Makefiles"`
  - `cmake --build .\\server_cpp\\build`

## 内置 Server 与覆盖规则

插件安装包内会自带一个 C++ server。

当前客户端启动规则：

1. 优先使用插件内置 server
2. 如果设置了 `nsf.serverPath`，则用该路径覆盖内置 server

结论：

- 普通用户不需要手动设置 `nsf.serverPath`
- `nsf.serverPath` 是开发/调试用的高级 override

## 本地替换内置 Server

当你需要在 VS Code 中调试本地刚编出来的 server：

1. 构建 `server_cpp/build/nsf_lsp.exe`
2. 在 VS Code 设置中配置 `nsf.serverPath`
3. 执行命令 `NSF: Restart LSP Server`

推荐只在以下场景使用 `nsf.serverPath`：

- 本地调试新的 server 二进制
- 验证临时修复
- 对比插件内置 server 和外部 server

## 设置分层

当前设置分成两层：

### 普通用户设置

位于 `NSF` 分组：

- include 路径
- shader 扩展名
- defines
- inlay hints
- semantic tokens
- `nsf.diagnostics.mode`

### 高级设置

位于 `NSF (Advanced)` 分组：

- `nsf.serverPath`

原则：

- 普通用户只应接触 `NSF` 分组
- `NSF (Advanced)` 在上线版里只保留稳定且有明确用途的高级 override

## 开发者设置原则

上线版只保留一个开发者可见高级设置：

- `nsf.serverPath`

其余历史上的内部调试/实验/细粒度 diagnostics 设置，已经不再作为支持的配置入口继续保留。

如果未来确实需要重新引入某类开发开关，应优先通过以下方式之一处理：

- 新增明确的开发命令
- 在开发分支或开发构建中单独暴露
- 通过代码常量或临时 patch 控制，而不是重新公开给所有用户

示例文件：

- `docs/settings.development.example.json`

## 诊断模式

当前推荐用户通过 `nsf.diagnostics.mode` 控制诊断强度，而不是直接改预算参数。

可选值：

- `basic`
- `balanced`
- `full`

说明：

- client 会把 mode 归一化成当前 server 仍然可理解的详细 diagnostics 配置
- server 也能直接解析 `diagnostics.mode`
- 细粒度 diagnostics 参数不再作为用户设置入口继续支持

## 推荐验证顺序

### 改资源

1. `npm run json:validate`
2. `cmake --build .\\server_cpp\\build`
3. `npm run test:client:repo`

### 改 client

1. `npm run compile`
2. `npm run test:client:repo`

### 改 server

1. `cmake --build .\\server_cpp\\build`
2. `npm run test:client:repo`

### 发版前

1. `npm run gate:d3`
2. `npm run package:vsix`

## 打包

当前推荐的正式打包命令：

- `npm run package:vsix`

它会做这些事：

1. 编译 TypeScript 客户端
2. 构建干净的 `server_cpp/build`
3. 用临时 staging 目录收集运行时最小文件集
4. 生成正式 `.vsix`

不建议直接在仓库根目录运行 `npx vsce package`，因为那样更容易把构建中间件、测试依赖或历史残留卷进包里。

## 当前已知待收敛项

- `package.json` 仍缺 `repository`
- 仓库仍缺正式 `LICENSE` 文件

## 何时更新本文档

以下内容变化时，必须更新本文档：

- 内置 server 或 `nsf.serverPath` 的优先级规则变化
- 打包流程变化
- 设置分层变化
- 诊断 mode 的语义变化
