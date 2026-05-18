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
  - 构建 C++ server，并通过 `nsf_lsp_resources` 目标把 `server_cpp/resources/` 拷贝到构建输出目录
  - 适用于 `server_cpp/src/` 变更，或需要本地构建产物拿到最新资源时
- `npm run test:client:repo`
  - 运行仓库模式集成测试，使用 `test_files/` 固定夹具
  - 是默认最可信的集成回归入口
- `npm run test:client:repo:m4`
  - 定向运行 shared analysis context repo 验收
  - 当前覆盖 `analysis-context-shared-key`、`analysis-context-active-unit`、`analysis-context-defines`、`analysis-context-include`、`analysis-context-unit-profile` 和 `analysis-context-workspace`
- `analysis-context-unit-profile` 当前同时覆盖 `gimlocalvariants.json` 命中、`used_shader_variants.csv` fallback 命中、`active_unit_variant_selection.csv` 按 unit stem 的 row 选择提示命中，以及“冲突 profile 宏不猜默认；仅作为 unresolved profile metadata 暴露”的边界；并覆盖 workspace 显式宏（`nsf.defines` / `nsf.preprocessorMacros`，含符号链解析为数值）作为 selection hint 覆盖/收敛 profile row 的路径，断言 profile 总行数 / 筛选后行数 / 单行命中 signature / selection hint source path 元数据
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
  - 测试模式会在本次 user-data 中补齐默认 `nsf.preprocessorMacros` preset，使 replay 环境接近普通用户首次填充后的配置
  - 默认跳过 `long-running` 脚本；完整文件输入类 replay 需要显式设置 `NSF_REAL_REPLAY_INCLUDE_LONG=1`，可再配合 `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER` 定向脚本
  - replay sampling window 支持直接声明 `sampleCount` / `sampleIntervalMs`，完整文件输入类用例默认以每类 probe 约 100 个采样点记录性能趋势
  - 完整文件输入类 replay 的 completion probe 应声明 `triggerText`；runner 会先输入到触发文本之前，再逐字符输入触发文本，并以 `TriggerCharacter` 上下文采集补全，避免只在最终位置核对候选表；重型真实输入脚本会启用 `nativeTrigger` 和 suggest / parameter hints UI command，让 VS Code 智能提示路径真实触发
  - 完整文件输入类 replay 可设置 `captureInlayContinuity`；runner 会在 checkpoint 对当前已输入全文范围请求 inlay hints，并在 `fullDocumentTyping.inlayContinuity` 记录样本。已经出现非空 hints 后，如果后续样本变成空/错误再恢复，会报 `inlay-hints-transient-drop`；如果结束时仍为空/错误，会报 `inlay-hints-ended-missing-after-visible`。
  - completion UI 覆盖可用 `NSF_REAL_REPLAY_COMPLETION_UI_MODE` 覆盖触发源：`nativeOnly` 只保留原生 typing / quick suggestion，不调用 `editor.action.triggerSuggest`；`explicitSuggest` 会额外执行显式 suggest UI command。该变量只影响 replay 测量，不改变产品 completion 行为。报告会写入 `completionCapture.uiCoverage.triggerSource` / `uiCoverageTriggerSource`，并在 `latencySummary.completion.uiCoverageByTriggerSource` 按触发源汇总；`explicitInvokeOverlapRequests` 只统计 `explicitSuggest` 测量源下的显式 Invoke 重叠，避免把 native quick-suggest 的 Invoke 形态误归为 replay 显式命令重叠
  - completion / signature replay 报告会保留旧的 capture 聚合耗时，同时按 `uiCoverage` 和 `providerVerification` 拆分真实 UI/native 触发覆盖与 `vscode.execute*Provider` 候选验证；provider 验证前会关闭 UI widget 并等待触发请求队列安静。`uiCoverage` 会记录 provider request sequence、first / last request 相对时间和 burst count；client provider timing 使用 async-local context 将 converter / LSP request 耗时绑定到实际执行 `next(...)` 的 provider draft，completion provider timing 会携带 `completionDebugRequestId` 并由 server debug history 回传 `nsfDebugRequestId`，避免 coalescing 延迟、并发 provider 或晚完成 stale 请求把旧 server debug 归到新 request sequence；provider request sequence 还会记录 `nextWaitMs`、`lspStartDelayMs`、`activeSameKindProviderCountAtStart`、`activeSameKindNextCountAtStart` 和关键阶段 document version / dirty 状态，用于区分 coordinator 等待、sendRequest promise 内部等待、请求重叠和文本同步推进；completion debug snapshot 还会记录 client send-start、server received、server worker start、server response-write-completed wall-clock timestamp，以及 completion 被 server 收到前 didChange 主输入线程处理的重叠摘要，用于把 `sendRequest` 拆成 client-to-server-send、server didChange input-thread blocking、server handling 和 server-response-to-client-resolve；报告顶层 `latencySummary` 会汇总 P50/P95/max、最慢 probe、UI queue quiet 超时、UI request burst、latest visible provider return、latest visible next wait / LSP start delay / LSP request / client-to-server received / server didChange overlap / server handler / server response to client resolve / client residual、post-latest cleanup、post-latest provider activity、post-latest quiet guard 和 duplicated request path。`postLatestVisibleCleanup` 保留为“latest visible return 到 queue quiet 完成”的总量；归因时应优先看 `postLatestVisibleProviderActivity` 与 `postLatestVisibleQuietGuard` 的拆分，避免把 replay quiet guard 误判为仍在执行的 provider cleanup。
  - 顶层 `captureCompletion` / `captureSignatureHelp` step 和完整文件输入 probe 都会进入 `latencySummary`。signature replay 的 `latencySummary.signatureHelp` 包含 `uiLatestVisibleProviderReturn`、`uiLatestVisibleLspRequest`、`postLatestVisibleProviderActivity`、`postLatestVisibleQuietGuard` 和 `uiQueueQuietGuard`；分析 signature help 尾延迟时应优先区分 latest visible provider/LSP 是否慢，以及 latest visible 返回后的时间是仍有 provider activity 还是只是 replay queue-quiet guard。
  - completion replay 的 `latencySummary.completion.coalescingSimulation` 是报告层模拟，不改变运行时 completion 行为；它基于 `uiCoverage.providerRequestSequence` 估算 identifier-prefix auto-trigger burst 在 25ms / 40ms / 60ms 短窗口下会保留和丢弃哪些 request sequence，并显式保留 explicit invoke、`.` member completion 和无法安全归类的请求
  - completion replay 的 `latencySummary.completion.coordinatorActual` 汇总 client completion request coordinator 的真实运行结果，包括 received、executed LSP、coalesced-before-LSP、stale-resolved-while-in-flight、stale-dropped-after-LSP、cancelled-before-LSP、cancelled-while-in-flight、各类 bypass 以及 retained / dropped request sequence；Phase B 之后分析产品路径应优先看 `coordinatorActual`，`coalescingSimulation` 只作为规则对照
  - completion replay 的 `latencySummary.completion.uiExecutedAttribution` 用于拆解 UI/native trigger 路径里实际执行到 LSP 的 coordinator request，报告 executed LSP request 耗时、latest executed request 完成前等待和完成后的 quiet/cleanup 等待；runner 会在 provider verification 前抓取 server `lastCompletionDebug` 及其 recent history，并优先按 `completionDebugRequestId` / `nsfDebugRequestId` 关联 `latestExecutedServerAttribution`；如果 client request id 存在但 server history 未命中，报告会记入 `serverDebugRequestIdUnmatchedCount` 且不回退套用 last debug。`latestExecutedClientAttribution`、`latestExecutedServerAttribution` 和汇总 stats 会拆出 client next wait / LSP start delay / in-flight overlap / document version 变化、server queue wait、request context build、completion handler 和剩余 client/transport LSP 时间；当 `coordinatorActual` 已经减少可见旧请求但 `uiQueueQuiet` 仍高时，应结合 detached cleanup、server debug id matched/unmatched、server queue 和 client residual 字段判断剩余瓶颈
  - diagnostics probe 可显式配置 `requireRuntimeReady`、`touchEveryMs` / `maxTouches`；runner 会记录 runtime ready 和 touch 次数，用于区分自然稳定时间和需要重新排队后才发布的 full diagnostics
