# NSF Diagnostics Upgrade Execution Plan

## 文档定位

本文是基于 2026-05-16 真实工作区 `.nsf` unit diagnostics 全量审计形成的可执行升级计划。它属于 `docs/human-ai/` 协作沉淀，不是当前事实文档；当某个阶段落地并改变架构、资源、测试策略或公开 diagnostics 行为时，必须同步更新对应当前事实文档。

相关输入：

- `docs/human-ai/2026-05-16-nsf-unit-diagnostics-audit-plan.md`
- `out/test/diagnostics-audit/real-workspace-diagnostics-audit.latest.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
- `docs/type-method-interface-contract.md`

核心约束：

- `.nsf` 是真实 diagnostics 审计和 active analysis 的 unit。
- 不在 diagnostics rule 中新增 suppress、fallback、shim 或局部特判来掩盖共享层缺陷。
- 语言知识、类型兼容、对象方法、预处理宏和 include context 必须通过共享入口维护。
- 任何改变公开 diagnostics 行为的实现阶段，开工前必须确认风险和预期行为变化。

## 总体背景

2026-05-16 全量审计覆盖真实工作区 `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`，以 `.nsf` 为 unit，逐个设置 active unit，并通过 server indexed include closure 构建该 unit 触达的 `.nsf/.hlsl` diagnostics。

审计结果：

| 指标 | 数量 |
| --- | ---: |
| `.nsf` units discovered / scanned | 813 / 813 |
| units with diagnostics | 786 |
| unit file visits | 25985 |
| unique files discovered / scanned | 1191 / 1191 |
| unique files with diagnostics | 750 |
| diagnostics total | 463556 |
| truncated file builds | 1 |
| timed-out file builds | 1 |
| file errors | 0 |

triage 分布：

| Triage | Count |
| --- | ---: |
| `likely-plugin-limitation` | 373861 |
| `needs-manual-review` | 53329 |
| `check-config-or-source` | 36363 |
| `likely-real-source` | 3 |

主要问题类别：

| Category | Count |
| --- | ---: |
| `expression-type-analysis` | 257008 |
| `undefined-identifier` | 53071 |
| `call-type-analysis` | 38920 |
| `preprocessor-context` | 36363 |
| `semantic-source-rule` | 36042 |
| `numeric-literal` | 15676 |
| `other` | 9432 |
| `effect-syntax-or-macro` | 9178 |
| `syntax-structure` | 7858 |
| `indeterminate-analysis` | 8 |

当前结论：

1. 最大缺陷不是单点规则错误，而是 HLSL 类型兼容、作用域模型、预处理上下文、parser recovery 和 diagnostics 发布前提之间缺少统一架构契约。
2. 大量 diagnostics 是共享层缺陷的级联结果，不能通过局部忽略或 message-level suppress 解决。
3. 后续治理必须以架构收敛为主线：先统一输入上下文和共享语义，再提高 diagnostics 可信度。

## 总体目标

1. 让 `.nsf` unit 成为 diagnostics、include closure、宏环境、semantic snapshot 和审计统计的一致分析边界。
2. 将 HLSL 类型兼容、对象方法参数形状、builtin overload、局部作用域和预处理宏等共享事实收敛到单一入口。
3. 显著降低 `likely-plugin-limitation` 类误报，使剩余 diagnostics 可以进入真实源码审核。
4. 为每个大类缺陷建立可重复验证路径：focused fixture、5-unit smoke、50-unit trend、必要时 full 813-unit audit。
5. 每次架构或公开行为变化都同步更新事实文档和头文件接口契约，避免方案只停留在实现细节。

## 总体验收标准

满足以下条件后，才认为本轮 diagnostics 升级完成：

1. full audit 中 `likely-plugin-limitation` 不再由 numeric literal、基础 HLSL 类型兼容、loop scope 或已确认 builtin macro 缺陷主导。
2. top message groups 能被分为明确的真实源码问题、真实配置问题、待规则确认问题，而不是 parser/type/scope 级联误报。
3. diagnostics 规则消费共享语义入口，不再在 request 或 feature 代码中复制类型表、宏表、对象方法适配规则。
4. 每个已修复类别至少有一个 repo fixture 或稳定 real audit sample 作为回归锚点。
5. `docs/architecture.md`、`docs/resources.md`、`docs/testing.md`、`docs/type-method-interface-contract.md` 在涉及对应边界变化时已经同步更新。

## Phase 0: 固化审计基线和趋势门禁

### 背景

当前已具备 unit-based audit，但后续每个阶段都需要可比较的趋势数据。没有稳定基线时，diagnostics 数量下降可能只是分类变化、样本变化或上下文缺失造成的假象。

### 目标

建立后续阶段统一使用的基线、样本集和趋势判断规则，使每次修复都能回答：

- 哪类 diagnostics 下降了。
- 哪些 top message 被消除或迁移了。
- 是否出现新的高频误报。
- 是否存在 truncated / timeout 导致统计不可比。

### 方案

1. 保留当前 latest audit 输出作为 2026-05-16 baseline。
2. 将后续审计结果按阶段命名保存，建议格式：
   - `real-workspace-diagnostics-audit.phase-01-numeric-literal.md`
   - `real-workspace-diagnostics-audit.phase-01-numeric-literal.json`
3. 在 audit report 中持续关注以下维度：
   - total diagnostics
   - triage summary
   - category summary
   - top canonical messages
   - affected units
   - affected files
   - truncated / timeout counts
4. 建立阶段通用验证顺序：
   - focused fixture
   - `npm run compile`，如果 TypeScript audit/test 代码变化
   - `cmake --build .\server_cpp\build`，如果 C++ server 或资源拷贝相关变化
   - 5-unit smoke audit
   - 50-unit trend audit
   - full 813-unit audit，仅在高频类别治理阶段或阶段收尾时执行
5. 如果发现同一测试坑点影响 2 条以上测试，按仓库制度沉淀到 shared helper 或 `docs/testing.md`。

### 验收标准

- 可以从 baseline 和阶段报告中直接比较 category / triage / top message 的变化。
- 5-unit 和 50-unit audit 命令可稳定复跑。
- full audit 中 truncated / timeout 未增加；如增加，必须单独说明原因。
- 阶段报告能指出“下降来自修复”，而不是分类器或样本范围变化。

### Phase 0 执行记录

状态：已落地 audit 趋势基础设施。

实现内容：

- 已将当前 `real-workspace-diagnostics-audit.latest.{json,md}` 本地冻结为 `out/test/diagnostics-audit/real-workspace-diagnostics-audit.baseline-2026-05-16.{json,md}`。
- audit runner 支持 `NSF_REAL_DIAGNOSTICS_REPORT_LABEL`，每次运行除 timestamp 与 latest 外，可额外生成 `real-workspace-diagnostics-audit.<label>.{json,md}`。
- audit runner 默认生成 baseline trend；5-unit 优先对比 `phase-00-baseline-smoke-5`，50-unit 优先对比 `phase-00-baseline-trend-50`，full audit 对比 `baseline-2026-05-16`，缺少同范围 baseline 时回退到 2026-05-16 full baseline；也可通过 `NSF_REAL_DIAGNOSTICS_BASELINE_JSON` 指向其他 baseline，或设为 `none` 禁用比较。
- baseline trend 覆盖 summary、triage、category 和 top canonical message delta，并在 `truncatedFiles`、`timedOutFiles` 或 `fileErrors` 增加时写出 warning。
- `docs/testing.md` 已升格记录阶段 label、baseline trend、5-unit smoke 和 50-unit trend audit 复跑命令。

验证结果：

- `npm run compile` 通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-00-baseline-smoke-5.{json,md}`；同范围 baseline trend delta 为 0，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-00-baseline-trend-50.{json,md}`；同范围 baseline trend delta 为 0，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 未重跑 full 813-unit audit；P0 使用已冻结的 2026-05-16 full latest 作为 full baseline。

阶段关闭判断：

- 命令是否变化：未新增 npm 命令；新增 audit 环境变量用法。
- 路径或命名是否变化：新增可选阶段报告命名和 2026-05-16 baseline 文件名；未改变默认 latest / timestamp 输出。
- 架构或单一事实来源是否变化：否，仅测试/audit 报告工具变化。
- 测试策略是否变化：是，阶段 audit 现在以 baseline trend 作为统一比较口径。
- 文档是否已同步：已更新 `docs/testing.md`。
- 是否改变公开 diagnostics 行为：否。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：P0 不修具体 diagnostics 类别，使用 2026-05-16 full audit baseline 作为稳定 real audit sample。

## Phase 1: 修复 numeric literal parser

### 背景

审计中 `numeric-literal` 共 15676 条，典型误报是 `1e-5` 被诊断为 `Invalid numeric literal suffix`。这是明确的 lexer / numeric literal parser 缺陷，会污染表达式类型推断，并可能级联出 assignment、return、call mismatch。

### 目标

按官方 HLSL numeric literal 语法识别数值字面量，消除 exponent literal 被误判为非法 suffix 的问题，并把历史/项目猜测写法从 parser 政策里剥离。

### 方案

标准依据：

- Microsoft HLSL working draft 的 lexical grammar 中 numeric literal 覆盖 decimal / octal / hexadecimal integer literal、fractional / exponent floating literal，以及 integer / floating suffix；其中 `ll/ull` 属于 implementation may support 的非核心 integer suffix。
- Microsoft Learn 的 HLSL appendix grammar 同样把 decimal point、exponent、`h/H/f/F/l/L` 浮点 suffix 和 `u/U/l/L` 整数 suffix 作为 numeric literal 语法组成部分。

1. 定位 numeric literal 解析入口，优先修共享 parser / lexer，不在 diagnostics rule 中新增 suppress。
2. 支持并测试以下官方 HLSL 合法形式：
   - `1e-5`
   - `1E+5`
   - `1e5`
   - `1.0e-5`
   - `.5`
   - `1.`
   - `1.e-5`
   - `1.0e+5f`
   - `1.0h` / `1.0H`
   - `1.0L`
   - `42u` / `42U` / `42l` / `42L` / `42ul` / `42LU`
   - `077` / `077ul`
   - `0xFFu` / `0xFFL` / `0xFFul` / `0xFFLU`
3. 对 `42ll` / `42ull` / `42llu`、`0xFFll` / `0xFFull` / `0xFFllu` 这类 implementation-only 历史写法保持可识别，但发布 warning，推荐规范写法 `l` / `ul`。
4. 明确非法 suffix 场景仍应保留 error diagnostics，例如无法归入 HLSL numeric literal 的尾部字符；`1.0u`、`1.0q`、`1e-5q`、`1.0fQ`、`42ulu`、`08`、`09u`、`0x1p`、`0x1p2`、`0x1ulu` 这类非官方语法内写法应继续 error。
5. 新增 focused repo fixture，覆盖合法 exponent、合法 suffix、历史 suffix warning、非法 suffix error 四类。
6. 修复后更新 audit classifier 的预期，不再把 exponent literal 作为插件缺陷样本留在 top group。

### 验收标准

- focused fixture 中合法 exponent literal 不再产生 `Invalid numeric literal suffix`。
- 历史 `ll/ull` suffix 产生 warning，非法 suffix 产生 error。
- 5-unit audit 中 exponent literal 样本消失。
- 50-unit audit 中 `numeric-literal` 类别显著下降。
- 如果 public diagnostics 行为变化，最终说明明确记录变化范围。

### Phase 1 执行记录

状态：已落地 numeric literal parser 修复。

实现内容：

- `diagnostics_expression_type.*` 新增 token-span aware `parseNumericLiteralFromTokens(...)`，统一识别官方 HLSL decimal / octal / hex / exponent numeric literal 和合法 suffix。
- semantic diagnostics 的 `Invalid numeric literal suffix` 发布改为消费共享 numeric literal parse result，不再在规则层复制 hex / decimal suffix 分支。
- implementation-only `ll/ull` integer suffix 由共享 parser 识别为 legacy form，semantic diagnostics 发布 `Deprecated numeric literal suffix: <suffix>. Use <suffix>.` warning，推荐标准 `l/ul` 写法。
- 真正非法 numeric literal suffix 的 severity 调整为 Error。
- expression type 推断在遇到 numeric literal 时会消费完整 token span，避免 `1.0e-5` 里 exponent sign 后续被当成独立表达式片段。
- `diagnostics_expression_type.*` 同步把 `double` 纳入 diagnostics expression typing 的 scalar/vector/matrix 与 builtin numeric promotion，保证 `l/L` 浮点 suffix 不只是消除 suffix error，而能参与类型推断和 half narrowing warning。
- 新增 focused fixture `test_files/module_diagnostics_numeric_literal_exponent.nsf`，覆盖 `1e-5`、`1E+5`、`1e5`、`1.0e-5`、`.5`、`1.`、`1.e-5`、`.5e+1`、`1.0e+5f`、`1.0h/H`、`1.0L`、`42u/U/l/L/ul/LU`、`077/077ul`、`0xFFu/U/l/L/ul/LU`，以及 `ll/ull/llu` legacy suffix warning。
- 新增 focused fixture `test_files/module_diagnostics_numeric_literal_invalid_suffix.nsf`，覆盖 `1q`、`1.0q`、`1e-5q`、`1.0e+5q`、`1.0fQ`、`1.0hQ`、`1.0u`、`42ulu`、`08`、`09u`、`0x1p`、`0x1p2`、`0x1ulu`。
- real diagnostics audit classifier 已把 `Deprecated numeric literal suffix` 归入 `numeric-literal` / `check-config-or-source`，避免同类趋势分散。
- `docs/architecture.md` 和 `diagnostics_expression_type.hpp` 已记录 diagnostics expression type helper 对官方 HLSL numeric literal token-span 解析的共享契约。

公开行为变化：

- 合法 HLSL exponent、leading/trailing dot、octal / hex integer、`h/H/f/F/l/L` 浮点 suffix 和 `u/U/l/L` 整数 suffix 组合不再产生 `Invalid numeric literal suffix` diagnostics。
- `ll/ull` implementation-only 历史 suffix 产生 warning：`Deprecated numeric literal suffix: <suffix>. Use l/ul.`。
- 非法 numeric literal suffix 继续产生 `Invalid numeric literal suffix: <suffix>.` error diagnostics。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，65 passing / 1 pending；覆盖 official 合法 numeric literal、legacy suffix warning、invalid suffix error 和 severity 断言。
- `$env:NSF_TEST_FILE_FILTER='member-completion-matrix'; npm run test:client:repo` 通过，1 passing；用于复核前一轮 full run 中 member completion matrix 的原生 typing / 文档污染型偶发失败。
- `npm run test:client:repo` 全量重跑通过，覆盖 compile + 56 个 repo 集成测试文件；前序 full run 中出现过的 `Completion Auto Trigger` typing edit 超时和 `member-completion-matrix` 文档污染型失败均未复现，判断为编辑器测试偶发问题，非本次 diagnostics 共享解析变更回归。
- `git diff --check` 通过；仅输出 Windows 工作区 CRLF 转换提示，无 whitespace error。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-01-numeric-literal-smoke-5.{json,md}`；`numeric-literal` 从 149 降到 0，`diagnosticsTotal` 从 4947 降到 4793，`undefined-identifier` 保持 531，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-01-numeric-literal-trend-50.{json,md}`；`numeric-literal` 从 1377 降到 0，`diagnosticsTotal` 从 43341 降到 41900，`undefined-identifier` 保持 4842，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和阶段 audit 报告；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，numeric literal token-span 解析收敛到 `diagnostics_expression_type.*` 共享入口。
- 测试策略是否变化：是，P1 focused fixture 补齐官方合法 / 非法 numeric literal 正反用例；audit 趋势验证仍沿用 P0 机制。
- 文档是否已同步：已更新 `docs/architecture.md`、`diagnostics_expression_type.hpp` 和本执行计划；资源、测试命令和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，合法 HLSL numeric literal 不再误报非法 suffix，`ll/ull` 历史 suffix 改为 warning，非法 suffix 保留 error。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-01 5-unit / 50-unit real audit 报告。

## Phase 2: 建立共享 HLSL 类型兼容模型

### 背景

最大误报来源是类型兼容：

- `Assignment type mismatch` 223382 条。
- `Return type mismatch` 27151 条。
- `Builtin call type mismatch` 16656 条。
- `Built-in method call type mismatch` 13855 条。

这些问题高度相似，说明 assignment、return、function argument、builtin overload、object method call 没有统一消费同一套 HLSL 兼容规则。继续在各 diagnostics rule 中局部修补会扩大长期维护成本。

### 目标

在共享层建立统一 HLSL 类型兼容模型，所有 diagnostics consumer 使用同一入口判断类型兼容。

### 方案

1. 设计共享接口，建议职责放在 `type_model.*` 或 diagnostics 下的共享 type compatibility 模块；最终位置以现有职责边界为准。
2. public API 至少覆盖：
   - assignment compatibility
   - return compatibility
   - function argument compatibility
   - builtin overload argument compatibility
   - object method argument compatibility
   - binary operator operand compatibility
3. 先实现类型归一化：
   - `half` / `float` / `double`
   - `int` / `uint` / `bool`
   - scalar / vector / matrix
   - same-width vector family conversion
   - scalar-to-vector splat
   - numeric literal 默认类型
   - typedef / macro-like alias，例如 `MaterialFloat4`
4. 兼容判断返回结构化结果：
   - compatible / incompatible
   - conversion kind
   - reason code
   - normalized expected / actual type
5. 将 expression diagnostics、return diagnostics、builtin diagnostics、object method diagnostics 逐步迁移到共享入口。
6. 不在 request handler、hover、completion 或 diagnostics rule 中复制局部类型表。
7. 修改 `*.hpp` public API 时，同步补齐职责、输入输出、调用前提和非目标范围说明。

### 验收标准

- `expression-type-analysis` 在 50-unit audit 中大幅下降。
- `Assignment type mismatch`、`Return type mismatch` 中 half/float、floatN/halfN、literal splat 类样本消失。
- builtin 和 object method diagnostics 复用同一兼容入口，而不是各自维护兼容分支。
- 新增 focused fixture 覆盖 assignment、return、function argument、builtin argument、object method argument。
- 如 `type_model.*` 或对象方法共享契约变化，已更新 `docs/type-method-interface-contract.md` 和相关头文件注释。

## Phase 3: 重建局部作用域和控制流模型

### 背景

审计中 `undefined-identifier`、`Duplicate local declaration`、`Unreachable code`、`Potential missing return` 存在明显污染。样本显示 `for` loop initializer 变量、嵌套 block、branch scope 和 parser recovery 状态会影响语义 diagnostics 的可信度。

### 目标

让 local symbol、duplicate declaration 和基础 control-flow diagnostics 基于可靠 lexical scope / block flow，而不是基于粗粒度扫描或不完整 parser 状态。

### 方案

1. 审查 `semantic_snapshot.*` 中 local variable 提取和可见性建模。
2. 建立或完善 scope tree：
   - function scope
   - block scope
   - `for` initializer scope
   - branch scope
   - nested block scope
3. 明确 `for (int i = ...; ...)` 的可见范围：
   - condition / iteration / body 可见
   - loop 外不可见
4. duplicate local declaration 只在同一 lexical scope 内判重，不跨 sibling block 误报。
5. unreachable / missing return 规则只在 scope 和 parser region 可靠时执行。
6. 如果现有 AST 不能可靠支撑 control-flow，先降低该类 diagnostics 产生条件，再补 CFG 或简化 block flow；不得新增静默兜底。
7. 新增 fixture 覆盖：
   - loop variable 可见性
   - sibling block 同名变量
   - nested block shadow
   - early return 后 unreachable
   - if/else all-return 和 partial-return

### 验收标准

- `Undefined identifier: i` 等 loop variable 误报消失。
- `Duplicate local declaration` 在 50-unit audit 中显著下降。
- `Unreachable code` 只在明确 control-flow 下出现，不再跟随 parser recovery 大面积级联。
- scope API 或 semantic snapshot 契约变化已更新对应头文件说明和 `docs/architecture.md`。

## Phase 4: 对齐预处理宏和真实 unit 编译上下文

### 背景

`preprocessor-context` 共 36363 条，典型样本是 `API_MOBILE_HIGH_QUALITY`、`API_SUPPORT_SV_INSTANCE_ID` 等平台/API 宏 undefined。需要判断它们是真实源码配置缺失，还是 LSP 没有同步 shadercompiler / workspace 的默认宏事实。

### 目标

让 `.nsf` unit 的 preprocessor environment 与真实编译输入一致，并明确 builtin preset、用户配置、源码 `#define/#undef` 的职责边界。

