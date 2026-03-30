# 测试与验证

本文档描述这个仓库当前常用的验证命令，以及什么时候该跑哪一层。

## 验证梯度

按成本从低到高：

1. `npm run json:validate`
2. `npm run compile`
3. `cmake --build .\\server_cpp\\build`
4. `npm run test:client:repo`
5. `npm run test:client:repo:m4`
6. `npm run test:client:all`
7. `npm run test:client:perf`
8. `npm run gate:d3`
9. `npm run package:vsix`

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

### `npm run test:client:repo:m4`

用途：

- 定向运行 `M4` shared analysis context repo 验收
- 当前会顺序执行：
  - `client.analysis-context-shared-key.test.ts`
  - `client.analysis-context-active-unit.test.ts`
  - `client.analysis-context-defines.test.ts`
  - `client.analysis-context-include.test.ts`
  - `client.analysis-context-workspace.test.ts`

适用场景：

- 调整 `AnalysisSnapshotKey`、`ActiveUnitSnapshot`、shared analysis context 失效传播
- 需要稳定验证 `M4` repo 验收，而不想把多个 analysis-context 子场景揉进同一个 VS Code 测试进程

### `npm run test:client:all`

用途：

- 运行 repo 模式 + real workspace 模式

适用场景：

- 发版前
- 需要确认真实工作区不退化

说明：

- `real` 模式依赖外部工作区路径配置，成本比 repo 模式高
- `real` 模式下的 smoke 断言应优先验证“结果存在且链路打通”，不要依赖外部项目里固定不变的引用数或编辑数

### `npm run test:client:perf`

用途：

- 运行 repo 模式下的性能基线 suite
- 采集编辑态请求的端到端 wall-clock 与 client 收到的最新 `nsf/metrics` 快照
- 将报告写到 `out/test/perf-reports/`

适用场景：

- 调整 interactive / diagnostics / deferred-doc / workspace-summary 的调度或 latest-only 策略
- 修改 `document_owner.*`、`document_runtime.*`、`interactive_semantic_runtime.*`、`deferred_doc_runtime.*`
- 需要建立或刷新某个 milestone 的性能基线

说明：

- 当前通过 `--mode perf --file-filter perf` 只加载性能 suite，不会夹带默认 repo correctness suites
- 默认采样规模偏向“先打通基线链路”的 smoke；如需更重采样，可通过环境变量提高 perf suite 的迭代数
- 当前 `client.perf.test.ts` 除 `M0` 基线外，还覆盖：
- `M1` 调度隔离 smoke：`Load-BG` completion、`Load-WS` hover、rapid edit burst 下的 deferred latest-only merge
- `M2` immediate syntax：基于 unmatched-bracket 编辑的 idle / `Load-BG` 延迟基线与 smoke 报告
- `M3` current-doc interactive runtime：member completion / signature help / hover / short-path definition / snapshot reuse
- `M4` shared analysis context / deferred-doc：active unit 切换下的 interactive 延迟，以及 medium / large 文档 semantic tokens 与 inlay hints 预算
- `M5` workspace summary / reverse-include smoke：idle references / prepareRename / rename，以及 `Load-WS` 下的 completion 隔离

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

特别是修改这些模块时，应默认补跑 `npm run test:client:repo`：

- `server_cpp/src/document_owner.*`
- `server_cpp/src/document_runtime.*`
- `server_cpp/src/deferred_doc_runtime.*`
- `server_cpp/src/inlay_hints_runtime.*`
- `server_cpp/src/immediate_syntax_diagnostics.*`
- `server_cpp/src/interactive_semantic_runtime.*`
- `server_cpp/src/workspace_summary_runtime.*`
- `server_cpp/src/main.cpp`
- `server_cpp/src/server_request_handlers.cpp`

其中如果改动涉及 deferred/current-doc cache、full diagnostics 预热/发布链路、inlay hints full-cache/慢路径失效，`npm run test:client:repo` 视为最小必跑项。

如果改动直接涉及这些模块的性能、调度优先级、latest-only / cancellation、metrics 采集或 current-doc cache 命中路径，还应补跑：

- `npm run test:client:perf`

### 发版或大范围重构

跑：

- `npm run gate:d3`
- `npm run package:vsix`

## 集成测试目录

- `src/test/`
  - VS Code 测试入口和测试启动器