- real workspace diagnostics audit
  - 定向统计真实 workspace 的 `.nsf/.hlsl` 诊断，报告写入 `out/test/diagnostics-audit/`
  - 统计口径是 `.nsf` unit：runner 逐个设置 active unit，使用 server indexed include closure 枚举该 unit 触达的 `.nsf/.hlsl`，再按当前 diagnostics 配置直接构建诊断快照；共享 include 会按使用它的 unit 重复计数
  - 测试模式不会污染真实 workspace；如果 workspace 未显式配置 `nsf.preprocessorMacros`，audit 会把 server registry 的默认 preset 写入本次测试专用 user-data 配置，用来模拟普通用户首次填充后的分析环境
  - 扫描范围优先使用 `nsf.intellisionPath` 配置的 shader 根；未配置时才退回 workspace folders，避免把编译临时产物和工具目录误当源码统计
  - 每次 audit 都会写入 timestamp 归档和 `real-workspace-diagnostics-audit.latest.{json,md}`；设置 `NSF_REAL_DIAGNOSTICS_REPORT_LABEL` 时会额外写入 `real-workspace-diagnostics-audit.<label>.{json,md}`，阶段验证推荐使用 `phase-XX-<topic>-smoke-5` / `phase-XX-<topic>-trend-50` / `phase-XX-<topic>-full` 这类稳定 label
  - 报告会自动生成 baseline trend，比较 summary、triage、category 和 top canonical messages；5-unit 优先对比 `phase-00-baseline-smoke-5`，50-unit 优先对比 `phase-00-baseline-trend-50`，full audit 对比 `baseline-2026-05-16`，缺少同范围 baseline 时回退到 2026-05-16 full baseline；可用 `NSF_REAL_DIAGNOSTICS_BASELINE_JSON` 指向其他 baseline，或设为 `none` 禁用比较
  - 报告会把 `Undefined macro in preprocessor expression` 拆成 `undefinedMacros` histogram，记录 macro name、diagnostic count、affected unit / file count、sample line、sample active unit，以及 enum-like stable constant、selector/profile、compiler context 和 source/generated config 的 owner hint；owner hint 只用于审计分流，不会改变 diagnostics 行为或自动补默认宏。分析宏缺口时必须区分 `SHADINGMODELID_DEFAULT_LIT` 这类稳定常量和 `SHADINGMODELID` 这类 profile selector：前者可在确认来源和值后进入稳定常量候选，后者仍应来自真实 compile profile 或 workspace 配置；如果同名 enum-like 候选在不同参数 include 中存在冲突值，只记录为待确认候选，不得直接补全局默认值
  - 报告 summary / fileStats 会记录 semantic rule prerequisites skipped metadata，包括 active unit、include closure、preprocessor context、parser region、semantic snapshot、local scope 和 expression type 相关 skipped reason；这些计数表示高置信 semantic diagnostics 因上下文前提不足被跳过，不是用户可见 diagnostics
  - 趋势判断应优先核对 `diagnosticsTotal`、triage/category delta、top message delta、affected units/files，以及 `truncatedFiles`、`timedOutFiles`、`fileErrors` 是否增加；如果 truncated / timeout 增加，阶段报告必须单独说明原因
  - 合法但有风险的隐式转换 warning 会归入 `type-conversion-risk` / `needs-manual-review`，包括 truncation、boolean、floating-integral、signedness 和 narrowing；这些 warning 默认不在 `balanced` mode 发布，只有 `full` mode 用于源码审核或专项治理时才应出现在 audit 中。分析 builtin / object method 阶段时应区分 mismatch 下降与风险 warning 上升
  - 默认不随 real suite 执行；需要显式设置：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

  - 可选限制 unit 数量：`$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "100"`；历史兼容变量 `NSF_REAL_DIAGNOSTICS_MAX_FILES` 仍会作为未设置 `MAX_UNITS` 时的 fallback
  - 可选限制单个 unit include closure：`$env:NSF_REAL_DIAGNOSTICS_CLOSURE_LIMIT = "1024"`
  - 可选限制写入 JSON 的诊断样本数量：`NSF_REAL_DIAGNOSTICS_SAMPLE_PER_GROUP`、`NSF_REAL_DIAGNOSTICS_SAMPLE_PER_UNIT`、`NSF_REAL_DIAGNOSTICS_SAMPLE_MAX_TOTAL`
  - 5-unit smoke audit 推荐命令：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "5"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "600000"