### 方案

1. 拉取或确认 shadercompiler 默认宏来源，建立宏来源审计表：
   - shadercompiler builtin macro
   - workspace `nsf.preprocessorMacros`
   - workspace `nsf.defines`
   - `.nsf` metadata / unit-specific config
   - source `#define/#undef`
2. 对 high-frequency undefined macro 分组：
   - confirmed builtin macro
   - workspace config macro
   - source-defined but include context missing
   - real source/config issue
3. confirmed builtin macro 只能通过 `language/preprocessor_macros` bundle 和生成脚本进入默认 preset。
4. workspace-specific macro 通过配置同步进入，不进入正式资源 bundle。
5. `preprocessor_view.*` 继续作为 `#if/#elif` 求值和 include 链宏传播唯一入口。
6. 修改资源时按 `docs/resources.md` 执行：
   - 修改 bundle
   - 必要时更新 schema
   - `npm run json:validate`
   - `cmake --build .\server_cpp\build`
7. 避免在 diagnostics rule 中对某些 macro name 直接放行。

### 验收标准

- 高频 undefined macro 被归因到 builtin、workspace config、source/config issue 中的一类。
- confirmed builtin macro 不再产生 undefined macro diagnostics。
- `preprocessor-context` 在 50-unit audit 中显著下降，或被明确拆分为真实配置问题。
- 资源变化已更新 `docs/resources.md`；预处理上下文契约变化已更新 `docs/architecture.md`。