- `src/test/suite/*.test.ts`
  - repo / real 模式测试入口文件
  - loader 会按 `*.test.ts` 作为当前事实入口过滤对应的编译产物，避免陈旧 `.js` 输出被重复执行
  - repo 模式当前按架构拆成 `client.runtime-config.test.ts`、`client.interactive-runtime.test.ts`、`client.diagnostics.test.ts`、`client.deferred-doc-runtime.test.ts`、`client.analysis-context-shared-key.test.ts`、`client.analysis-context-active-unit.test.ts`、`client.analysis-context-defines.test.ts`、`client.analysis-context-include.test.ts`、`client.analysis-context-workspace.test.ts`、`client.workspace-summary.test.ts`、`client.references-rename.test.ts`
  - 其中 `client.analysis-context-shared-key.test.ts` 当前承接 `M4` 的 interactive / deferred 共用 analysis key 验收
  - `client.analysis-context-active-unit.test.ts` 当前承接 active unit 驱动的 shared analysis context 验收
  - `client.analysis-context-defines.test.ts` 当前单独承接 `M4` 的 defines 驱动共享 analysis context 验收
  - `client.analysis-context-include.test.ts` 当前单独承接 `M4` 的 include closure 驱动共享 analysis context 验收
  - `client.analysis-context-workspace.test.ts` 当前单独承接 workspace-summary 驱动的 shared analysis context 验收，避免和其他 `M4` 场景互相放大时序噪音
  - perf 模式当前通过 `client.perf.test.ts` 采集主线 wall-clock 与最新 `nsf/metrics` 快照，并输出到 `out/test/perf-reports/`
- `src/test/suite/client.integration.groups.ts`
  - repo 模式共享的 architecture registrar，按层聚合测试定义，供各 `*.test.ts` 入口复用
  - 具体 integration 实现当前位于 `src/test/suite/integration/*.ts`
- `src/test/suite/test_helpers.ts`
  - repo 模式共享 helper
- `test_files/`
  - 夹具文件

如果你修改了行为但没有更新相关夹具或断言，测试很可能只是假通过。

## 对失败结果的处理建议

- 单条集成测试偶发失败时，先重跑一次确认是否稳定复现
- 如果只在一次运行里出现时序型失败，不要立刻修改业务逻辑
- 如果失败与资源或路径相关，先确认是否已重新构建 C++ server 并把最新资源拷贝到输出目录

## 集成测试注意事项

- workspace-summary / include-graph / cross-file 类用例不要默认依赖当前工作区根目录刚好能覆盖目标文件。
  - 这类测试如果需要额外扫描根，应显式设置 `nsf.intellisionPath` 到对应夹具根，再等待 indexing 进入 `Idle`
  - 当前仓库里的 shared helper 已把这件事收敛到 `withTemporaryIntellisionPath(...)`

- 测试里动态修改 `nsf.intellisionPath` 后，不要立刻断言 cross-file 结果。
  - 当前推荐顺序是：更新配置 -> 重启 server -> 等 indexing `Idle` -> 再发 definition / references / rename / diagnostics 请求
  - 恢复原配置后也不要立刻进入下一条 cross-file 用例；shared helper 现在会在 restore 后再次等待 indexing `Idle`
  - client 侧“缺少 intellisionPath”提示在 test mode 下应保持关闭；否则 restore 到空路径后可能弹窗阻塞后续 suite
  - restore 完成后建议再给 client/server 事件队列一个很短的 settle 窗口；shared helper 当前已内置这一等待

- file watch / reverse-include 类用例不要只验证“当前正在编辑的一个 consumer 能刷新”。
  - 当前更推荐至少打开两个依赖同一 provider 的 consumer，其中一个保持 untouched
  - 更新 provider 后只 touch 一个 consumer，再断言另一个 untouched consumer 也完成回流
  - 这样才能覆盖 `workspace_summary_runtime.*` / `workspace_index.*` 提供的 reverse include closure，而不是把结果误判成“仅靠当前文档重排成功”

- 测试如果要在扩展刚激活后立刻修改 `nsf.intellisionPath`、`nsf.defines`、`nsf.inlayHints.*` 这类 runtime 配置，不要先改配置再等 client 起。
  - 当前 shared helper 已补充 `waitForClientReady(...)`
  - 推荐顺序是：触发扩展激活 -> `waitForClientReady(...)` -> 更新配置 -> 再等待 indexing / request 收敛
  - 否则 client 侧配置变更事件可能先于语言客户端 ready，触发 `Language client is not ready yet` 这类时序失败