$env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL = "phase-XX-topic-smoke-5"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

  - 50-unit trend audit 推荐命令：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "50"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "1800000"
$env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL = "phase-XX-topic-trend-50"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```
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
- 改 completion auto-trigger coordinator：至少补跑 completion request coordinator 单元测试、completion auto-trigger、completion client metrics、member completion、interactive visibility、real-workspace-replay repo 定向测试和 `pbr-flow-water-full-input` real replay
- 改预处理宏资源、active-unit compile profile 宏提供链路或 active-unit include 预处理上下文：补跑 `npm run json:validate`（仅资源改动时）、`cmake --build .\server_cpp\build`、`npm run test:client:repo:m4` 和 diagnostics repo 集成用例
- 改 semantic diagnostics 的 local scope、`for` initializer 可见性、duplicate local 或基础 control-flow 规则：至少补跑 diagnostics repo 集成用例、5-unit smoke audit 和 50-unit trend audit
- 改 missing-semicolon parser boundary、macro-heavy recovery 或 local structural syntax 前提：至少补跑 diagnostics repo 集成用例、5-unit smoke audit 和 50-unit trend audit
- 改 deferred/current-doc cache、full diagnostics 预热/发布链路、inlay hints full-cache 或慢路径失效：`npm run test:client:repo` 是最小必跑项
- 改 builtin overload 或 object method diagnostics 参数匹配：至少补跑 diagnostics repo 集成用例、5-unit smoke audit 和 50-unit trend audit，并核对 `call-type-analysis` 与 `type-conversion-risk` 的迁移关系
- 改 semantic diagnostics prerequisites、debug/audit skipped metadata 或高置信 diagnostics 发布前提：至少补跑 diagnostics repo 集成用例、5-unit smoke audit 和 50-unit trend audit，并核对 `prerequisiteSkippedTotal` / skipped reason、`indeterminate-analysis`、`undefined-identifier`、`semantic-source-rule` 和 `expression-type-analysis` 的迁移关系
- 改调度优先级、latest-only、cancellation、metrics 或性能命中路径：补跑 `npm run test:client:perf`
- 发版或大范围重构：跑 `npm run gate:d3` 和 `npm run package:vsix`

编辑器壳层配置变更还要手工 smoke：

- `.nsf`、`.hlsl` 的语言模式归属
- `Ctrl+/` 行注释、`Shift+Alt+A` 块注释
- 自动配对、包裹、基础缩进和 `wordPattern`
- `///`、`/** */` 注释续写
- `// #region` / `// #endregion` 折叠，并确认嵌套区域可折叠
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