## Phase 5: 修复 HLSL / NSF parser boundary 和 recovery

### 背景

`effect-syntax-or-macro` 和 `syntax-structure` 中仍有大量 `Missing semicolon`。样本涉及 multiline function signature、complex conditional、NSF/effect syntax 和 macro-heavy 区域。parser boundary 错误会级联污染 undefined、duplicate、unreachable 和 type diagnostics。

### 目标

提高 HLSL / NSF parser 对真实语法结构的覆盖，并让 parser recovery 产出的不可信区域不会继续触发高置信 semantic diagnostics。

### 方案

1. 按 top samples 分类 parser failure：
   - multiline function signature
   - macro-wrapped parameter list
   - attribute / semantic suffix
   - NSF metadata / effect block
   - complex condition with nested parentheses
2. 优先修共享 parser / syntax boundary，不在 diagnostics 中按 message 局部跳过。
3. 对 parser recovery 输出增加可信区间或错误区域标记。
4. semantic diagnostics 在不可信 AST 区域内不发高置信规则诊断；如需要输出，应使用明确的 syntax diagnostics，而不是级联语义错误。
5. 新增 fixture 覆盖每类 parser failure。
6. 如果 NSF/effect syntax 支持边界变化，同步更新相关当前事实文档。

### 验收标准

