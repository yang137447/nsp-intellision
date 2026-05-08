# 测试与验证

本文档描述当前验证命令、适用场景和测试约束。开发调试和打包细节见 `docs/development.md`。

## 验证梯度

按成本从低到高：

1. `npm run json:validate`
2. `npm run compile`
3. `cmake --build .\server_cpp\build`
4. `npm run test:client:repo`
5. `npm run test:client:repo:m4`
6. `npm run test:client:all`
7. `npm run test:client:perf`
8. `npm run test:client:real:replay`
9. `npm run gate:d3`
10. `npm run package:vsix`

## 命令职责

- `npm run json:validate`
  - 校验 `server_cpp/resources/` 下所有 bundle 的 `base.json`、`override.json` 和 `schema.json`
  - 适用于资源、schema 或资源生成脚本变更
- `npm run compile`
  - 构建 TypeScript client 和测试入口
  - 适用于 `client/`、`src/test/` 或根级 TypeScript 配置变更
- `cmake --build .\server_cpp\build`
  - 构建 C++ server，并把 `server_cpp/resources/` 拷贝到构建输出目录
  - 适用于 `server_cpp/src/` 变更，或需要本地构建产物拿到最新资源时
- `npm run test:client:repo`
  - 运行仓库模式集成测试，使用 `test_files/` 固定夹具
  - 是默认最可信的集成回归入口
- `npm run test:client:repo:m4`
  - 定向运行 shared analysis context repo 验收
  - 适用于 `AnalysisSnapshotKey`、`ActiveUnitSnapshot`、defines、include closure 和 workspace summary 失效传播变更
- `npm run test:client:all`
  - 运行 repo 模式 + real workspace 模式
  - 适用于发版前或需要确认真实工作区不退化时
- `npm run test:client:perf`
  - 运行 repo 模式性能基线 suite，并写报告到 `out/test/perf-reports/`
  - 适用于 interactive / deferred / diagnostics / workspace-summary 调度或性能路径变更
- `npm run test:client:real:replay`
  - 在真实 workspace 输入路径上回放短交互脚本，并写报告到 `out/test/perf-reports/real-replay/`
  - 适用于分析真实交互延迟和 anomaly 趋势
- `npm run gate:d3`
  - 发版前完整门禁：资源校验、TypeScript 编译、Clang 20+ clean configure、C++ 构建、hover smoke、client 全量测试
- `npm run package:vsix`
  - 生成可分发 `.vsix`
  - 打包流程细节以 `docs/development.md` 为准

## 推荐矩阵

- 只改文档：通常不需要构建或测试
- 改资源或资源脚本：至少跑 `npm run json:validate`
- 改 TypeScript client：至少跑 `npm run compile`
- 改 C++ server：至少跑 `cmake --build .\server_cpp\build`
- 改 completion、hover、signature help、diagnostics、semantic tokens 或 client/server 协议交互：补跑 `npm run test:client:repo`
- 改 deferred/current-doc cache、full diagnostics 预热/发布链路、inlay hints full-cache 或慢路径失效：`npm run test:client:repo` 是最小必跑项
- 改调度优先级、latest-only、cancellation、metrics 或性能命中路径：补跑 `npm run test:client:perf`
- 发版或大范围重构：跑 `npm run gate:d3` 和 `npm run package:vsix`

编辑器壳层配置变更还要手工 smoke：

- `.nsf`、`.hlsl`、`.hlsli` 的语言模式归属
- `Ctrl+/` 行注释、`Shift+Alt+A` 块注释
- 自动配对、包裹、基础缩进和 `wordPattern`
- `///`、`/** */` 注释续写
- `#region/#endregion` 折叠
- snippets 出现和占位符顺序

## 集成测试结构

