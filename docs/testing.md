# 测试与验证

本文档描述这个仓库当前常用的验证命令，以及什么时候该跑哪一层。

## 验证梯度

按成本从低到高：

1. `npm run json:validate`
2. `npm run compile`
3. `cmake --build .\\server_cpp\\build`
4. `npm run test:client:repo`
5. `npm run test:client:all`
6. `npm run gate:d3`
7. `npm run package:vsix`

## 每条命令做什么

### `npm run json:validate`

用途：

- 校验 `server_cpp/resources/` 下所有 bundle 的 `base.json`、`override.json`、`schema.json`
- 检查关键覆盖率门禁

适用场景：

- 修改任何资源 bundle
- 修改资源生成脚本
- 修改 schema 规则

### `npm run compile`

用途：

- 构建 TypeScript 客户端和测试入口

适用场景：

- 修改 `client/`
- 修改 `src/test/`
- 修改根级 TypeScript 配置

### `cmake --build .\\server_cpp\\build`

用途：

- 构建 C++ 服务端
- 同时把 `server_cpp/resources/` 拷贝到构建输出目录

适用场景：

- 修改 `server_cpp/src/`
- 修改资源并希望本地构建产物拿到最新 bundle

注意：

- Windows 下如果 `nsf_lsp.exe` 正被 VS Code 或测试进程占用，链接阶段可能报 `permission denied`
- 这种情况先结束占用进程，再重新构建

### `npm run test:client:repo`

用途：

- 运行仓库模式集成测试
- 使用 `test_files/` 作为固定夹具

适用场景：

- 修改 completion、hover、signature help、diagnostics、semantic tokens
- 修改 client/server 协议交互
- 修改默认资源并想验证实际行为

说明：

- 这是默认最可信的一条集成验证命令
- 如果只跑一条集成测试，优先跑它

### `npm run test:client:all`

用途：

- 运行 repo 模式 + real workspace 模式

适用场景：

- 发版前
- 需要确认真实工作区不退化

说明：

- `real` 模式依赖外部工作区路径配置，成本比 repo 模式高
- `real` 模式下的 smoke 断言应优先验证“结果存在且链路打通”，不要依赖外部项目里固定不变的引用数或编辑数

### `npm run gate:d3`

用途：

- 运行完整门禁

当前包含：

1. `npm run json:validate`
2. `npm run compile`
3. `cmake --build .\\server_cpp\\build`
4. `python .\\server_cpp\\tools\\hover_smoke_test.py`
5. `npm run test:client:all`

适用场景：

- 发版前
- 大范围重构后

### `npm run package:vsix`

用途：

- 构建 TypeScript 客户端
- 构建干净的 C++ server 输出目录 `server_cpp/build`
- 生成只包含运行时必需文件的 `.vsix`

适用场景：

- 给他人安装插件
- 发版前生成可分发安装包

说明：

- 这是当前推荐的打包方式
- 不建议直接在仓库根目录运行 `npx vsce package`
- 脚本会使用临时 staging 目录组装最小运行时内容

## 推荐验证矩阵

### 只改文档

通常不需要构建或测试。

### 改资源或资源脚本

至少跑：

- `npm run json:validate`

通常还应跑：

- `cmake --build .\\server_cpp\\build`
- `npm run test:client:repo`

### 改 TypeScript client

至少跑：

- `npm run compile`

如果影响行为，再跑：

- `npm run test:client:repo`

### 改编辑器壳层配置

适用场景：

- 修改 `package.json` 里的语言注册、语言配置或 snippets 挂接
- 修改 `language-configuration.json`
- 修改 `*.code-snippets`

至少做：

- 手工 smoke 检查 `.nsf`、`.hlsl`、`.hlsli` 的语言模式归属
- 手工检查 `Ctrl+/` 行注释、`Shift+Alt+A` 块注释、基础自动配对和基础缩进行为
- 手工检查双击选词对 `SV_Position`、宏名和常见标识符不退化
- 手工检查 `///`、`/** */` 的注释续写和 `#region/#endregion` 折叠

如果改了 snippets，再补做：

- 手工检查 `nsf` 语言下最小片段是否出现且占位符顺序合理

如果同时改了 `client/` 代码，再额外跑：

- `npm run compile`

### 改 C++ server

至少跑：

- `cmake --build .\\server_cpp\\build`

如果影响行为，再跑：

- `npm run test:client:repo`

### 发版或大范围重构

跑：

- `npm run gate:d3`
- `npm run package:vsix`

## 集成测试目录

- `src/test/`
  - VS Code 测试入口和测试启动器
- `src/test/suite/client.integration.test.ts`
  - 主要仓库模式集成测试
- `test_files/`
  - 夹具文件

如果你修改了行为但没有更新相关夹具或断言，测试很可能只是假通过。

## 对失败结果的处理建议

- 单条集成测试偶发失败时，先重跑一次确认是否稳定复现
- 如果只在一次运行里出现时序型失败，不要立刻修改业务逻辑
- 如果失败与资源或路径相关，先确认是否已重新构建 C++ server 并把最新资源拷贝到输出目录

## 何时需要同步更新本文档

以下内容变化时，必须更新本文档：

- 新增或删除验证命令
- `gate:d3` 内容变化
- 推荐打包命令变化
- repo/real 测试入口变化
- 验证层级的推荐顺序变化