- 合法 multiline signature 和 complex condition 不再报 `Missing semicolon`。
- parser recovery 不再导致同一区域出现大量 undefined / duplicate / unreachable 级联。
- `effect-syntax-or-macro`、`syntax-structure` 在 50-unit audit 中下降。
- parser 或 syntax diagnostics 公开行为变化已记录并验证。

## Phase 6: 统一 builtin overload 和 object method 匹配

### 背景

`Builtin call type mismatch`、`Built-in method call type mismatch`、`Function call argument mismatch` 与 Phase 2 的类型兼容相关，但还涉及 builtin overload、object method、texture family、array coordinate dimension 和 label 参数拆分。

### 目标

让 builtin 和 object method 的参数匹配统一消费共享类型兼容、对象类型语义和方法资源。

### 方案

1. builtin 函数继续以 `hlsl_builtin_docs.*` 和 `resources/builtins/intrinsics/` 为单一事实来源。
2. object type / object method 继续以 `type_model.*`、`hover_markdown.*` 和 `resources/types/*`、`resources/methods/object_methods/` 为单一事实来源。
3. object method 参数维度必须通过 `type_model.*` 查询：
   - texture-like family
   - sampler-like family
   - sample coord dim
   - load coord dim
   - array texture extra coordinate
4. builtin overload resolution 消费 Phase 2 共享 compatibility result，不本地复制 half/float、splat、vector 规则。
5. 对 `label: expr` / `label: Type name` 暂按 `docs/type-method-interface-contract.md` 的当前偏差处理：正式支持前不继续扩散局部分支；若要支持，先建共享解析模型。
6. mixed signedness 规则必须明确产品语义：
   - 接受
   - warning
   - error
   - needs manual review