- `src/test/`: VS Code 测试入口和测试启动器
- `src/test/suite/*.test.ts`: repo / real / perf 测试入口；loader 只加载匹配的编译产物，避免陈旧 `.js` 重复执行
- `src/test/suite/client.integration.groups.ts`: repo 模式 architecture registrar，按层聚合 integration 定义
- `src/test/suite/integration/*.ts`: 具体 integration 实现
- `src/test/suite/test_helpers.ts`: repo 模式共享 helper
- `test_files/`: 固定夹具

当前 repo 模式测试按架构拆分。定向诊断可设置：

```powershell
$env:NSF_TEST_FILE_FILTER = "<substring>"
```

`npm run test:client:perf` 也通过同一机制只加载 perf suite。

## 测试写法约束

### Workspace 与 include

- workspace-summary、include-graph、cross-file 类用例如果需要额外扫描根，应使用 `withTemporaryIntellisionPath(...)` 显式设置夹具根，再等待 indexing `Idle`。
- 动态修改 `nsf.intellisionPath` 后，推荐顺序是：更新配置、重启 server、等待 indexing `Idle`、再发请求。
- file watch / reverse-include 类用例至少打开两个依赖同一 provider 的 consumer，验证 untouched consumer 也完成回流。
- 依赖 active unit 的用例应调用 `nsf._setActiveUnitForTests`，并等待 active unit include closure 与 interactive visibility fingerprint 收敛。

### 编辑与请求时序

- 测试刚激活后修改 runtime 配置前，应先 `waitForClientReady(...)`。
- repo-mode / replay runner 不要用 `vscode.commands.executeCommand('type')` 或 `deleteLeft` 驱动普通编辑；优先用 `typeTextForTests(...)` / `deleteLeftForTests(...)`。
- 如果测试目标是 editor-native auto-trigger，使用 `typeWithEditorFocusForTests(...)`。
- semantic tokens、inlay hints、document symbols 属于 deferred 路径，应使用 `waitFor(...)` 等待 provider、legend 或 cache 进入可断言状态。
- completion / hover 断言特定 server 结果时，优先用 `waitForCompletionLabels(...)`、`waitForHoverText(...)`，不要只等非空。

### Runtime 断言

- `interactive-visibility` 类用例应结合 `getInteractiveRuntimeDebug(...)` 断言 `lastResolvedLayer`。
- reverse-include / workspace-summary 回流断言应优先用 `getDocumentRuntimeDebug(...)` 比较 runtime 摘要，而不是只靠 diagnostics 间接猜测。
- 跨文件或 deferred semantic diagnostics 用例应使用 `waitForDiagnostics(...)`；必要时用 `touchDocument(...)` 或 `waitForDiagnosticsWithTouches(...)` 重新排队。
- definition / references 存在同名候选时，优先断言结果集合包含目标路径；只有当前架构明确承诺选主顺序时才断言首项。

### 并发与性能

- background lane latest-only / cancellation 用例不要手搓原始并发请求；优先使用 test mode 内部命令 `nsf._spamInlayRequests`、`nsf._spamDocumentSymbolRequests`、`nsf._spamWorkspaceSymbolRequests`。
- visible-range inlay / perf 用例不要把冷请求固定绑定为 range-build；range-build 或基于 full-cache 的 range-filter 都可能是合法路径。
- real workspace 测试不要依赖外部工程里某一整行固定文本永远不变；优先用稳定前缀定位并缓存原始文本。

## 失败处理

- 单条集成测试偶发失败时，先重跑一次确认是否稳定复现。
- 如果只在一次运行里出现时序型失败，不要立刻修改业务逻辑。
- 如果失败与资源或路径相关，先确认是否已重新构建 C++ server 并把最新资源拷贝到输出目录。
- 已被 shared helper 吸收的测试前提，不要继续在单个 test case 里重复手写。

## 更新本文档

以下变化必须同步更新本文档：

- 新增、删除或重命名验证命令
- `gate:d3`、打包入口或推荐验证顺序变化
- repo / real / perf 测试入口变化
- 测试 helper、请求时序或 runtime 分层变化
- 已确认测试坑点需要制度化，或旧注意事项已被 helper 吸收需要清理