- semantic tokens / inlay hints / document symbols 属于 deferred 路径。
  - 不要假设第一次请求一定已经就绪
  - 对这类能力应优先使用 `waitFor(...)` 等待 provider / legend / full-cache 进入可断言状态

- 当测试目标是验证 background lane 的 latest-only / cancellation 行为时，不要在单个 test case 里手搓一组原始并发请求。
  - 当前 test mode 已提供内部命令：`nsf._spamInlayRequests`、`nsf._spamDocumentSymbolRequests`、`nsf._spamWorkspaceSymbolRequests`
  - 这些命令会统一统计 `completed / cancelled / failed`，适合做 `M1` 调度隔离与 latest-only smoke
  - 如果后续还要覆盖新的 background 能力，优先按同样方式把并发驱动收敛成共享 test-only command，再写 repo 断言

- 当测试明确依赖某个 `.nsf` 成为 active unit 时，不要完全依赖“打开编辑器后自然收敛”。
  - 当前 test mode 已提供内部命令 `nsf._setActiveUnitForTests`
  - 对 include closure / active unit 相关用例，推荐顺序是：打开目标 `.nsf` -> 调用 `nsf._setActiveUnitForTests(uri)` -> 再发 definition / hover / runtime debug 请求

- 当测试目标是验证“只有命中的 open docs 刷新了 analysis key / workspaceSummaryVersion”时，不要只靠 diagnostics 侧效果间接猜。
  - 当前 shared helper 已补充 `getDocumentRuntimeDebug(...)`
  - 它通过内部命令 `nsf._getDocumentRuntimeDebug` 请求 server 返回当前打开文档的 runtime 摘要
  - 适合用于 reverse-include / workspace-summary 回流断言，直接比较受影响文档和 unrelated open doc 的 analysis fingerprint 是否变化

- interactive completion / hover 用例如果目标是验证 server 返回的特定标签或文案，不要只等“结果非空”。
  - 词法补全、snippets 或较早返回的局部 hover 可能先满足“非空”，但还没到真正想验证的 server 结果
  - 当前 shared helper 已补充 `waitForCompletionLabels(...)`、`waitForHoverText(...)`，优先等目标标签或目标文案出现

- 跨文件或 deferred semantic diagnostics 用例不要只等“已经有 diagnostics”。
  - immediate syntax 和较早的半成品 semantic diagnostics 可能先到，此时仍可能带着旧的 `Undefined identifier`
  - 对这类用例应优先等待目标 mismatch / type error 已出现，且旧的 undefined diagnostics 已消失；当前 shared helper 已补充 `waitForDiagnostics(...)`
  - 如果用例前面刚发生 `intellisionPath` / active unit / indexing 上下文切换，必要时先做一次 noop touch 重新排队当前文档 diagnostics；当前 shared helper 已补充 `touchDocument(...)`
  - 对明显依赖 cross-file summary / active unit 收敛的 diagnostics 用例，优先使用 `waitForDiagnosticsWithTouches(...)`，在等待窗口内周期性重排当前文档

- 对 definition / references 结果，不要在存在同名候选时机械断言“第一个结果必须是某文件”。
  - 如果当前测试目标是“目标候选存在”，优先断言结果集合包含目标路径
  - 只有在当前架构明确承诺了单一当前上下文选主时，才断言首项顺序

- repo 模式测试当前按架构拆分为多个 `*.test.ts` 入口。
  - 定向诊断时可设置环境变量 `NSF_TEST_FILE_FILTER=<substring>`，只加载文件名匹配的 suite
  - `npm run test:client:perf` 当前也通过同一机制只加载 perf suite
  - 该过滤只用于开发诊断，不改变默认 `npm run test:client:repo` 的全量行为

- 单独运行某一组 repo suite 时，内部测试命令可能依赖扩展已激活。
  - 当前 shared helper 已在需要时通过打开 `nsf`/`hlsl` 夹具触发激活
  - 如果手工复现某条测试，先确保扩展和语言模式都已激活

- 当测试 helper、请求时序或 runtime 分层发生变化时，要回头清理这里的注意事项。
  - 已被 shared helper 吸收的前提，不要继续在文档里保留成“每条测试都要手工做”的说明
  - 已不再符合当前架构的注意事项，应在同一次任务里删除或改写，避免把历史 workaround 误写成当前事实

## 何时需要同步更新本文档

以下内容变化时，必须更新本文档：

- 新增或删除验证命令
- `gate:d3` 内容变化
- 推荐打包命令变化
- repo/real 测试入口变化
- perf 测试入口变化
- 验证层级的推荐顺序变化