7. 新增 fixture 覆盖 `Sample`、`SampleLevel`、`Load`、`lerp`、`clamp`、`dot` 和 array texture 坐标。

### 验收标准

- object method diagnostics 使用共享对象语义和共享 compatibility，不再本地推导 array texture 坐标。
- `Built-in method call type mismatch` 在 50-unit audit 中显著下降。
- `Builtin call mixed integer signedness` 被明确分类并有测试覆盖。
- 对象类型 / 方法共享契约变化已更新 `docs/type-method-interface-contract.md`。

## Phase 7: 建立 diagnostics 可信度和发布前提契约

### 背景

当前 diagnostics 混合了明确错误、上下文缺失、parser recovery 级联和未实现规则。审计可以分类，但 server 内部 rule 还缺少统一的 prerequisites 契约。

### 目标

让每条 semantic diagnostic rule 只在前提满足时发出，避免上下文不完整时制造高置信误报。

### 方案

1. 为 diagnostics rule 建立 prerequisites 清单：
   - active unit ready
   - include closure ready
   - preprocessor context reliable
   - parser region reliable
   - semantic snapshot available
   - local scope reliable
   - expression type available
2. diagnostics facade 层统一检查或传递 rule context，不让每个 rule 自己猜环境。
3. 当前提不满足时：
   - 不发布该类高置信 semantic diagnostic。
   - 不新增 silent fallback 或旧逻辑兜底。
   - 必要时通过 debug/audit metadata 记录 skipped reason，便于审计。