real replay 可按脚本 ID 过滤；长链路完整文件输入用例默认不进入短回归：

```powershell
$env:NSF_REAL_REPLAY_INCLUDE_LONG = "1"
$env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER = "pbr-flow-water-full-input"
npm run test:client:real:replay
```

`pbr-flow-water-full-input` 会从空 buffer 输入完整节点文件，按成员补全、texture method、函数族补全、内置函数、局部变量、uniform、预处理宏、signature help 和最终 diagnostics 分组采样；脚本属于重型验证，单次运行可能需要数分钟。

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
- `didChange` 后的 local structural snapshot 由异步 fast diagnostics worker 补齐；runtime 断言应通过 `waitFor(...)` 轮询 `getDocumentRuntimeDebug(...)`，不要假设 `applyEdit(...)` 返回后同步 ready。
- 跨文件或 deferred semantic diagnostics 用例应使用 `waitForDiagnostics(...)`；必要时用 `touchDocument(...)` 或 `waitForDiagnosticsWithTouches(...)` 重新排队。
- definition / references 存在同名候选时，优先断言结果集合包含目标路径；只有当前架构明确承诺选主顺序时才断言首项。

### 并发与性能

- background lane latest-only / cancellation 用例不要手搓原始并发请求；优先使用 test mode 内部命令 `nsf._spamInlayRequests`、`nsf._spamDocumentSymbolRequests`、`nsf._spamWorkspaceSymbolRequests`。
- visible-range inlay / perf 用例不要把冷请求固定绑定为 range-build；range-build 或基于 full-cache 的 range-filter 都可能是合法路径。
- real workspace 测试不要依赖外部工程里某一整行固定文本永远不变；优先用稳定前缀定位并缓存原始文本。
- real replay 脚本解析锚点前会把已打开的 dirty 文档恢复到磁盘文本基线，避免前置 real workspace 用例的未保存 buffer 污染后续锚点解析。
- real diagnostics audit 只做统计和初筛分类，不作为通过/失败门禁；`likely-plugin-limitation`、`likely-real-source` 和 `check-config-or-source` 都是 triage hint，最终归因仍需要结合样例行、include context 和真实编译结果复核。

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