4. diagnostics code/source 保持稳定，使审计和用户定位可区分。
5. 对公开 diagnostics 行为变化先确认，再实现。

### 验收标准

- full audit 中 `indeterminate-analysis` 和 parser/scope 级联误报下降。
- diagnostics rule 的 prerequisites 在代码和必要头文件注释中可见。
- 不存在 request 层为绕过上下文缺失新增的兼容路径。
- `docs/architecture.md` 和 `docs/testing.md` 已描述新的 diagnostics 前提和验证策略。

## Phase 8: 全量复审和真实源码问题分流

### 背景

前面阶段消除 LSP 架构缺陷后，剩余 diagnostics 才具备真实审核价值。此时需要把真实源码问题、真实配置问题和仍待规则确认的问题拆开处理。

### 目标

完成一轮可信 full audit，形成后续产品修复、源码修复和配置修复的分流清单。

### 方案

1. 跑 full 813-unit audit。
2. 对 top message groups 逐类复核：
   - 是否真实源码问题。
   - 是否真实 workspace 配置问题。
   - 是否仍是 LSP 缺陷。
   - 是否是规则策略待确认。
3. 为真实 LSP 缺陷补最小 fixture。
4. 为真实配置问题形成 workspace 配置建议，不写进资源或 diagnostics 特判。
5. 为真实源码问题保留样本和 include context，交由源码侧处理。
6. 形成最终报告：
   - baseline vs final 趋势
   - 已修复类别
   - 剩余问题分类
   - 后续风险

### 验收标准

- full audit 可稳定完成，file errors 为 0。
- `likely-plugin-limitation` 不再占据绝对主导。
- 剩余 top groups 均有明确 owner：LSP、workspace config、source、rule decision。
- 最终报告可指导下一轮真实源码审核。

## 阶段执行顺序

推荐顺序：

1. Phase 0: 固化审计基线和趋势门禁。
2. Phase 1: 修复 numeric literal parser。
3. Phase 2: 建立共享 HLSL 类型兼容模型。
4. Phase 3: 重建局部作用域和控制流模型。
5. Phase 4: 对齐预处理宏和真实 unit 编译上下文。
6. Phase 5: 修复 HLSL / NSF parser boundary 和 recovery。
7. Phase 6: 统一 builtin overload 和 object method 匹配。
8. Phase 7: 建立 diagnostics 可信度和发布前提契约。
9. Phase 8: 全量复审和真实源码问题分流。

Phase 2、Phase 3、Phase 4、Phase 7 是架构治理重点。它们分别对应类型系统、语义作用域、真实编译上下文和 diagnostics 发布契约；如果这些阶段不收敛，后续问题会继续以局部补丁形式扩散。

## 每阶段固定关闭检查

每个阶段完成前，按顺序回答：

1. 命令是否变化。
2. 路径或命名是否变化。
3. 架构或单一事实来源是否变化。
4. 测试策略是否变化。
5. 文档是否已同步。
6. 是否改变公开 diagnostics 行为。
7. 是否新增了 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径。
8. 是否有新的资源 bundle、资源路径、命名或加载规则变化。
9. 是否补齐 focused fixture 或稳定 real audit sample。
10. 是否重新跑了对应验证并记录结果。

若第 3、4、6、8 项任一为“是”，必须在最终说明中明确风险、事实文档更新和验证结果。

## 推荐验证命令

资源 / schema：

```powershell
npm run json:validate
```

TypeScript client / test：

```powershell
npm run compile
```

C++ server：

```powershell
cmake --build .\server_cpp\build
```

repo integration：

```powershell
npm run test:client:repo
```

5-unit smoke audit：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "5"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "600000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

50-unit trend audit：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "50"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "1800000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

full audit：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "0"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "7200000"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

## 十轮审查记录

### Review 1: 结构审查

结论：文档按背景、目标、方案、验收标准组织；每个 phase 都包含四个固定小节，满足可执行计划结构要求。

### Review 2: 审计事实审查

结论：关键统计与 2026-05-16 full audit 保持一致，包括 813 units、463556 diagnostics、主要 triage 和 category 分布。

### Review 3: 架构边界审查

结论：计划明确把类型兼容、作用域、预处理宏、对象方法和 diagnostics prerequisites 放回共享层，不要求在 request 或 feature 代码中复制语言知识。

### Review 4: 禁止项审查

结论：计划未引入 fallback、shim、compat layer、feature flag、双写路径或 diagnostics suppress；每个阶段都要求修共享根因。

### Review 5: 公开行为风险审查

结论：Phase 1 到 Phase 7 都可能改变公开 diagnostics 行为，文档已要求实现前确认风险和行为变化。

### Review 6: 资源规则审查

结论：Phase 4 只允许 confirmed builtin macro 通过 `language/preprocessor_macros` bundle 和生成脚本进入默认 preset，不允许 diagnostics-local macro allowlist。

### Review 7: 对象类型 / 方法契约审查

结论：Phase 6 要求 object method 参数维度通过 `type_model.*` 查询，并要求对象类型 / 方法契约变化同步更新 `docs/type-method-interface-contract.md`。

### Review 8: 测试可执行性审查

结论：每阶段都有 focused fixture、5-unit、50-unit、full audit 的验证策略；文档列出了可直接执行的 PowerShell 命令。

### Review 9: 文档同步审查

结论：计划明确列出 architecture、resources、testing、type-method-interface-contract 的更新触发条件，并在每阶段关闭检查中要求确认文档同步。

### Review 10: 可交付性审查

结论：计划具备阶段顺序、架构重点、执行步骤、验收标准和关闭检查；可以作为后续逐阶段实现的本地执行文档。
