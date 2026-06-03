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

Git 记录：

- 2026-05-16 已本地提交 P0 阶段：`1b2d927 test: add unit diagnostics audit trend`。
- 提交内容覆盖 unit-based real diagnostics audit、内部 debug request、趋势报告和 `docs/testing.md` 阶段验证说明。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

Git 记录：

- 2026-05-16 已本地提交 P1 阶段：`946f681 fix: normalize HLSL numeric literal diagnostics`。
- 提交内容覆盖共享 numeric literal parser、semantic diagnostics 消费路径、focused 正反 fixture、repo integration 断言、架构契约说明和本执行计划。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

### Phase 2 官方规则确认记录

状态：已完成人工确认，后续 P2 实现按以下规则收敛。

官方依据：

- [HLSL working draft](https://microsoft.github.io/hlsl-specs/specs/hlsl.html#Conv)：standard conversion sequence 由 lvalue / array、numeric / boolean / elementwise、scalar splat 或 vector / matrix truncation、qualification conversion 组成；并明确列出 integral conversion、floating conversion、floating-integral conversion、boolean conversion、component-wise conversions、usual arithmetic conversions、overload viable functions 和 conversion sequence rank。
- [Microsoft Learn HLSL operators](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-operators)：compiler 会执行 implicit type cast，例如 `int2 + 2` 可按 `int2 + int2(2, 2)` 理解；显式 cast 只改变表达意图，不改变官方可转换性的基础规则。
- [Microsoft Learn HLSL scalar data types](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-scalar)：`half` 在部分 shader target 下映射到 `float`，但 SM 6.2 起存在 `float16_t`；因此 diagnostics 不假设所有 target 都没有 half 精度风险，`float -> half` 仍作为有风险的隐式 narrowing 提醒用户。

确认规则：

1. `type mismatch` 的基础判定只看官方规则下是否存在 implicit conversion sequence；能形成转换序列时不再发布 mismatch error，找不到转换序列时才发布 assignment / return / argument / builtin / object method type mismatch。
   - 示例：`float3 a = float4(1, 2, 3, 4);` 官方可通过 truncation conversion 成立，产品发 truncation warning；`float3 b = float2(1, 2);` 没有扩维 conversion，产品继续发 mismatch error。
2. scalar splat 到 vector / matrix 是官方 standard conversion；纯 splat 不发 warning。若 splat 同时伴随 narrowing、signedness、boolean 等风险，则按对应风险规则处理。
   - 示例：`float3 a = 1.0;` 官方 scalar splat，产品不发 warning；`half3 b = runtimeFloat;` 是 splat + `float -> half`，产品发 narrowing warning。
3. vector / matrix truncation 是官方 standard conversion；隐式 truncation 不发布 mismatch error，但发布 warning：`Implicit truncation conversion: <actual> -> <expected>. Use an explicit cast or swizzle if this is intentional.` 显式 cast、constructor-style conversion 和 swizzle/member projection 不发该 warning。
   - 示例：`float3 a = wide4;` 发 truncation warning；`float3 b = (float3)wide4;`、`float3 c = float3(wide4);`、`float3 d = wide4.xyz;` 不发 truncation warning。
4. floating conversion 是官方 standard conversion；`half -> float -> double` 方向不发 warning，隐式 `double -> float/half`、`float -> half` 发布 narrowing warning。同形 vector / matrix 逐元素适用。numeric literal 可按目标类型适配，暂不因 `half h = 1.0;` 发布 narrowing warning。
   - 示例：`half h = runtimeFloat;` 发 narrowing warning；`half literal = 1.0;` 作为简单安全 literal adaptation，不发 warning。
5. integer / floating conversion 是官方 standard conversion；`int/uint -> float/double` 不发 warning，`int/uint -> half` 发布 warning，`float/half/double -> int/uint` 发布 warning。精确安全 literal 可不发 warning，例如 `float f = 1;`、`int i = 1.0;`；明显 lossy literal 如 `int i = 1.5;` 发布 warning。
   - 示例：`float f = intValue;` 不发 warning；`half h = intValue;` 和 `int i = runtimeFloat;` 发 floating-integral warning。
6. signedness conversion `int <-> uint` 是官方 integral conversion；非 literal 隐式转换发布 signedness warning，明显安全 literal 不发 warning。`uint u = -1;` 这类需要 unary folding 的场景，P2 暂不做复杂常量折叠，可保守 warning。
   - 示例：`uint u = intValue;` 发 signedness warning；`int i = 1u;` 不发 warning。
7. boolean conversion 是官方 standard conversion；合法时不发布 mismatch error。隐式 numeric -> bool 和 bool -> numeric 都发布 boolean conversion warning，即使 `bool b = 0/1` 也发布 warning。显式转换不发 warning。
   - 示例：`bool b = intValue;`、`int i = b;`、`bool literal = 1;` 均发 boolean conversion warning；`bool b = (bool)intValue;` 不发 warning。
8. vector / matrix 同 shape 时按 component-wise conversion 处理；元素转换合法则整体不发布 mismatch error，warning 跟随元素转换风险。目标 shape 更大仍为 mismatch，除非 source 是 scalar splat。
   - 示例：`float3 a = int3(1, 2, 3);` 合法且不发 warning；`half3 b = float3(1, 2, 3);` 发 narrowing warning；`float3 c = float2(1, 2);` 继续 mismatch。
9. binary arithmetic / comparison operator 走官方 usual arithmetic conversions，不复用 assignment expected/actual 规则；合法时不发布 binary mismatch。operand 侧发生隐式 truncation、signedness、floating-integral、boolean 等风险时发布对应 warning，表达式结果再交给外层 assignment / return / argument 规则判断。
   - 示例：`float3 v = float3(1, 2, 3) + float4(1, 2, 3, 4);` 不发 binary mismatch，发 truncation warning；结果再按外层 `float3` assignment 检查。
10. function、builtin 和 object method 参数匹配按官方 viable candidate + conversion rank 建模；只要存在 viable candidate，就不发布 argument mismatch / builtin mismatch。对最终选中候选所需的 risky implicit conversion 发布 warning。object method 的对象族、坐标维度仍通过 `type_model.*` 和 Phase 6 处理，不在 P2 复制 array texture 坐标规则。
    - 示例：`P2AcceptFloat3(float4Value)` 存在 viable candidate，发 truncation warning，不发 `Function call argument mismatch`；`dot(float3Value, float4Value)` 这类 builtin 参数转换可行时同理。
11. P2 只做简单 numeric literal adaptation：常见安全 literal 不发 warning，明显 lossy literal 发 warning；不实现完整 constant evaluator、unary folding、overflow / underflow range checking。
    - 示例：`half h = 1.0;` 不发 warning；`int i = 1.5;` 可发 floating-integral warning；`uint u = -1;` 不做 unary folding 精确判定。
12. C-style cast、HLSL constructor-style conversion 和 swizzle/member projection 压制对应 implicit conversion warning，但不压制真正不可行转换的 error。
    - 示例：`float3 a = (float3)wide4;` 不发 truncation warning；`float3 b = float2(uv);` 仍是不可行扩维 mismatch。
13. risky implicit conversion 统一使用 warning，不再伪装成 mismatch error；一次转换同时命中多个风险时只发布最高优先级 warning，优先级为 truncation、boolean、floating-integral、signedness、narrowing。
    - 示例：`bool b = float2(1, 0);` 同时有 shape reduction 和 boolean risk 时，只发布优先级更高的 truncation warning。

P2 实施边界：

- P2 包含：共享 official implicit conversion sequence 模型，assignment / return / function argument / builtin argument / object method argument / binary operator 统一消费 compatibility result，conversion result 结构化返回 compatible / incompatible、conversion kind / rank / cost、warning kind、normalized expected / actual。
- P2 focused fixture 覆盖：scalar splat、truncation warning、floating narrowing warning、floating-integral warning、signedness warning、boolean warning、component-wise vector / matrix conversion、overload viable / rank、explicit cast / constructor / swizzle warning suppression。
- P2 不包含：object method array texture 坐标维度新规则、`label: expr` / `label: Type name` 共享建模、完整 constant evaluator、overflow / underflow range checking、target-profile-specific `half` 存储宽度配置、性能优化建议类 diagnostics。

### Phase 2 执行记录

状态：已完成 P2 主体实现与阶段验证。

实现内容：

- `type_relation.*` 已扩展为 diagnostics / overload 共享 HLSL implicit conversion sequence 模型，结构化返回 conversion kind、warning kind、cost、normalized expected / actual 和 viability。
- `type_relation.*` 现在覆盖 scalar splat、vector / matrix truncation、floating conversion、floating-integral conversion、signedness conversion、boolean conversion、component-wise vector / matrix conversion、object family conversion 和 usual arithmetic conversion。
- `overload_resolver.*` 保留既有 resolver 调用方式，并记录 best candidate 的 per-argument relation，供 diagnostics 对最终选中候选发布 risky implicit conversion warning。
- semantic diagnostics 的 assignment、return、binary operator、builtin call 和 user function call 已迁移到共享 relation；合法官方转换不再发布 type mismatch error。
- risky implicit conversions 使用独立 warning 文案，包括 truncation、narrowing、floating-integral、signedness 和 boolean；一次转换只由共享 relation 选择最高优先级 warning。
- numeric literal target adaptation 按已确认边界只做简单单 literal 识别；例如 `half h = 1.0;` 不再发布 narrowing warning，明显 runtime narrowing 仍发布 warning。
- builtin `dot`、`distance`、`lerp`、`min` 和 `max` 等诊断路径开始消费 shared usual arithmetic conversion / relation warning，避免继续把官方可行转换当作 builtin mismatch。
- `type_desc.*` 补充 macro-like numeric alias 归一化，例如 `MaterialFloat3`、`MaterialHalf4x4`、`MaterialDouble2`、`MaterialInt4` 和 `MaterialUInt4`，避免项目类型别名绕过共享 relation。
- 新增 focused fixture `test_files/module_diagnostics_type_relation_official_conversions.nsf`，覆盖 scalar splat、truncation、narrowing、floating-integral、signedness、boolean、component-wise vector conversion、user-call viable conversion、显式 cast / constructor / swizzle warning suppression 和保留非法 shape expansion mismatch。
- repo diagnostics 断言已按公开行为变化更新：`float4 -> float3/float2` 等官方 truncation 不再期待 mismatch error，而期待 truncation warning；literal adaptation 不再期待 narrowing warning。

公开行为变化：

- 官方 implicit conversion sequence 可行时，不再发布 assignment / return / function argument / builtin / binary operator type mismatch error。
- 隐式 truncation、floating narrowing、floating-integral、signedness 和 boolean conversion 现在发布 warning，提示使用显式 cast 或 swizzle 表达意图。
- 简单 numeric literal 到目标 numeric 类型的安全适配不发布 narrowing warning；复杂 constant folding 和范围检查仍不属于 P2。
- builtin/user function 参数匹配中，存在 viable candidate 时不再发布 mismatch；对最终选中候选的 risky implicit conversion 发布 warning。

已运行验证：

- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，66 passing / 1 pending；该定向 repo 回归已重跑两次，结果一致。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-02-type-relation-smoke-5.{json,md}`；`diagnosticsTotal` 4947 -> 4730（-217，-4.39%），`likely-plugin-limitation` 3907 -> 863（-77.91%），`expression-type-analysis` 2755 -> 35（-98.73%），assignment mismatch 2455 -> 25，return mismatch 250 -> 10，binary mismatch 50 -> 0，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-02-type-relation-trend-50.{json,md}`；`diagnosticsTotal` 43341 -> 40625（-2716，-6.27%），`likely-plugin-limitation` 34380 -> 7906（-77.00%），`expression-type-analysis` 23841 -> 350（-98.53%），`call-type-analysis` 3337 -> 1731（-48.13%），assignment mismatch 21027 -> 250，return mismatch 2357 -> 100，binary mismatch 457 -> 0，builtin mismatch 1313 -> 493，function mismatch 936 -> 150，`numeric-literal` 1377 -> 0，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

审计备注：

- 新增的 implicit conversion warnings 目前被 audit classifier 归入 `other` / `needs-manual-review`，因此 `other` 和 `needs-manual-review` 明显增加。这不是 P2 回归，而是已确认的公开 diagnostics 行为变化；后续若要细分转换 warning，可在 audit classifier 中新增 conversion-warning 类别。
- P2 未重跑 full 813-unit audit；本阶段按计划使用 focused fixture、repo diagnostics 回归、5-unit smoke 和 50-unit trend 作为收敛验证。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，HLSL implicit conversion sequence 收敛到 `type_relation.*`，轻量类型别名归一化收敛到 `type_desc.*`。
- 测试策略是否变化：是，P2 focused fixture 补齐官方转换正反用例；audit 趋势验证沿用 P0 机制。
- 文档是否已同步：已更新 `docs/architecture.md` 和本执行计划；资源、测试命令、对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，官方可行转换不再发布 mismatch error，改为对 risky implicit conversion 发布 warning。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-02 5-unit / 50-unit real audit 报告。

Git 记录：

- 2026-05-16 已本地提交 P2 阶段：`ee92d44 fix: model HLSL implicit conversions`。
- 提交内容覆盖共享 HLSL implicit conversion model、diagnostics / overload consumer 迁移、macro-like numeric alias 归一化、focused fixture、repo integration 断言、架构契约说明和本执行计划。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

### Phase 3 执行记录

状态：已落地 lexical local scope / loop initializer 可见性收敛。

实现内容：

- `server_parse.*` 新增共享 `extractForInitializerDeclarationsInLineShared(...)`，统一解析 `for (int i = ...; ...)` initializer 中的声明，返回原始行 byte span，避免 diagnostics rule 自行猜测 loop 变量。
- `diagnostics_semantic.cpp` 将函数内 local 状态从按名称平铺的 `localsByName` 改为 lexical scope stack；进入 / 退出 block 时同步维护可见 local 集，`for` initializer 使用独立 loop scope，braced body 结束后释放，非 braced body 在下一条语句后释放。
- duplicate local declaration 只在当前 lexical scope 内、且 active preprocessor branch signature 重叠时发布；sibling block 同名和 nested block shadow 不再误报 duplicate。
- `semantic_snapshot.*` 的 local extraction 记录 `scopeStartOffset` / `scopeEndOffset`、`scopeId` 和 `parentScopeId`；`querySemanticSnapshotLocalTypeAtOffset(...)` 必须同时满足 declaration offset 与 half-open lexical scope range，避免 depth-only 查询把已离开 block 的 local 继续暴露给 hover / completion 等共享语义 consumer。
- 新增 focused fixture `test_files/module_diagnostics_local_scope_control_flow.nsf`，覆盖 loop initializer body 内可见 / loop 外不可见、sibling block 同名、nested block shadow、if/else all-return 和 early return 后 unreachable。
- `src/test/suite/integration/diagnostics.ts` 新增 lexical-scope diagnostics 集成断言。
- `docs/architecture.md`、`docs/testing.md`、`semantic_snapshot.hpp`、`semantic_cache.hpp` 已更新 lexical local scope / semantic snapshot / 阶段验证契约。

公开行为变化：

- `for` initializer 变量在 condition / iteration / body 内可见，不再产生 loop 内 `Undefined identifier` 误报；loop 外继续按 undefined identifier 报告。
- sibling block 或 nested block 中同名 local 不再被当作 `Duplicate local declaration`；同一 lexical scope 内重复声明仍保持 warning。
- local hover / completion / diagnostics 类型查询不再把 scope range 之外的 local 当作可见符号。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，67 passing / 1 pending；覆盖新增 lexical scope fixture 和既有 diagnostics 回归。
- `npm run test:client:repo` 全量通过，56 个 repo integration 文件。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-03-local-scope-smoke-5.{json,md}`；相对 P0 baseline：`diagnosticsTotal` 4947 -> 4211（-736，-14.88%），`likely-plugin-limitation` 3907 -> 448（-88.53%），`undefined-identifier` 531 -> 116（-78.15%），`Duplicate local declaration` 284 -> 165（-41.90%），`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。相对 P2 smoke 的 `diagnosticsTotal=4730` 继续下降到 4211。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-03-local-scope-trend-50.{json,md}`；相对 P0 baseline：`diagnosticsTotal` 43341 -> 35806（-7535，-17.39%），`likely-plugin-limitation` 34380 -> 4008（-88.34%），`undefined-identifier` 4842 -> 944（-80.50%），`Duplicate local declaration` 2547 -> 1476（-42.05%），`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。相对 P2 trend 的 `diagnosticsTotal=40625` 继续下降到 35806。

审计备注：

- `Unreachable code` 仍在 real audit 中较高，当前阶段只修复 local scope 污染和明确 block-flow 前提，剩余样本多与 parser/recovery 或真实源码控制流相关，应在 Phase 5 / Phase 7 继续复核。
- P3 未重跑 full 813-unit audit；本阶段按计划使用 focused fixture、repo diagnostics 回归、5-unit smoke 和 50-unit trend 作为收敛验证。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和阶段 audit 报告；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，`for` initializer declaration 解析收敛到 `server_parse.*`，semantic snapshot local visibility 改为 half-open lexical scope range。
- 测试策略是否变化：是，P3 focused fixture 补齐 local scope / loop variable / block-flow 正反用例；audit 趋势验证沿用 P0 机制。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`semantic_snapshot.hpp`、`semantic_cache.hpp` 和本执行计划；资源、测试命令和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，loop 内 undefined identifier 误报和跨 sibling / nested scope duplicate local 误报被移除；同 scope duplicate、loop 外 undefined 和明确 unreachable 仍保留。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-03 5-unit / 50-unit real audit 报告。

Git 记录：

- 2026-05-17 已本地提交 P3 阶段：`fix: model lexical local scopes`；当前提交 hash 以 `git log -1 --oneline` 为准。
- 提交内容覆盖共享 `for` initializer declaration 解析、diagnostics lexical scope stack、semantic snapshot local scope range、focused fixture、repo integration 断言、架构 / 测试契约说明和本执行计划。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

### Phase 4 执行记录

状态：已落地 shadercompiler 编译上下文宏 preset 补齐。

实现内容：

- `scripts/builtins/update_preprocessor_macros.py` 新增 `--compiler-context` 输入，可从 shadercompiler `hlsl_process.py` 提取 `SYSTEM_SUPPORT_MACROS`、`DEVICE_SUPPORT_MACRO`、`API_SUPPORT_MACRO`、`API_SUPPORT_MACRO_APPEND_IN_FUTURE_HIGH` 和 `API_PLATFORM_QUALITY_MACROS` 中的宏名。
- `server_cpp/resources/language/preprocessor_macros/base.json` 从 138 个条目扩展到 149 个条目，新增 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_SSBO`、`API_SUPPORT_SV_INSTANCE_ID`、`API_SUPPORT_TEXFETCH`、`API_SUPPORT_SV_VERTEX_ID`、`API_SUPPORT_FRAGCOORD`、`API_SUPPORT_TEXTURE_GATHER`、`SYSTEM_SUPPORT_DEPTH_BUFFER_AS_TEXTURE`、`GLES_USE_UBO`、`SYSTEM_SUPPORT_SRGB`。
- 新增编译上下文宏默认 replacement 均为 `0`；这与之前 undefined macro 在条件表达式中按 false 参与求值的活跃分支保持一致，只移除“已知编译上下文宏 undefined”的误报。真实 target / compile mode 值仍由 workspace `nsf.preprocessorMacros` 或 `nsf.defines` 覆盖。
- `scripts/json/validate_resources.js` 增加关键编译上下文宏 coverage 检查，避免后续生成时再次漏掉 API / system 宏。
- `test_files/module_diagnostics_preprocessor_builtin_macros.nsf` 和 `src/test/suite/integration/diagnostics.ts` 扩展 focused 断言，验证 server preset 包含这些宏、默认值为 `0`，并且设置到 workspace 后不再产生对应 undefined macro diagnostics。
- `docs/resources.md` 已记录 `--compiler-context`、默认 preset 来源和编译上下文宏默认值策略；`docs/architecture.md` 已记录预处理宏 preset 的职责边界和 workspace / defines 覆盖关系。

公开行为变化：

- 新 preset 或显式合并后的 workspace 配置中，shadercompiler 编译上下文宏不再产生 `Undefined macro in preprocessor expression` diagnostics。
- 已有显式 `nsf.preprocessorMacros` workspace 不会自动叠加新的 bundle 默认值；需要用户配置合并这些 key，或删除显式配置后重新 seed，符合“配置表是完整有效 preset”的既有契约。

验证结果：

- `npm run json:validate` 通过。
- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，67 passing / 1 pending。
- `npm run test:client:repo` 全量通过。
- 5-unit smoke audit 使用临时 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace`，将新 preset 合并进显式 `nsf.preprocessorMacros` 后通过；输出 `real-workspace-diagnostics-audit.phase-04-preprocessor-context-smoke-5.{json,md}`。相对 P3 smoke：`preprocessor-context` 469 -> 183，`diagnosticsTotal` 4211 -> 3926，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 使用同一临时 copy workspace 通过；输出 `real-workspace-diagnostics-audit.phase-04-preprocessor-context-trend-50.{json,md}`。相对 P3 trend：`preprocessor-context` 4004 -> 1482，`diagnosticsTotal` 35806 -> 33284，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

审计备注：

- 50-unit 剩余 `preprocessor-context` 样本已不再由 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_SV_INSTANCE_ID` 或 `API_SUPPORT_TEXFETCH` 主导；当前样本集中在 `COLOR_CHANGE_*`、`RENDER_VELOCITY` 等 workspace/source 配置宏，后续应按真实项目配置或源码 include context 分流，不进入正式 bundle。

阶段关闭判断：

- 命令是否变化：资源生成脚本新增可选 `--compiler-context` 参数；npm / cmake 验证命令未变化。
- 路径或命名是否变化：无运行时路径或 bundle 命名变化；新增阶段 audit 报告和临时验证 workspace。
- 架构或单一事实来源是否变化：是，默认预处理宏 preset 的事实来源扩展为 `builtin_macros.py` + `hlsl_process.py` 编译上下文宏名，仍通过 `language/preprocessor_macros` bundle 和 `language_registry.*` 统一暴露。
- 测试策略是否变化：是，focused fixture 和资源校验补齐编译上下文宏 coverage；real audit 使用临时 copy workspace 合并新 preset，以避免修改真实 workspace 文件。
- 文档是否已同步：已更新 `docs/resources.md`、`docs/architecture.md` 和本执行计划；`docs/testing.md` 的验证命令与策略无需变化。
- 是否改变公开 diagnostics 行为：是，已知编译上下文宏不再产生 undefined macro 误报。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否；仅扩展既有 bundle 内容和生成脚本输入。
- 是否补齐 focused fixture 或稳定 real audit sample：已扩展 focused fixture，并生成 phase-04 5-unit / 50-unit real audit 报告。

Git 记录：

- 2026-05-17 已本地提交 P4 阶段：`66017be fix: include shader compiler context macros`。
- 提交内容覆盖 shadercompiler 编译上下文宏提取、预处理宏 preset 资源、资源校验 coverage、focused fixture / repo integration 断言、架构 / 资源契约说明和本执行计划。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

### Phase 5 执行记录

状态：已落地 HLSL / NSF parser boundary 和 missing-semicolon recovery 收敛。

实现内容：

- `server_parse.*` 的 shared line scan 新增 line-before parenthesis / bracket depth，missing-semicolon 共享判断现在同时消费 line-before 和 line-after grouping 状态。
- `shouldReportMissingSemicolonShared(...)` 统一收敛多行函数签名尾行、跨行 control condition 尾行、表达式 continuation、NSF metadata / effect block header 和 macro-only recovery 区域的缺分号边界判断；immediate syntax diagnostics 与 full semantic diagnostics 共用同一入口。
- semantic diagnostics 的 pending multiline local declaration recovery 不再要求 semicolon 与声明起点处于完全相同 brace depth，避免数组 / initializer block 在后续 `};` 处被误判为缺分号。
- 新增 focused fixture `test_files/module_diagnostics_parser_boundary_recovery.nsf`，覆盖多行函数签名、跨行 return/constructor 表达式、跨行 `if` condition、macro-generated function line、object-like statement macro 和数组 initializer。
- `src/test/suite/integration/diagnostics.ts` 新增 settled diagnostics 断言，保证 immediate 与 full diagnostics 稳定后都不再发布上述 false `Missing semicolon`。
- `docs/architecture.md` 已记录 `server_parse.*` 对 missing-semicolon syntax boundary 和 recovery 区域的共享职责；`docs/testing.md` 已补充 parser boundary / recovery 的推荐验证矩阵。

公开行为变化：

- 合法多行函数签名尾行、跨行 control condition 尾行、跨行表达式 continuation、macro-only 展开行和多行 initializer 不再发布高置信 `Missing semicolon.`。
- 单行真实缺分号、return / break / continue / discard 缺分号、postfix update 缺分号仍保留 diagnostics。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，68 passing / 1 pending。
- 5-unit smoke audit 使用临时 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 通过；输出 `real-workspace-diagnostics-audit.phase-05-parser-boundary-smoke-5.{json,md}`。相对 P4 smoke：`effect-syntax-or-macro` 118 -> 30，`syntax-structure` 79 -> 29，`diagnosticsTotal` 3926 -> 3787，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 使用同一临时 copy workspace 通过；输出 `real-workspace-diagnostics-audit.phase-05-parser-boundary-trend-50.{json,md}`。相对 P4 trend：`effect-syntax-or-macro` 983 -> 287，`syntax-structure` 705 -> 261，`diagnosticsTotal` 33284 -> 32144，`undefined-identifier` 保持 944，`semantic-source-rule` 保持 2531，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

审计备注：

- 剩余 `Missing semicolon` 主要集中在更复杂的 multiline return / constructor 表达式和真实缺分号样本；本阶段先收敛已确认的 parser boundary / recovery 高置信误报，不引入 message-level suppress。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和阶段 audit 报告；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，missing-semicolon parser boundary 与 recovery 可信判断收敛到 `server_parse.*` 共享入口。
- 测试策略是否变化：是，P5 focused fixture 覆盖 parser boundary / macro-heavy recovery，`docs/testing.md` 增加对应验证矩阵；audit 趋势验证沿用 P0 机制。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`server_parse.hpp` 和本执行计划；资源、开发和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，上述合法 parser boundary / recovery 区域不再发布 `Missing semicolon.`。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-05 5-unit / 50-unit real audit 报告。

Git 记录：

- 2026-05-17 已本地提交 P5 阶段：`9caea41 fix: tighten parser boundary diagnostics`。
- 提交内容覆盖 shared missing-semicolon parser boundary、macro-only / multiline recovery、focused fixture、repo integration 断言、架构 / 测试契约说明和本执行计划。
- 2026-05-17 已推送至 `origin/main`；远端同步点包含 `9caea41` 和 `3264d52`。

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

### Phase 6 执行记录

状态：已落地 builtin overload mixed signedness 和 object method 参数匹配收敛。

实现内容：

- `hover_markdown.*` 的 `lookupHlslBuiltinMethodRule(...)` 现在暴露由 `methods/object_methods` 签名模板展开后的参数声明；`{floatCoord}` / `{intCoordPlus1}` 等占位符通过 `type_model.*` 的 sample / load coord 维度查询解析。
- object method diagnostics 不再在 rule 内本地维护 sampler-like、float coord、int coord 或 array texture 坐标判断；现在先通过 `type_model.*` 确认 base object family，再用 object method 资源展开参数，并交给 `overload_resolver.*` / `type_relation.*` 判定实参兼容与 warning。
- `diagnostics_expression_type.*` 的 builtin 元素类型合并改为消费 `type_relation.*` 的 usual arithmetic conversion；`mul` 等形状规则仍保留自身矩阵 / 向量形状判定，但元素 signedness 不再产生 builtin mismatch。
- real diagnostics audit classifier 将 `Implicit truncation/boolean/floating-integral/signedness/narrowing conversion` 归入 `type-conversion-risk` / `needs-manual-review`，避免合法但有风险的转换 warning 继续混在 `other`。
- 新增 focused fixture `test_files/module_diagnostics_builtin_object_method_matching.nsf`，覆盖 `Sample`、`SampleLevel`、`Load`、`SampleCmp`、Texture2DArray 坐标、array load 坐标、sampler comparison mismatch 和 object method 参数 conversion warning。
- 扩展 `module_diagnostics_hlsl_builtin_overload_args.nsf` 与 repo integration 断言，覆盖 `mul(int, uint)` 不再报 `Builtin call type mismatch`，而是发布 signedness warning。
- `docs/architecture.md`、`docs/type-method-interface-contract.md` 和 `docs/testing.md` 已同步记录 object method 参数共享入口、builtin mixed signedness 语义和 audit 分类口径。

公开行为变化：

- 合法 builtin mixed signedness 调用不再发布 `Builtin call mixed integer signedness` 或 `Builtin call type mismatch`；非 literal `int` / `uint` 隐式转换改为 `Implicit signedness conversion` warning。
- 合法 object method 参数隐式转换不再发布 `Built-in method call type mismatch`；按 `type_relation.*` 发布 truncation / boolean / floating-integral / signedness / narrowing warning。
- Texture array `Sample` / `Load` 坐标维度由 `type_model.*` 展开的资源参数决定；无法形成合法 conversion sequence 时才保留 `Built-in method call type mismatch`。
- `label: expr` / `label: Type name` 本阶段未新增支持，仍遵守当前偏差边界。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，69 passing / 1 pending。
- `npm run test:client:repo` 全量通过。
- 5-unit smoke audit 使用临时 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 通过；输出 `real-workspace-diagnostics-audit.phase-06-builtin-method-matching-smoke-5.{json,md}`。相对 P5 smoke：`diagnosticsTotal` 3787 -> 3702，`likely-plugin-limitation` 360 -> 205，`call-type-analysis` 179 -> 24，`Built-in method call type mismatch` 110 -> 0，`Builtin call type mismatch` 50 -> 5，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 使用同一临时 copy workspace 通过；输出 `real-workspace-diagnostics-audit.phase-06-builtin-method-matching-trend-50.{json,md}`。相对 P5 trend：`diagnosticsTotal` 32144 -> 31319，`likely-plugin-limitation` 3312 -> 1785，`call-type-analysis` 1731 -> 204，`Built-in method call type mismatch` 1077 -> 0，`Builtin call type mismatch` 493 -> 43，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

审计备注：

- P6 后 `type-conversion-risk` 成为主要 category，这是显式分类变化：P5 中大量合法 implicit conversion warning 仍落在 `other`，P6 将其统一归类为需要人工审核的风险转换；新增的 object method / builtin 合法转换也会进入该类。
- 50-unit 剩余 `Builtin call type mismatch` 主要集中在 `normalize(mul(camera_matrix, ...))` 一类仍待进一步建模的矩阵 / macro-like 类型场景；`Built-in method call type mismatch` 已在 50-unit 样本中清零。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和阶段 audit 报告；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，object method diagnostics 的参数形状收敛到 `methods/object_methods` + `hover_markdown.*` + `type_model.*`，builtin mixed signedness 收敛到 `type_relation.*`。
- 测试策略是否变化：是，audit classifier 新增 `type-conversion-risk`，并补充 builtin / object method diagnostics 推荐验证矩阵。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/type-method-interface-contract.md`、`docs/testing.md`、`hover_markdown.hpp` 和本执行计划；资源、开发文档未变化。
- 是否改变公开 diagnostics 行为：是，builtin / object method 的合法隐式转换从 mismatch 改为 warning。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-06 5-unit / 50-unit real audit 报告。

Git 记录：

- 2026-05-17 已本地提交 P6 阶段：`2046e9c fix: unify builtin method diagnostics`。
- 本阶段未执行远端 push；如后续需要上传远端，应在 push 后追加记录目标 remote / branch。

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

### Phase 7 执行记录

状态：已落地 semantic diagnostics prerequisites 发布契约。

实现内容：

- 新增 `diagnostics_prerequisites.*`，统一表达 semantic diagnostics 高置信发布前提：active unit ready、include closure ready、preprocessor context reliable、parser region reliable、semantic snapshot available、local scope reliable 和 expression type available。
- `diagnostics_preprocessor.*` 现在返回 preprocessor view 与 prerequisites metadata；当 `.hlsl` 目标不能被证明属于当前 active `.nsf` unit include closure 时，semantic rules 会把 include / preprocessor context 视为不可靠，只保留 local syntax / preprocessor 检查路径。
- `diagnostics_semantic.*` 的 semantic source、expression type、call type 和 undefined identifier 高置信 diagnostics 改为先通过共享 prerequisites gate；当前提不满足时不发布高置信 diagnostics，并在 `DiagnosticsBuildResult.prerequisiteSkips` 中记录 skipped reason。
- real diagnostics audit debug response 和报告已输出 `prerequisiteSkippedTotal` / `prerequisiteSkippedByReason`，阶段趋势可以区分 diagnostics 下降是规则修复还是前提不足导致的跳过。
- 新增 focused fixture：
  - `test_files/module_diagnostics_prerequisites_parser_region.nsf`：验证 open grouping parser region 内不再发布 undefined identifier 级联，同时可靠行仍发布 truncation warning。
  - `test_files/module_diagnostics_prerequisites_orphan_include.hlsl`：验证不属于 active unit include closure 的 orphan include 不发布高置信 semantic diagnostics，并输出 include / preprocessor prerequisites skipped metadata。
- `docs/architecture.md`、`docs/testing.md`、`diagnostics_prerequisites.hpp`、`diagnostics.hpp`、`diagnostics_preprocessor.hpp` 和 `diagnostics_semantic.hpp` 已同步记录 prerequisites 与 skipped metadata 契约。

公开行为变化：

- 对 parser region 不可靠的行，semantic source、expression type、call type 和 undefined identifier 规则不再发布高置信 diagnostics；syntax / preprocessor diagnostics 仍按原路径发布。
- 对没有可靠 active unit include context 的 `.hlsl`，不再用单文件 fallback preprocessor context 发布高置信 semantic diagnostics；debug / audit metadata 会记录 skipped reason。
- expression type 不可用时，assignment / return / binary / call 这类高置信 mismatch 不再静默忽略为无来源差异，而是计入 prerequisite skipped metadata；既有明确 indeterminate diagnostics 路径保留。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，71 passing / 1 pending；覆盖 parser-region prerequisites、orphan include prerequisites、既有 undefined / scope / return / builtin / object method diagnostics 回归。
- 首次 `npm run test:client:repo` 全量在 `client.real-workspace-replay-recorder` 的步骤数量断言处失败：`4 !== 3`；该测试与 P7 C++ diagnostics 路径无交集。随后 `$env:NSF_TEST_FILE_FILTER='client.real-workspace-replay-recorder'; npm run test:client:repo` 通过，确认该失败为测试时序 / 状态波动；再次 `npm run test:client:repo` 全量通过。
- 5-unit smoke audit 使用临时 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 通过；输出 `real-workspace-diagnostics-audit.phase-07-diagnostics-prerequisites-smoke-5.{json,md}`。相对 P6 smoke：`diagnosticsTotal` 3702 -> 3592，`semantic-source-rule` 327 -> 238，`undefined-identifier` 保持 116，`expression-type-analysis` 保持 35，`call-type-analysis` 保持 24，`prerequisiteSkippedTotal=1725`，其中 `parser_region_unreliable=1190`、`expression_type_unavailable=535`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 使用同一临时 copy workspace 通过；输出 `real-workspace-diagnostics-audit.phase-07-diagnostics-prerequisites-trend-50.{json,md}`。相对 P6 trend：`diagnosticsTotal` 31319 -> 30302，`semantic-source-rule` 2531 -> 1858，`undefined-identifier` 保持 944，`expression-type-analysis` 保持 350，`call-type-analysis` 保持 204，`prerequisiteSkippedTotal=16342`，其中 `parser_region_unreliable=11458`、`expression_type_unavailable=4884`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 未重跑 full 813-unit audit；P7 以 focused fixture、repo full regression、5-unit smoke 和 50-unit trend 验证发布契约，full audit 留给 Phase 8 全量复审。

审计备注：

- P7 后 `likely-plugin-limitation` 在 50-unit 样本中保持 P6 的 1785，说明本阶段没有靠 message-level suppress 改写分类；下降主要来自 parser/scope 前提不足区域跳过部分 `semantic-source-rule` 级联。
- `prerequisiteSkippedTotal` 是 debug / audit metadata，不是用户可见 diagnostics；分析趋势时应与 category delta 一起看，避免把“前提不足导致跳过”和“规则真正建模成功”混为一类。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture、`diagnostics_prerequisites.*` 和阶段 audit 报告；无资源路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，semantic diagnostics 高置信发布前提收敛到 `diagnostics_prerequisites.*` 共享入口，preprocessor/include context 可靠性由 diagnostics facade 传递。
- 测试策略是否变化：是，audit 报告新增 prerequisites skipped metadata，`docs/testing.md` 增加 P7 验证矩阵和趋势核对口径。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、相关头文件说明和本执行计划；资源、开发和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，前提不满足时不再发布高置信 semantic diagnostics。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-07 5-unit / 50-unit real audit 报告。

Git 记录：

- 本阶段已本地提交，commit message：`fix: gate diagnostics on prerequisites`；当前提交 hash 以 `git log -1 --oneline` 为准。

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

### Phase 8 执行记录

状态：已完成 full audit 和剩余 diagnostics 分流。

实现内容：

- 使用 Phase 4 之后的临时 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 跑 full 813-unit audit，避免修改真实 workspace，同时保留补齐后的 `nsf.preprocessorMacros` preset 口径。
- 生成阶段报告 `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-08-final-full.{json,md}`。
- 将详细分流表直接并入本执行计划，按 LSP、workspace config、source review 和 rule decision 对剩余 top groups 归因。
- 本阶段未改 diagnostics 代码、资源、schema、测试 helper 或公开行为。

full audit 结果：

- `NSF units scanned=813`，`unitFileVisits=25985`，`filesScanned=1191`。
- `diagnosticsTotal=306981`，相对 2026-05-16 baseline `463556` 下降 `156575`（`-33.78%`）。
- `likely-plugin-limitation=22710`，相对 baseline `373861` 下降 `351151`（`-93.93%`），不再占据绝对主导。
- `needs-manual-review=273719`，主要由新增显式分类的 `type-conversion-risk=251553` 构成。
- `check-config-or-source=10549`，主要是项目 / workspace 宏未配置。
- `numeric-literal=0`，`Built-in method call type mismatch=0`，builtin mixed signedness 旧 top groups 为 `0`。
- `Diagnostic wait timeouts=0`，`fileErrors=0`。
- `truncatedFiles=1`、`timedOutFiles=1`，均为既有 `ui/floor_board_ui.nsf -> ui/floor_board_ui.nsf`，未比 baseline 增加。
- `prerequisiteSkippedTotal=178989`，其中 `parser_region_unreliable=121189`、`expression_type_unavailable=57800`；这些是 debug / audit metadata，不是用户可见 diagnostics。

剩余分流结论：

- Rule decision / source review：`type-conversion-risk` 是当前最大量级，需要先决定 `balanced` mode 是否应默认显示这一类合法但有风险的隐式转换 warning，再交源码侧按需加显式 cast / swizzle。
- Workspace config / source config：`Undefined macro in preprocessor expression` 剩余样本集中在 `RENDER_VELOCITY`、`COLOR_CHANGE_MODE`、`COLOR_CHANGE_PICKER`、`COLOR_CHANGE_MULTIPLE`、`COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE` 等项目宏或 enum 宏，应通过 workspace 配置或真实 include context 进入，不进入正式资源 bundle。
- LSP 后续治理：boolean literal 被 undefined identifier 扫到、macro define numeric typing、`MaterialFloat4(...)` 这类 macro-like constructor typing、多行 return / constructor expression parser boundary、control-flow return completeness、`smaa.hlsl` 条件栈和 `sincos/log/log2/fwidth/round/cosh` builtin modeling。
- Source review 优先样本：`grass_max_offset` only-use、`SampleTexArryPkgNormalBias` 定义 5 参数但调用 4 参数、`GetVisibility(float,float3)` 被传入 `float2`、`shading_models.hlsl` 中两个同签名 `Init(...)`。

Phase 8 剩余 top groups 分流表：

| Group | Count | Owner | 依据与下一步 |
| --- | ---: | --- | --- |
| Implicit narrowing / floating-integral / signedness / truncation / boolean conversion | 251553 | Rule decision + source review | 合法但有风险的隐式转换 warning；需要先决定 `balanced` 是否承载这一级噪声，源码侧按需补显式 cast / swizzle，不写成资源或 diagnostics 特判。 |
| Undefined macro in preprocessor expression | 10549 | Workspace config / source config | 样本集中在 `RENDER_VELOCITY`、`COLOR_CHANGE_MODE`、`COLOR_CHANGE_PICKER`、`COLOR_CHANGE_MULTIPLE`、`COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE` 等项目宏或枚举宏，应由 workspace `nsf.preprocessorMacros` / `nsf.defines` 或真实 include context 提供。 |
| Duplicate local declaration | 10289 | Workspace config first, then source review | 样本来自 `surface_functions.hlsl` 的 `COLOR_CHANGE_MODE == ...` 多分支；缺少 enum/config 宏会让多个分支同时按 `0 == 0` 进入 active analysis。 |
| Undefined identifier | 11733 | Mixed: LSP + source/config | 样本包含 `true` 这类应作为 boolean literal 处理的 LSP 问题，也包含 `grass_max_offset` 这类 search root 内只找到使用点的 source/config 问题；后续 audit 可增加 exact-symbol 聚合。 |
| Missing semicolon, effect-syntax-or-macro | 3056 | LSP parser/recovery | 样本是合法多行 `return lerp(...)` / `return float3x3(...)` 形式，应补 focused fixture 继续收敛 multiline return / constructor expression boundary。 |
| Missing semicolon, syntax-structure | 3886 | LSP parser/recovery + source review | 样本包含合法多行 constructor return，如 `return half3(...` 后续行闭合并有分号；先按 parser boundary 治理，剩余真实缺分号再交源码侧。 |
| Function call argument mismatch | 2243 | Mixed: source/config + LSP context | `GetVisibility(float,float3)` 被传入 `float2` 的样本看起来像真实源码或 inactive branch 配置问题；需结合 `SHADOW_AA` active branch 和真实编译结果确认。 |
| Return type mismatch | 1478 | LSP type/macro alias modeling | `return MaterialFloat4(...)` 被推成 `define`，说明 macro-like type alias / constructor typing 仍不完整，应补 focused fixture。 |
| Assignment type mismatch | 3789 | LSP expression typing | ternary / macro constants 被推成 `ifndef` / `define` 等样本说明 conditional expression 和 define numeric value typing 仍需收敛。 |
| Builtin call type mismatch | 287 | Mixed: source + LSP matrix/shape modeling | `mul(float3x3, half2)` 样本可能是真实形状错误，也可能被上游 member/alias typing 污染，需要抽样编译确认。 |
| Function call argument count mismatch | 90 | Source review first | `SampleTexArryPkgNormalBias` 定义为 5 个参数，样本调用传 4 个参数；search root 内只找到该定义。 |
| Indeterminate builtin call | 33 | LSP builtin/type modeling | `sincos`、`log/log2` vector form、`fwidth`、`round`、`cosh` 等 builtin rule 未覆盖；另有 `define` 参数类型不可用。 |
| Unreachable code / potential missing return | 7671 | LSP control-flow precision + source review | 多个样本看起来不是不可达或缺 return，例如 `InShadowMapRange` 的 if/else 两支均 return；应补 control-flow focused fixture。 |
| Duplicate global declaration | 307 | Source review | `shading_models.hlsl` 中存在两个同签名 `void Init(...)`，优先按真实源码重复声明处理。 |
| Duplicate parameter declaration / local shadows parameter | 3 | LSP parser + source review | `1.025f` 被抽成 `025f` 的 duplicate parameter 样本显示 NSF 参数块 / numeric parser 仍有边界问题；`Local shadows parameter` 需要源码侧确认命名意图。 |
| Unterminated preprocessor conditional | 3 | LSP preprocessor parser/recovery | 报告样本指向 `smaa.hlsl:700 #if SMAA_INCLUDE_PS`，但同文件后续存在 `#endif // SMAA_INCLUDE_PS`；需先补 preprocessor conditional fixture 复核。 |

后续建议：

1. 先做 diagnostics policy 决策，即 `type-conversion-risk` 是否继续在 `balanced` 中默认显示。
2. 做 workspace config 宏清单，把 `COLOR_CHANGE_*`、`RENDER_VELOCITY`、`SHADOW_AA_*` 等项目宏来源分为真实编译配置、项目 enum 常量和源码缺省值。
3. 做下一轮 LSP focused fixtures：boolean literal undefined、macro define numeric typing、macro-like constructor return、multiline return / constructor expression、control-flow return completeness、`smaa.hlsl` conditional stack、builtin `sincos/log/log2/fwidth/round/cosh`。
4. 做源码侧抽样清单：`grass_max_offset` only-use、`SampleTexArryPkgNormalBias` 4/5 参数不一致、`GetVisibility` float2/float3 不一致、`shading_models.hlsl` duplicate `Init`。
5. 继续降低 audit 体量时，优先顺序是 conversion warning policy -> workspace macro config -> parser/control-flow false positives -> remaining type alias / builtin modeling。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_REAL_DIAGNOSTICS_AUDIT='1'; $env:NSF_REAL_DIAGNOSTICS_MAX_UNITS='0'; $env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS='7200000'; $env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL='phase-08-final-full'; node .\out\test\runCodeTests.js --mode real --workspace "D:\YYBWorkSpace\GitHub\nsp-intellision\out\test\diagnostics-audit\phase-04-preprocessor-context.code-workspace" --file-filter realWorkspace.diagnostics-audit` 通过，`1 passing`，耗时约 37 分钟。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增阶段 audit 输出；P8 分流内容并入本执行计划，无独立 2026-05-17 报告文档；无运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：否。
- 测试策略是否变化：否，沿用 P0/P7 已记录的 full audit 口径。
- 文档是否已同步：已更新本执行计划；当前事实文档无需更新。
- 是否改变公开 diagnostics 行为：否。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已生成 `phase-08-final-full` 稳定 real audit sample；未新增 failing focused fixture，避免在未实现后续修复前制造失败回归。
- 是否重新跑了对应验证并记录结果：是，已记录 C++ build 和 full audit。

Git 记录：

- 本阶段随 P9 收尾一起本地提交，commit message：`fix: gate conversion risk diagnostics by mode`；当前提交 hash 以 `git log -1 --oneline` 为准。

## Phase 9: diagnostics policy 与 workspace macro 分流

### 背景

Phase 8 full audit 显示 `type-conversion-risk=251553` 已成为最大剩余体量。这类 diagnostics 是合法但有风险的隐式转换 warning，适合源码审核，不适合继续作为默认 `balanced` 日常噪声。剩余 `Undefined macro in preprocessor expression` 则集中在项目宏 / enum 宏，应该进入 workspace/source 配置边界，而不是资源 bundle。

### 目标

把隐式转换风险 warning 从默认 `balanced` 移到 `full`，并记录 workspace macro 的长期处理边界。

### 方案

1. 在 diagnostics build options 中新增显式策略位，表达“是否发布合法但有风险的隐式转换 warning”。
2. 由 `nsf.diagnostics.mode` 派生该策略：
   - `basic`: 不发布 conversion-risk warning。
   - `balanced`: 不发布 conversion-risk warning。
   - `full`: 发布 conversion-risk warning。
3. 保持共享 type relation 作为唯一类型兼容事实来源；mode gate 只影响 warning 发布，不影响 mismatch / error 判定。
4. 把该策略纳入 deferred diagnostics fingerprint，避免 mode 切换后复用旧 full diagnostics。
5. 更新 integration tests：
   - 依赖 conversion-risk warning 的用例显式跑 `full`。
   - 新增 `balanced` 用例验证风险 warning 被压下，但真实 mismatch 仍保留。
6. 对 Phase 8 中暴露的项目宏做分流记录，明确 workspace/source config 边界，不写入资源 bundle。

### 验收标准

- `balanced` 下不再发布 truncation、narrowing、floating-integral、signedness、boolean conversion 等合法隐式转换风险 warning。
- `full` 下仍发布上述 conversion-risk warning，源码审核能力不丢失。
- assignment / return / call mismatch 等真实错误不受 warning gate 影响。
- 5-unit 和 50-unit real audit 中 `type-conversion-risk` 不再出现在默认 `balanced` category summary。
- 项目宏分流有明确 owner：workspace config、source include context 或项目编译配置；不新增资源 bundle 事实。

### Phase 9 执行记录

状态：已完成 diagnostics policy 实装、测试更新、宏分流报告、5/50-unit audit 验证和 P9 后 full audit 复核。

实现内容：

- 新增 `DiagnosticsBuildOptions::typeConversionRiskWarningsEnabled`，由 `nsf.diagnostics.mode` 派生：`basic=false`、`balanced=false`、`full=true`。
- `collectReturnAndTypeDiagnostics(...)` 继续统一使用共享 type relation 判定兼容性；只有合法风险 warning 受 mode gate 控制，assignment / return / call mismatch 错误仍照常发布。
- deferred full diagnostics fingerprint 纳入 `typeConversionRiskWarningsEnabled`，避免 mode 切换后复用旧 diagnostics。
- diagnostics 集成测试新增 mode helper：依赖 conversion warning 的用例显式跑 `full`，新增 `balanced` 用例验证风险 warning 被压下但硬 mismatch 仍保留。
- 更新用户设置说明、架构说明、测试口径和 VS Code setting enumDescription。
- 将 P9 policy、workspace macro triage 和最终 full audit 复核直接并入本执行计划，记录 `RENDER_VELOCITY`、`COLOR_CHANGE_*`、`CHANNEL_COLOR_CHANGE*`、`SHADOW_AA*` 等宏应由 workspace/source config 管理，不进入资源 bundle。

policy / audit 结果：

- `balanced` 现在不发布 `Implicit truncation/narrowing/floating-integral/signedness/boolean conversion` 等 conversion-risk warning。
- `full` 仍发布 conversion-risk warning；现有 HLSL implicit conversion focused 用例转为显式 `full` mode。
- `balanced` focused 回归确认 `Assignment type mismatch: float3 = float2.` 仍发布，说明真实 mismatch 不受 warning gate 影响。
- 5-unit smoke audit：`diagnosticsTotal=775`，相对 Phase 0 smoke baseline `4947` 下降 `4172`（`-84.33%`）；`type-conversion-risk` 未出现在 category summary。
- 50-unit trend audit：`diagnosticsTotal=6559`，相对 Phase 0 trend baseline `43341` 下降 `36782`（`-84.87%`）；`type-conversion-risk` 未出现在 category summary。
- 5-unit 和 50-unit audit 均为 `truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- P9 后 full audit：`diagnosticsTotal=69902`，相对 2026-05-16 baseline `463556` 下降 `393654`（`-84.92%`）；`filesWithDiagnostics=382`，相对 baseline `750` 下降 `368`（`-49.07%`）；`type-conversion-risk` 未出现在 category summary。
- P9 后 full audit triage：`likely-plugin-limitation=37186`，`needs-manual-review=22164`，`check-config-or-source=10549`，`likely-real-source=3`。
- P9 后 full audit category 前列：`semantic-source-rule=16817`、`indeterminate-analysis=14509`、`undefined-identifier=11733`、`preprocessor-context=10549`、`expression-type-analysis=5268`。
- P9 后 full audit 为 `Diagnostic wait timeouts=0`、`fileErrors=0`，`truncatedFiles=1`、`timedOutFiles=1` 均未比 baseline 增加；`prerequisiteSkippedTotal=178971`，其中 `parser_region_unreliable=121189`、`expression_type_unavailable=57782`，仍为 debug / audit metadata。

workspace macro 分流结论：

- `RENDER_VELOCITY`：项目 / pass 配置宏，样本来自 `shaderlib/deferred.hlsl:47`，并可在 meadow config XML 与部分 source default 中找到来源；应通过 workspace `nsf.preprocessorMacros` / `nsf.defines` 或真实 active unit context 提供。
- `COLOR_CHANGE_MODE`、`COLOR_CHANGE_*`、`CHANNEL_COLOR_CHANGE*`：项目 enum / material 参数宏，不同 PBR parameter 文件存在不同取值和默认逻辑；应优先由 source include context 提供，audit preset 只应模拟明确选择的 material family。
- `SHADOW_AA`、`SHADOW_AA_*`、`SHADOW_AA_PCF_ENABLE`：阴影宏常量和默认选项来自项目 config / shader source，如 `shaderlib/const_macros.hlsl` 与 `no_source/*/macro*.xml`；属于项目编译配置，不属于语言资源事实。
- 这些宏不进入正式资源 bundle，不新增 diagnostics 特判。

验证结果：

- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`72 passing`，`1 pending`。
- `$env:NSF_REAL_DIAGNOSTICS_AUDIT='1'; $env:NSF_REAL_DIAGNOSTICS_MAX_UNITS='5'; $env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL='phase-09-policy-macro-smoke-5'; node .\out\test\runCodeTests.js --mode real --workspace "D:\YYBWorkSpace\GitHub\nsp-intellision\out\test\diagnostics-audit\phase-04-preprocessor-context.code-workspace" --file-filter realWorkspace.diagnostics-audit` 通过，`diagnosticsTotal=775`，`type-conversion-risk` 未出现在 category summary，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- `$env:NSF_REAL_DIAGNOSTICS_AUDIT='1'; $env:NSF_REAL_DIAGNOSTICS_MAX_UNITS='50'; $env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL='phase-09-policy-macro-trend-50'; node .\out\test\runCodeTests.js --mode real --workspace "D:\YYBWorkSpace\GitHub\nsp-intellision\out\test\diagnostics-audit\phase-04-preprocessor-context.code-workspace" --file-filter realWorkspace.diagnostics-audit` 通过，`diagnosticsTotal=6559`，`type-conversion-risk` 未出现在 category summary，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- `$env:NSF_REAL_DIAGNOSTICS_AUDIT='1'; $env:NSF_REAL_DIAGNOSTICS_MAX_UNITS='0'; $env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS='7200000'; $env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL='phase-09-final-balanced-full'; node .\out\test\runCodeTests.js --mode real --workspace "D:\YYBWorkSpace\GitHub\nsp-intellision\out\test\diagnostics-audit\phase-04-preprocessor-context.code-workspace" --file-filter realWorkspace.diagnostics-audit` 通过，`1 passing`，耗时约 36 分钟；输出 `real-workspace-diagnostics-audit.phase-09-final-balanced-full.{json,md}`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 phase-09 audit 输出；P9 分流内容并入本执行计划，无独立 2026-05-17 报告文档；无运行时路径 / 资源命名变化。
- 架构或单一事实来源是否变化：是，diagnostics mode policy 成为共享 type relation warning 的发布边界；类型兼容知识仍只在共享层维护。
- 测试策略是否变化：是，conversion-risk 用例显式 `full`，并增加 `balanced` suppression 回归。
- 文档是否已同步：已更新 `README.md`、`package.json`、`docs/architecture.md`、`docs/development.md`、`docs/testing.md` 和本执行计划。
- 是否改变公开 diagnostics 行为：是，默认 `balanced` 不再发布合法隐式转换风险 warning。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补 `balanced` / `full` mode focused integration coverage，并生成 `phase-09-policy-macro-smoke-5`、`phase-09-policy-macro-trend-50` 与 `phase-09-final-balanced-full` audit sample。
- 是否重新跑了对应验证并记录结果：是。

Git 记录：

- 本阶段已本地提交，commit message：`fix: gate conversion risk diagnostics by mode`；当前提交 hash 以 `git log -1 --oneline` 为准。

## Phase 10: 收敛 common builtin indeterminate modeling

### 背景

P9 后 full audit 中最大 LSP-owned 剩余项之一是 `indeterminate-analysis=14509`。其中一批来自已知官方 builtin 缺少共享类型规则，例如 `log/log2`、`sincos`、`round`、`ddx/ddy`、`all/any`、`transpose` 和大小写变体 `Radians`。这些不应靠 diagnostics message suppress 处理，而应进入 `diagnostics_expression_type.*` 与 `hlsl_builtin_docs.*` 共享入口。

### 目标

消除 common builtin 的 `NSF_INDET_BUILTIN_UNMODELED`，同时保留参数类型无法推断时的 indeterminate metadata。

### 方案

1. 将 common builtin 纳入 `hlsl_builtin_docs.*` 的 type-checked fallback 名单。
2. 在 `resolveBuiltinCall(...)` 中统一大小写归一化，避免 `Radians(...)` 这类项目写法绕过共享规则。
3. 在共享 builtin resolver 中补齐：
   - 同型返回：`log/log2/log10/round/radians/degrees/ddx/ddy/ddx_coarse/ddy_coarse/ddx_fine/ddy_fine/fwidth`
   - bool 返回：`all/any`
   - 矩阵转置：`transpose`
   - call-only：`sincos`
4. 新增 focused fixture，证明这些 builtin 不再产生 unmodeled / arg-unavailable indeterminate 或 builtin mismatch。
5. 保持 macro / parser 导致的空参数类型为 indeterminate，不新增 fallback 猜测类型。

### Phase 10 执行记录

状态：已完成 common builtin indeterminate modeling 第一轮。

实现内容：

- `hlsl_builtin_docs.*` 的 fallback / type-checked fallback 名单补充 `log/log2/log10/round/radians/degrees/ddx/ddy/ddx_coarse/ddy_coarse/ddx_fine/ddy_fine/fwidth/all/any/transpose/sincos`。
- `diagnostics_expression_type.*` 在 `resolveBuiltinCall(...)` 内统一 builtin 名大小写，并新增上述 common builtin 的共享返回类型 / call validation 规则。
- `sincos(...)` 作为 call-only builtin 只验证参数形状和数值类型，表达式返回仍保持不可用，避免伪造 `void` 或错误返回类型。
- 新增 focused fixture `test_files/module_diagnostics_hlsl_builtin_indeterminate_modeled.nsf`，覆盖 `log/log2/log10/round/Radians/degrees/ddx/ddy/fwidth/all/any/transpose/sincos`。
- diagnostics 集成测试新增 `models common builtin intrinsics without indeterminate diagnostics`，等待硬 mismatch 哨兵出现后断言上述 builtin 不再发布 unmodeled / arg-unavailable indeterminate 或 builtin mismatch。
- 本阶段未处理 macro / parser 造成的 `arg types unavailable`，例如 `abs()`、`max(float, )`、`min(float, ifndef)`、`pow(float, )` 和 object method `SampleBias(..., )`，这些属于下一轮 macro-expression / parser boundary 治理。

公开行为变化：

- common builtin 合法调用不再产生 `Indeterminate builtin call: type rules not implemented`。
- `Radians(...)` 这类大小写变体进入同一共享规则。
- 参数类型缺失仍发布 indeterminate，不静默降级或猜测。

验证结果：

- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`73 passing`，`1 pending`。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-10-builtin-indeterminate-smoke-5.{json,md}`；`diagnosticsTotal=700`，`NSF_INDET_BUILTIN_UNMODELED=0`，`indeterminate-analysis=35`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-10-builtin-indeterminate-trend-50.{json,md}`；相对 P9 50-unit：`diagnosticsTotal` `6559 -> 5848`，`filesWithDiagnostics` `31 -> 27`，`likely-plugin-limitation` `2858 -> 2147`，`indeterminate-analysis` `1073 -> 362`，`NSF_INDET_BUILTIN_UNMODELED` `709 -> 0`，`prerequisiteSkippedTotal` `16342 -> 15852`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和 phase-10 audit 输出；无运行时路径 / 资源命名变化。
- 架构或单一事实来源是否变化：是，common builtin 类型规则继续收敛到 `diagnostics_expression_type.*` / `hlsl_builtin_docs.*` 共享入口。
- 测试策略是否变化：是，新增 common builtin focused fixture 和阶段 audit sample。
- 文档是否已同步：已更新 `docs/architecture.md`、`diagnostics_expression_type.hpp` 和本执行计划；资源、开发、testing 和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，common builtin 合法调用不再发布 unmodeled indeterminate。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-10 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是。

Git 记录：

- 本阶段已本地提交，commit message：`fix: model common builtin diagnostics`；当前提交 hash 以 `git log -1 --oneline` 为准。

## Phase 11 (P11): 收敛 macro-expression / parser boundary argument availability

### 背景

P10 已消除 common builtin 的 `NSF_INDET_BUILTIN_UNMODELED`，但仍明确留下 macro / parser 造成的 `arg types unavailable`，例如 `abs()`、`max(float, )`、`min(float, ifndef)`、`pow(float, )` 和 object method `SampleBias(..., )`。这类问题不能通过 builtin 表继续补洞；根因更可能在 callsite argument splitting、macro / conditional token 边界、parser recovery region 和 object method argument 解析之间。

### 目标

让 diagnostics 能区分三类情况：

- 源码真实缺参数，应保留明确 call argument diagnostics。
- macro / conditional source 造成当前 token stream 不完整，应通过 diagnostics prerequisites 或结构化 indeterminate reason 记录，不猜测类型。
- parser / callsite 边界误切导致的参数不可用，应在共享 parser / call query 层修正，而不是在单个 builtin 或 object method rule 中 suppress。

### 方案

1. 从 `phase-10-builtin-indeterminate-trend-50` 和后续 5-unit smoke 中抽取 `arg types unavailable` top samples，按真实缺参、macro 条件空参、parser boundary、object method argument split 四类归因。
2. 优先检查 `callsite_parser.*`、`diagnostics_expression_type.*`、object method diagnostics 参数收集路径和 `diagnostics_prerequisites.*` 的边界契约。
3. 如果根因是 parser region unreliable，应把 reason 收敛到 prerequisites / audit metadata；如果根因是 argument splitter 误切，应修共享 callsite parser。
4. 新增 focused fixture，至少覆盖：
   - 真实空参数仍发布 error。
   - macro / conditional 造成的不可判定参数不发布伪 mismatch。
   - object method `SampleBias` / texture-like call 不因 trailing macro segment 误报。
   - builtin `abs/max/min/pow` 不再因为合法 macro 包裹表达式落入 unstructured indeterminate。
5. 跑 diagnostics repo 回归、5-unit smoke 和 50-unit trend audit；只有样本仍不可归因时才追加 full audit。

### 验收标准

- `arg types unavailable` 的主要样本被分为真实源码问题或明确 parser / macro prerequisite reason。
- 50-unit audit 中 `indeterminate-analysis` 继续下降，且不通过 suppress、fallback、shim 或猜测类型达成。
- object method 与 builtin call diagnostics 继续消费共享 callsite / type relation / type model 入口。
- 新增 focused fixture 覆盖真实缺参与 macro/parser 不可判定的正反行为。

### Phase 11 执行记录

状态：已完成 macro-expression / parser boundary argument availability 第一轮。

样本归因：

- `abs(u_vfog_max_distance)`、`max(..., u_exp_fog_start_distance)`、`pow(..., max(..., u_dir_inscattering_exponent))` 的空参数类型来自 object-like macro replacement 未进入 expression typing，而不是 builtin 表缺项。
- `min(ray_length, u_exp_fog_max_distance_km * UNIT_KM_TO_CM)` 的 `Args: (float, ifndef)` 来自 symbol type fallback 把 `#ifndef UNIT_KM_TO_CM` 误读成普通声明。
- `SampleBias(..., TEXTURE_BIAS)` 的末尾空参数来自有效 macro 数字 replacement 未通过共享 preprocessor context 传给 object method argument typing。
- 真实空参数仍按 call type mismatch 发布，不转成 indeterminate。

实现内容：

- `preprocessor_view.*` 新增 active macro replacement 查询契约，记录 initial macro state、源文件 `#define/#undef` 事件和 include 链宏传播后的 delta，供下游按行查询。
- `diagnostics_expression_type.*` 消费同一 `PreprocessorView`，对 active object-like macro replacement 递归使用共享表达式 parser 推断类型；function-like macro 不展开，缺上下文时仍保持不可判定，不新增猜测类型。
- `diagnostics_symbol_type.*` 的 current-text symbol fallback 跳过预处理指令行，避免把 `#ifndef/#define` directive token 当作变量声明类型。
- `callsite_parser.*` 和 expression argument parser 在拆分参数时纳入 `{}` brace depth，避免 initializer / braced segment 内逗号误切。
- builtin / object method call diagnostics 区分真实空参数 range 和非空参数的类型不可用；真实空参数发布 call type mismatch，非空但类型缺失仍走 indeterminate / prerequisite 路径。
- 新增 focused fixture `test_files/module_diagnostics_macro_argument_availability.nsf`，覆盖 macro alias、numeric macro、object method `SampleBias` 和真实空参数。
- diagnostics 集成测试新增 `keeps macro expression arguments available for builtin and object method diagnostics`。

公开行为变化：

- 合法 object-like macro 包裹的 builtin / object method 实参不再发布 `arg types unavailable` indeterminate。
- `#ifndef` 等预处理指令不再被 symbol fallback 当作类型。
- 真实空参数继续作为用户可见 call type mismatch，而不是被归类为 indeterminate metadata。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`74 passing`，`1 pending`。
- `npm run test:client:repo` 通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-11-argument-availability-smoke-5.{json,md}`；相对 P10 5-unit：`diagnosticsTotal` `700 -> 635`，`likely-plugin-limitation` `240 -> 175`，`indeterminate-analysis` `35 -> 0`，`expression_type_unavailable` `485 -> 455`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-11-argument-availability-trend-50.{json,md}`；相对 P10 50-unit：`diagnosticsTotal` `5848 -> 5186`，`filesWithDiagnostics` `27 -> 24`，`likely-plugin-limitation` `2147 -> 1485`，`indeterminate-analysis` `362 -> 0`，`expression-type-analysis` `350 -> 50`，`prerequisiteSkippedTotal` `15852 -> 15552`，`expression_type_unavailable` `4394 -> 4094`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和 phase-11 audit 输出；无运行时路径 / 资源命名变化。
- 架构或单一事实来源是否变化：是，active macro replacement 查询收敛到 `preprocessor_view.*`，macro expression typing 收敛到 `diagnostics_expression_type.*`。
- 测试策略是否变化：是，新增 macro argument availability focused fixture 和阶段 audit sample。
- 文档是否已同步：已更新 `docs/architecture.md`、`preprocessor_view.hpp`、`diagnostics_expression_type.hpp` 和本执行计划；README、资源、development、testing 和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，合法 macro 实参不再发布 arg-unavailable indeterminate，真实空参数保持 call mismatch。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-11 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是。

Git 记录：

- 本阶段已本地提交，commit message：`fix: keep macro arguments available in diagnostics`；当前提交 hash 以 `git log -1 --oneline` 为准。

## Phase 12 (P12): 收敛 literal / macro-like expression typing

### 背景

P8 / P9 full audit 仍显示 `undefined-identifier`、`semantic-source-rule` 和 `expression-type-analysis` 中存在 LSP-owned 剩余项：`true` / `false` 被当成普通 identifier、macro define numeric typing 不稳定、`MaterialFloat4(...)` 这类 macro-like constructor 不能稳定进入 expression type，以及 `normalize(mul(camera_matrix, ...))` 等矩阵 / alias 组合仍可能残留类型不可判定。

### 目标

把 boolean literal、numeric macro value、macro-like constructor / alias 和矩阵表达式的类型入口收敛到共享 expression typing / type desc 层，使后续 diagnostics 不再靠 rule-local 猜测。

### 方案

1. 在 `diagnostics_expression_type.*` / `type_desc.*` 中审查 boolean literal、numeric macro replacement、macro-like scalar / vector / matrix alias 的归一化职责。
2. 对 `true` / `false` 建立 focused undefined-identifier 回归，确保 boolean literal 不进入普通 symbol undefined 路径。
3. 对 `MaterialFloat*`、`MaterialHalf*`、matrix alias 和 project macro numeric value 增加 focused fixture，证明 assignment / return / builtin argument 使用同一类型结果。
4. 对 `mul(...)`、`normalize(...)`、constructor return 和 multiline return expression 增加组合用例，避免 P12 修类型时重新引入 parser boundary 误判。
5. 更新 architecture / header contract，只记录共享职责变化；不把项目 alias 写成 diagnostics rule 局部表。

### 验收标准

- boolean literal 不再产生 undefined identifier。
- macro-like numeric / vector / matrix alias 在 assignment、return、builtin argument 中使用同一 expression type。
- 50-unit audit 中 `undefined-identifier` 和 `expression-type-analysis` 的 LSP-owned 样本继续下降。
- 如果新增或扩展 public API，头文件职责、输入输出、调用前提和非目标范围同步更新。

### Phase 12 执行记录

状态：已完成 literal / macro-like expression typing 收敛。

根因判断：

- expression typing 已能识别 `true` / `false`，但 undefined identifier 扫描未消费同一 literal 分类入口，导致 boolean literal 仍进入普通 symbol undefined 路径。
- P11 已把 active object-like macro replacement 暴露给 expression typing，但 declaration-side 类型 token 和 macro-like constructor call 没有统一按当前行 active replacement 归一；`MaterialFloat4(...)` 这类构造在 active branch 下应先解析成实际 replacement 类型，再进入 assignment / return / builtin argument 的共享 type relation。

实现内容：

- `diagnostics_expression_type.*` 新增 `normalizeTypeTokenWithPreprocessor(...)`，按当前行 active macro replacement 归一 declaration-side 类型 token；replacement 不是单一 numeric type token 时不猜测。
- expression parser 对 object-like macro replacement 得到的 numeric type 增加 constructor-call 消费：`MaterialFloat4(...)` 会先按 active replacement 得到 `half4` / `float4`，再跳过参数列表并继续支持后续 postfix，例如 `.xyz`。
- constructor fallback 继续消费 `type_desc.*` 的共享轻量 numeric alias 形状；active macro replacement 优先级高于静态 alias 形状。
- undefined identifier 扫描改为复用 `inferLiteralType(...)`，`true` / `false` 不再走普通 symbol undefined 诊断路径。
- 新增 focused fixture `test_files/module_diagnostics_literal_macro_like_expression_typing.nsf`，覆盖 boolean literal、numeric object-like macro、active `MaterialFloat*` / matrix alias constructor、`mul(...)` / `normalize(...)` 组合、constructor postfix swizzle 和 hard mismatch sentinel。
- diagnostics 集成测试新增 `keeps literal and macro-like expression typing shared`，证明合法 literal / macro-like expression 不再产生 undefined、return mismatch、constructor swizzle truncation warning 或 builtin / user-call mismatch。

公开行为变化：

- `true` / `false` 不再发布 `Undefined identifier` diagnostics。
- active object-like macro 类型别名构造（例如 `MaterialFloat4(...)`）按当前 active replacement 进入 expression type；合法 assignment / return / builtin argument 不再因字面 alias 名称或未消费 constructor 参数产生误报。
- replacement 缺失时只消费 `type_desc.*` 已有共享轻量形状；function-like macro、replacement 不是单一 numeric type token，或 `type_desc.*` 也无法归一时仍不猜测类型，不新增 suppress / fallback / shim。

验证结果：

- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`75 passing`，`1 pending`。
- `npm run test:client:repo` 通过。
- 5-unit smoke audit 使用可比阶段 workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 通过，输出 `real-workspace-diagnostics-audit.phase-12-literal-macro-typing-smoke-5.{json,md}`；相对 P11 5-unit：`diagnosticsTotal` `635 -> 555`，`filesWithDiagnostics` `20 -> 19`，`undefined-identifier` `116 -> 36`，`expression-type-analysis=5` 保持，`call-type-analysis=24` 保持，`prerequisiteSkippedTotal=1645` 保持，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 使用同一可比阶段 workspace 通过，输出 `real-workspace-diagnostics-audit.phase-12-literal-macro-typing-trend-50.{json,md}`；相对 P11 50-unit：`diagnosticsTotal` `5186 -> 4420`，`filesWithDiagnostics` `24 -> 23`，`likely-plugin-limitation` `1485 -> 719`，`undefined-identifier` `944 -> 178`，`expression-type-analysis=50` 保持，`call-type-analysis=204` 保持，`prerequisiteSkippedTotal=15552` 保持，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- phase-12 5-unit / 50-unit 报告中未检出 `Undefined identifier: true.`、`Undefined identifier: false.`、`Assignment type mismatch: Material*` 或 `Return type mismatch: expected Material*`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 和 phase-12 audit 输出；无运行时路径 / 资源命名变化。
- 架构或单一事实来源是否变化：是，declaration-side type token 与 macro-like constructor typing 收敛到 `diagnostics_expression_type.*`，active macro replacement 仍由 `preprocessor_view.*` 提供。
- 测试策略是否变化：是，新增 literal / macro-like expression typing focused fixture 和阶段 audit sample。
- 文档是否已同步：已更新 `docs/architecture.md`、`diagnostics_expression_type.hpp` 和本执行计划；README、AGENTS、resources、testing、development 和对象类型 / 方法契约未变化。
- 是否改变公开 diagnostics 行为：是，boolean literal 和合法 active macro-like constructor 不再发布对应误报。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已新增 focused fixture，并生成 phase-12 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是。

## Phase 13 (P13): P11/P12 后 full audit 和剩余问题分流

### 背景

P9 full audit 已把默认 `balanced` diagnostics 从 463556 降到 69902；P10 的 50-unit trend 继续消除了 common builtin unmodeled 问题。P11 / P12 后需要重新判断剩余 diagnostics 是否仍由 LSP 架构缺陷主导，还是已经进入真实源码、项目配置和规则策略确认阶段。

### 目标

执行一轮 post-P12 full audit，形成下一轮是否继续 LSP 架构治理的依据，并把剩余问题按 owner 分流。

### 方案

1. 在 P11 / P12 focused fixture、repo diagnostics 回归、5-unit smoke 和 50-unit trend 均通过后，执行 full audit。
2. 对 full audit 输出按 category、top canonical message、affected unit / file 和 sample line 重新聚类。
3. 将剩余样本分为：
   - LSP 共享层缺陷。
   - 项目 workspace / source config 缺失。
   - 真实源码问题。
   - diagnostics policy 待确认问题。
4. 若 `likely-plugin-limitation` 仍由单一 LSP 根因主导，新增下一轮 Phase；若不再主导，则把本轮 diagnostics 架构治理收束为源码 / 配置审核清单。

### 验收标准

- full audit 可稳定完成，`fileErrors=0`，`truncatedFiles` / `timedOutFiles` 不比 baseline 增加。
- `likely-plugin-limitation` 的主导 top groups 有明确 owner，不再是 parser/type/scope 级联误报。
- 文档记录下一轮是否继续 LSP 架构治理，以及对应优先级、风险和验证入口。

### Phase 13 执行记录

状态：已完成 post-P12 full audit 和剩余问题第一轮 owner 分流。

验证结果：

- 使用 Phase 4 之后的可比 copy workspace `out/test/diagnostics-audit/phase-04-preprocessor-context.code-workspace` 跑 full 813-unit audit，避免修改真实 workspace，同时保留补齐后的 `nsf.preprocessorMacros` preset 口径。
- `$env:NSF_REAL_DIAGNOSTICS_AUDIT='1'; $env:NSF_REAL_DIAGNOSTICS_MAX_UNITS='0'; $env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS='7200000'; $env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL='phase-13-post-p12-full'; node .\out\test\runCodeTests.js --mode real --workspace "D:\YYBWorkSpace\GitHub\nsp-intellision\out\test\diagnostics-audit\phase-04-preprocessor-context.code-workspace" --file-filter realWorkspace.diagnostics-audit` 通过，`1 passing`，耗时约 43 分钟。
- 输出 `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-13-post-p12-full.{json,md}`，同时写出 timestamp 归档 `real-workspace-diagnostics-audit.2026-05-18T04-10-52-724Z.{json,md}`。
- `fileErrors=0`，`truncatedFiles=1`，`timedOutFiles=1`；truncated / timed-out 与 2026-05-16 full baseline、P9 full 均持平，未增加。

总体趋势：

- 相对 2026-05-16 baseline：`diagnosticsTotal` `463556 -> 41720`，下降 `91.00%`；`filesWithDiagnostics` `750 -> 307`；`likely-plugin-limitation` `373861 -> 9013`；`numeric-literal=0`。
- 相对 P9 full：`diagnosticsTotal` `69902 -> 41720`，继续下降 `28182`；`likely-plugin-limitation` `37186 -> 9013`；`indeterminate-analysis` `14509 -> 8`；`undefined-identifier` `11733 -> 2588`；`expression-type-analysis` `5268 -> 816`；`prerequisiteSkippedTotal` `178971 -> 168725`。
- P11 / P12 的主要目标在 full audit 中成立：common builtin unmodeled、macro argument unavailable 和 boolean / macro-like constructor 级联不再主导 full 结果。

剩余 owner 分流：

- 项目 workspace / source config 缺失：`Undefined macro in preprocessor expression` 共 `10549`，样本包括 `RENDER_VELOCITY`、`COLOR_CHANGE_MODE`、`COLOR_CHANGE_PICKER`、`COLOR_CHANGE_MULTIPLE`、`COLOR_CHANGE_GRADIENT`。这类问题优先由真实编译配置、项目 preset 或源码宏定义确认，不应在 diagnostics rule 里本地 allowlist。
- 真实源码 / 手工审核优先：`Duplicate local declaration` `10278`、`Unreachable code` `6210`、`Missing semicolon`（syntax-structure / needs-manual-review）`3886`、`Potential missing return on some paths` `1461`、`Duplicate global declaration` `307`、`Missing return statement` `10`、`Unterminated preprocessor conditional` `3`。这些已经不是 numeric/type/scope 级联主导，下一步应结合真实编译结果决定源码修复或 diagnostics policy。
- LSP parser / recovery 后续治理：`Missing semicolon`（effect-syntax-or-macro / likely-plugin-limitation）`3056` 仍集中在合法多行 `return lerp(...)`、`return float3x3(...)`、多行赋值 / constructor continuation 上；`Indeterminate assignment type: rhs type unavailable` `6` 也集中在多行 RHS 表达式。若继续治理，优先开窄范围 parser continuation phase，验证入口是 focused fixture、diagnostics repo、5-unit / 50-unit / full audit。
- LSP macro expansion / parser 边界后续治理：`Undefined identifier` `2588` 不是单一根因；样本分为真实缺失符号如 `grass_max_offset`，以及 macro 展开边界如 `HAIR_SHADING_PARAMS_PREPARE` 中的 `VoL`。后者应和 parser continuation / macro line-continuation recovery 一起收敛，前者应进源码 / 配置清单。
- diagnostics policy / source confirmation：`Function call argument mismatch` `2168` 主要是 `GetVisibility(float, float3)` 被传入 `float2`；`Function call argument count mismatch` `90` 主要是 `SampleTexArryPkgNormalBias` 定义 5 参、调用 4 参；`Assignment type mismatch` `789` 主要是 `half4 = half3`。这些需要真实编译结果确认是源码问题、项目编译器扩展，还是 diagnostics policy 应降噪。
- 小型 LSP type/modeling 后续项：`Builtin call type mismatch: mul(float3x3, half2)` `287` 涉及项目注释中的编译器转换行为；`Return type mismatch: expected uint64_t but got int` `26` 指向 64-bit integer / bitwise shift-or 表达式建模；`Binary operator type mismatch` `1`、`cosh` / `clip` builtin unmodeled 各 `1`。这些不再主导 overall audit，可按低风险 focused fixture 单独处理。

下一轮建议：

- 不需要再开一个大范围 diagnostics 架构治理 phase；P13 后剩余 `likely-plugin-limitation` 不再由单一 LSP 根因主导。
- 若继续做 LSP 侧，优先拆成小阶段：parser continuation / macro line-continuation recovery、64-bit integer + bitwise expression typing、少量 builtin modeling / project-specific `mul` policy。任何会改变公开 diagnostics 行为的阶段，开工前仍按仓库规则先确认风险和预期行为变化。
- 并行把 `Undefined macro`、duplicate / unreachable / missing-return / argument mismatch 等 top groups 输出为源码 / 配置审核清单；这部分不应通过 suppress、fallback、shim 或 diagnostics-local 特判解决。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 phase-13 audit 输出和临时 stdout / stderr / exitcode 日志；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：否，本阶段只审计和分流。
- 测试策略是否变化：否，沿用 P0 full audit 口径和 P4 之后的可比 workspace。
- 文档是否已同步：已更新本执行计划；README、AGENTS、architecture、resources、testing、development 和对象类型 / 方法契约均无当前事实变化。
- 是否改变公开 diagnostics 行为：否。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：本阶段不新增 fixture，已生成稳定 phase-13 full audit sample。
- 是否重新跑了对应验证并记录结果：是。

## Phase 14 (P14): active-branch 宏上下文契约

### 重置说明

2026-05-19 起，P14 全量推翻重来。此前围绕 histogram、owner taxonomy、稳定常量试探、阶段 workspace preset 试探、`GL3_PROFILE` / `FOLIAGE_MODE` / 旧版 `P14B` / 旧版 `P14C` 子批次的执行记录、返工记录和“已完成/已关闭”结论，均已作废并从本文删除，不再作为执行依据、验收依据或趋势比较依据。

本次重置只保留一个新前提：P14 的问题定义改为“active branch 所需的宏上下文契约是否成立”，而不是“是否继续推导更多宏值”。

### 背景

P13 full audit 剩余 `preprocessor-context` 共 `10549` 条，典型样本集中在 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE`、`RENDER_VELOCITY` 等宏。

2026-05-22 问答确认后，P14 进一步收敛为“共享预处理宏推导核心层”。它不是给若干业务宏补固定值，而是按当前 unit、include 顺序、分支状态和用户配置构建可追踪的宏状态机，供 hover、跳转和 diagnostics 共用。

核心矛盾不是“LSP 还缺多少默认值”，而是当前 active unit 是否拿到了足够的上下文，使预处理器在使用点能够确认两件事：

- 这个宏在此处是否已定义。
- 当前 `#if` / `#elif` 应该进入哪条 active branch。

### 目标

建立共享宏推导核心层。hover、跳转、diagnostics 必须消费同一份宏推导快照；编辑态能力同优先，且必须满足项目现有性能指标。

P14 的完成标准不再是“收集了多少宏候选值”，而是：

- 严格宏状态机成立，覆盖 include 顺序、分支状态、`#define`、`#undef`、`#ifdef`、`#ifndef`、`defined(...)`、符号链、用户配置、函数式宏和缺失后补 `0`。
- `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 三类主痛点宏都完成独立验收，不能只打通其中一类就关闭 P14。
- shared runtime / debug / audit 能明确区分真实来源、`#ifndef` 默认值、用户配置、缺失后补 `0`、展开链异常、分支汇合和非 active 分支状态。

### 已确认规则

宏来源与优先级：

- 合法来源为当前 unit 可见宏和用户配置宏。当前 unit 包含 unit 本体以及按真实 include 展开顺序可见的 include 内容。
- 同名冲突时，当前 unit 的当前链路状态优先，用户配置只补缺口；源码后续 `#define` 可以覆盖用户配置，且不报错。
- 用户配置只支持定义宏，不支持用户侧 `undef`；空值配置按已定义且值为 `0`。
- 用户配置表达式属于 P14 最终语义范围，按 C 预处理表达式求值；其中的符号链和缺失叶子宏继续走同一套推导规则。
- 如果用户配置表达式能力需要扩展现有用户配置语法或改变现有字段解释方式，必须按公开配置行为变化单独确认，并同步 `README.md`、`docs/resources.md` 和 `docs/testing.md`。

顺序宏状态机：

- 严格按 include 展开顺序和源码顺序处理 `#define`、`#undef`、`#ifdef`、`#ifndef`、`defined(...)`、`!defined(...)`。
- `#ifndef` / `!defined(...)` 中的默认定义只有在当前链路里位于使用点之前且确实生效时，才算当前 unit 的正式来源。
- `#undef` 会真实清空当前宏状态；之后再次数值使用同一宏时，进入新的缺失周期并重新报错、补 `0`。
- `#define FOO` 这种无 replacement 宏视为已定义且值为 `0`，并可沿符号链继续传播。
- 同一路径重复 `#define` 时以后一个为准，不额外发布重定义诊断。

缺失与补值：

- `#ifdef FOO`、`#ifndef FOO`、`defined(FOO)` 这类存在性判断中，`FOO` 未定义不算错误。
- `#if FOO`、`#if FOO == 1` 或混合表达式中的数值使用，如果宏未定义，则在该数值使用位置发布 `宏初始定义缺失` Error，再补 `0` 继续推导。
- 同一表达式中同一个缺失叶子宏只报一次；同一分支中同一个缺失叶子宏只在首次使用位置报一次，后续复用补值。
- 缺失后补出的 `0` 在当前分支内同时具备数值态和已定义态；后续 `defined(FOO)` 视为 true。
- 补值先只在当前分支生效，不立即污染其他分支；跨 include 返回后，只要仍在同一条推导链上，补值继续有效。
- 如果后续出现真实 `#define`，从该位置开始切换为真实值，不回溯重算前面的推导结果。

分支推导：

- 开发态不做全局剪枝，各分支按各自的宏状态继续推导，未激活分支里的 Error 也默认进入 diagnostics。
- 分支汇合后发布 Info，后续采用 active branch 的宏状态继续；其他分支状态和值作为 info/debug 元数据保留。
- 同一位置、同类、同叶子根因的多分支错误在展示层默认分组；组内保留触发 unit、上下文摘要和 include 链。
- 底层 diagnostics 去重键至少包含文件位置、宏名 / 叶子根因、触发 unit、上下文摘要和 include 链；展示分组可以按文件位置、错误类型、宏名 / 叶子根因收敛。
- 底层 diagnostics entity 必须逐 context 保留；展示分组只属于 presentation 层，必须通过 stable group id / diagnostic data 保留组内明细，不能改变底层计数、audit 统计或跳转来源。
- 仅未激活分支命中的错误组需要明显标记；同时命中 active branch 的错误组也要标明 active branch 命中。排序先按是否命中 active branch，再按严重级别。
- 不提供用户可见的 active-only 过滤开关；全分支结果默认可见。

符号链和宏展开：

- 符号链必须尽量解析到最终值；若链路断在缺失叶子宏，例如 `A -> B` 但 `B` 缺失，则报 `B` 宏未定义，给 `B=0` 后继续推导整条链。
- `function-like macro`、嵌套实参、变参宏、`__VA_ARGS__`、`##` token pasting 和 `#` stringization 都属于 P14 完整语义范围。
- 函数式宏实参先按同一套规则求值和补缺，再进入宏体展开；宏体内部引用到的外部宏也按同一套规则处理。
- 函数式宏参数替换只在本次展开作用域内生效，不污染外部宏状态；实参拆分必须按真实预处理规则处理嵌套括号、嵌套调用和逗号。
- `##` 拼出的新 token 若形成宏名，需要继续作为普通宏递归解析；`##` / `#` 产出非法 token 或无法继续解析时，按 `展开链异常` Warning 处理，落点在原始未展开位置。
- 宏展开必须有严格递归 / 深度保护；触发保护时发布 `展开链异常` Warning，再按可继续路径补 `0` 推导。

导航、hover 与 diagnostics：

- hover 主展示只看当前 active unit + active branch 的当前值；其他分支候选值、补 `0`、展开异常、`#ifndef` 默认值路径、符号链和用户配置表达式求值过程放次级信息。
- 如果 active branch 当前值来自补 `0`，hover 主展示仍显示该补值；若其他分支有真实值，只放次级信息。
- 如果 active branch 当前值稳定真实，但其他分支存在补 `0` 或展开链异常，hover 主展示保持 active 值，同时给轻提示并把详情放次级信息。
- 多步符号链主展示最终值，次级展示完整链路；若链路中有补 `0`，次级链路标明 synthesized。
- 多次状态切换的宏，hover 次级信息展示状态时间线；长时间线默认显示摘要和关键节点，完整时间线按需展开。关键节点至少包含首次定义 / 首次缺失、`#undef`、分支汇合、覆盖来源切换、展开链异常和 `#ifndef` 默认值生效。
- 用户配置来源不作为 `Go to Definition` 的默认 Location；当当前值来自用户配置时，`Go to Definition` 默认返回空结果，不跳到用户配置，hover 显示用户定义宏 / 用户配置表达式来源。真实 define 位于 include 中时，跳转直接到 include 的真实定义。
- 用户配置被源码真实 define 覆盖时不进 diagnostics；hover / debug 次级信息保留“用户配置值被源码覆盖”的说明。
- 当前生效值若来自缺失后补 `0`，跳转到这次 Error 对应的首次缺失位置；如果位置在 include 中，直接跳到 include 的首次缺失处，并给轻量交互提示，不新增 diagnostics。
- include 内 diagnostics 挂在 include 的实际使用位置，同时保留触发 unit 和 include 链路；同一 include 位置被不同 unit / 不同上下文触发时，底层诊断不归并。

缓存与刷新：

- 词法 / 解析结果可以缓存，宏推导结果必须上下文敏感；缓存键至少包含宏值摘要、active branch 路径、include 调用栈和进入位置。
- hover、跳转、diagnostics 优先基于同一份快照返回；连续快速编辑时合并抖动，只保留最新一次重算，旧任务取消即作废。
- 对外已发布的 hover / 跳转 / diagnostics 可以来自上一份完整且内部一致的快照；freshness / pending 只表示后台调度状态，不阻止已发布快照被消费，也不得让三个能力分别读取不同版本的部分结果。
- 为等待统一快照可以显示轻量 loading / 状态提示；旧任务取消后静默切到新快照，不额外提示。
- 受影响文件较多时，小批量合并推送结果，避免逐文件抖动，也不等待全量结束。
- freshness / pending 标记只用于 debug / audit，不向用户暴露；粒度为上下文实例级，即 unit + include 链 + branch 路径 + 宏摘要。

### 问答案例

基础解析伪代码：

```text
resolve_macro(name, usage):
  if current_unit_chain_has_live_value(name):
    return unit live value

  if user_config_has_value(name):
    return user config value

  if usage is existence check:
    return undefined without diagnostic

  emit Error at numeric usage: macro initial definition missing
  define synthesized name = 0 in current branch state
  return 0
```

`#ifndef` 默认值只在当前链路和使用点之前生效：

```c
#ifndef COLOR_CHANGE_MODE
#define COLOR_CHANGE_MODE 0
#endif

#if COLOR_CHANGE_MODE == COLOR_CHANGE_PICKER
#endif
```

如果这段默认定义按真实 include 顺序位于使用点之前，`COLOR_CHANGE_MODE=0` 是 unit 内正式来源，不报缺失。如果默认定义位于使用点之后，则使用点仍按缺失处理，先报 Error，再补 `0` 继续推导，后续走到真实 `#define` 后从该位置开始切换。

`#undef` 会开启新的缺失周期：

```c
#if FOO == 0
#endif

#undef FOO

#if FOO == 0
#endif
```

第一次数值使用 `FOO` 时若缺失，报一次并补 `0`。`#undef FOO` 会清空当前状态，后续再次使用 `FOO` 时视为新的缺失周期，需要重新报错并再次补 `0`。

存在性判断不报缺失，数值使用才报：

```c
#if defined(FOO) && FOO == 1
#endif
```

如果 `FOO` 未定义，`defined(FOO)` 正常求为 false，不报错；后面的 `FOO == 1` 是数值使用，即使左侧已足以决定短路结果，也仍然报 `FOO` 缺失并补 `0`，因为 diagnostics 要完整扫描表达式。

同一表达式里的多个缺失宏都要收集：

```c
#if FOO == 1 && BAR == 2
#endif
```

即使 `FOO` 补 `0` 后左侧已经足以决定表达式结果，`BAR` 仍需要继续求值、报缺失并补 `0`。同一表达式里同一个缺失宏重复出现时，只报一次。

分支内各自推导，汇合后按 active branch 收敛：

```c
#if A
#define X 1
#else
#define X 2
#endif

#if X == 1
#endif
```

两个分支都记录各自的 `X` 值并继续推导。汇合点发布 Info，后续主链使用 active branch 的 `X` 值；其他分支值进入 hover 次级信息和 debug 元数据，不污染 active hover 主展示。

符号链缺失定位到叶子宏：

```c
#define A B
#define C B

#if A == 1 || C == 2
#endif
```

真正缺失的是叶子宏 `B`。同一表达式内 `B` 只报一次，给 `B=0` 后继续完成 `A`、`C` 的推导；如果换到另一分支，另一分支可以有自己的首次缺失 Error，但元数据要标明共享同一个叶子根因。

空 replacement 按 `0` 沿链传播：

```c
#define FOO
#define BAR FOO

#if BAR == 1
#endif
```

`FOO` 视为已定义且值为 `0`，`BAR -> FOO -> 0`，因此 `BAR` 也按 `0` 参与分支判断。

函数式宏先处理实参，再展开宏体：

```c
#define IS_MODE(x) ((x) == DEFAULT_MODE)

#if IS_MODE(FOO_MODE)
#endif
```

`FOO_MODE` 作为实参先按同一套规则解析；宏体里的 `DEFAULT_MODE` 也按同一套规则解析。两者任何一个缺失，都在对应原始使用位置报错、补 `0` 后继续展开和求值。

函数式宏多参数和变参都按实参逐个处理：

```c
#define IN_RANGE(x, minv, maxv) ((x) >= (minv) && (x) <= (maxv))
#define LOG(fmt, ...) DEBUG(fmt, __VA_ARGS__)

#if IN_RANGE(FOO, 1, BAR)
#endif
```

`FOO`、`BAR` 这类缺失实参各自报错、各自补 `0`，再进入整体展开；变参宏中的每个 `__VA_ARGS__` 实参也按同一规则处理后再组装。

`##` 拼出的新宏名继续解析：

```c
#define CAT(a, b) a##b
#define PICK(name) CAT(COLOR_, name)

#if PICK(CHANGE_MODE) == 1
#endif
```

`PICK(CHANGE_MODE)` 最终形成 `COLOR_CHANGE_MODE` 后，需要继续当普通宏解析。如果 `COLOR_CHANGE_MODE` 缺失，就在原始未展开位置发布缺失 Error 并补 `0` 继续推导。

`#` stringization 和非法拼接都按真实预处理链路处理：

```c
#define TO_STR(x) #x
#define BAD_JOIN(a, b) a##b
```

字符串化结果按真实预处理语义继续流转；`##` / `#` 产出非法 token 或无法继续解析时，统一发布 `展开链异常` Warning，落点在原始未展开位置。

用户配置表达式定位到配置里的缺失叶子：

```text
FOO = BAR + 1
BAZ = BAR + 2
```

如果 `BAR` 没有 unit 来源也没有用户配置来源，正式 Error 挂在用户配置里的 `BAR`，只报一次并给 `BAR=0`；`FOO`、`BAZ` 继续复用这次补值结果。源码侧使用 `FOO` 的位置只保留触发关联信息。

include 中触发的问题挂实际 include 位置：

```c
// unit.nsf
#include "common.hlsl"

// common.hlsl
#if MISSING_MACRO == 1
#endif
```

正式 diagnostics 挂在 `common.hlsl` 的实际使用位置，同时保留触发 unit 和 include 链。若当前值来自缺失后补 `0` 且无真实定义，跳转默认落到首次缺失位置；如果该位置在 include 内，就直接跳到 include 内。

同一 include 多次引入必须按进入上下文重算：

```c
#define MODE 1
#include "common.hlsl"

#undef MODE
#define MODE 2
#include "common.hlsl"
```

两次进入 `common.hlsl` 的宏状态不同，不能复用同一份宏推导结果；只能复用词法 / 解析结果，宏推导快照必须带上下文敏感缓存键。

hover 和跳转展示规则：

```text
active branch: A -> B -> C -> 1
other branch:  A -> B -> MISSING, synthesized 0
```

hover 主展示当前 active branch 的最终值 `A=1`，次级信息展示完整链路、其他分支候选值和补 `0` 情况。若当前 active 值来自用户配置，跳转不跳到配置位置，只在 hover 中显示用户定义来源；若来自 include 内真实 `#define`，跳转直接到 include 内定义。

用户配置被源码覆盖只进入 hover / debug：

```text
user config: FOO = 1
unit chain:
  #if FOO == 1
  #endif
  #define FOO 2
  #if FOO == 2
  #endif
```

前半段先用用户配置，走到源码 `#define FOO 2` 后切换为真实值。该覆盖不报 diagnostics；hover 次级信息保留用户配置曾给值、后续被源码覆盖的时间线。


### 子阶段拆分

P14A：严格顺序宏状态机。

- 覆盖 `#define`、`#undef`、`#ifdef`、`#ifndef`、`defined(...)`、include 顺序、无 replacement 按 `0`、缺失后补 `0`。

P14B：分支敏感推导与汇合。

- 覆盖全分支推导、未激活分支 diagnostics、active branch 收敛、分支汇合 Info、错误分组展示和分支元数据。

P14C：符号链与用户配置增强。

- 覆盖符号链终值解析、缺失叶子定位、用户配置宏、空值按 `0`、用户配置表达式求值。若这部分改变 `nsf.preprocessorMacros` / `nsf.defines` 的公开语义，必须先停下来确认。

P14D：function-like macro 完整展开。

- 覆盖固定参数、嵌套实参、变参、`__VA_ARGS__`、`##`、`#`、递归 / 深度保护和展开链异常。

P14E：编辑态共享集成与 audit 指标。

- 覆盖 hover / 跳转 / diagnostics 共用宏推导核心层、性能指标、健康度 audit、以及 `COLOR_CHANGE_MODE` / `EMISSIVE_MODE` / `FOLIAGE_MODE` 三类主痛点宏的收益验收。

子阶段关闭要求：每个子阶段必须同时给出纯语义最小用例、至少一个真实链路组合样例、debug / audit 可观察证据，以及不会绕过共享宏推导核心层的说明。P14A / P14B 可以先形成内部语义闭环，但不能单独声明 P14 完成。

### 验证计划

验证分两层推进：

- 先补纯宏推导语义用例，按语义点覆盖 `#define`、`#undef`、`defined`、`#ifndef`、include 顺序、缺失补 `0`、分支汇合、符号链、函数式宏、变参、`##`、`#`。
- 再补真实链路组合用例，优先顺序为：`include + #ifndef 默认值 + 后续 #undef/#define 覆盖`，`多分支并行推导 + 分支汇合后按 active branch 收敛`，`function-like macro + 变参 + ##/# + 符号链求值`。
- 首批真实主样本先用 `#ifndef` 默认值链路打底，再用 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 做主验收。
- 编辑态验证必须证明 hover、跳转、diagnostics 基于同一宏推导快照且行为一致。
- 性能验收使用项目现有测试指标，不另行发明临时性能口径。

Audit 需要新增宏推导链路健康度指标：

- 宏来源分布：unit、user、`#ifndef` default、synthesized zero。
- 缺失叶子宏 top list 和补 `0` 次数。
- 展开链异常次数、分支汇合次数、上下文敏感缓存命中率。
- `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 三类主痛点宏的剩余缺失次数、补 `0` 次数、真实来源次数、active branch 命中次数、非 active 分支错误次数，以及 hover / 跳转 / diagnostics 一致性 sample 引用。

audit 指标落地时必须定义 JSON 字段名、计数口径和分母：例如缓存命中率的分母是上下文敏感宏推导请求数，补 `0` 次数按缺失叶子宏事件计数，分支汇合次数按 context 实例内 merge point 计数。

这些 audit 指标属于测试口径变化，落地实现时必须同步 `docs/testing.md`。

### 方案

1. 重置 P14 baseline：后续一律以“新 P14”名义记录，旧 P14 的任何下降数据、临时 preset 试探和阶段结论不得继续引用。
2. 第一批实现必须从共享宏推导核心层开始，不能先做 diagnostics 专用实现再回抽。
3. P14A 到 P14E 可分阶段落地；若用户配置表达式或 function-like macro 完整展开被拆成子阶段，P14 完成前必须补齐。
4. 除本文已通过问答确认的 P14 行为外，任何新增或偏离本文的 `nsf.preprocessorMacros` / `nsf.defines` 语义、用户配置 schema、正式资源或公开 hover / 跳转 / diagnostics 行为变化，都必须先停下来确认。
5. focused 验证从“某宏是否被注入”升级为“宏推导快照是否正确、可追踪、可导航、可复用”。
6. audit 口径同步升级：除 diagnostics 数量外，必须输出宏推导链路健康度指标。
7. 宏推导核心层落地后必须同步 `docs/architecture.md`；audit 指标落地后必须同步 `docs/testing.md`。

### 验收标准

- 本文中旧 P14 相关记录已全部删除，且明确声明作废。
- 新 P14 baseline 已重置，后续记录不再沿用旧阶段编号或旧趋势结论。
- 已完成独立宏推导核心层，hover、跳转、diagnostics 只消费该核心层产出的快照。
- P14A 到 P14E 全部完成。
- `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 三类主痛点宏都完成可复现验收：正确推导、正确 branch、hover / 跳转 / diagnostics 一致、有可观测 diagnostics 收益。
- “可观测 diagnostics 收益”不要求固定百分比，但必须在同口径 5-unit / 50-unit / full 或明确替代样本中证明对应宏族的 `preprocessor-context`、缺失叶子宏数或补 `0` 次数下降；若未下降，必须用最小复现证明剩余问题已超出 P14 边界。
- 对仍 unresolved 或需要移出 P14 的问题，必须有最小复现样例、正确宏推导结果证据、hover / 跳转 / diagnostics 三者一致证据，以及为什么它已超出 P14 边界的结论说明。
- 如果后续需要修改正式资源、workspace 默认配置或公开 hover / 跳转 / diagnostics 行为，仍需单独停下来确认，并同步更新对应事实文档。

### P14A 执行记录

状态：已落地严格顺序宏状态机的 focused 语义闭环；P14A 不能单独声明 P14 完成，P14B-P14E 仍待推进。

实现内容：

- `preprocessor_view.*` 继续作为共享预处理宏推导核心入口，diagnostics、active include context 和 expression typing 仍消费同一份 `PreprocessorView`，未新增 diagnostics-local 特判路径。
- `#if` / `#elif` 数值表达式中的未定义 object-like macro 现在在当前 live state 首次使用处发布 `Undefined macro in preprocessor expression: <macro>.`，随后合成 `0` 并作为已定义宏继续参与后续推导，直到真实 `#define` 或 `#undef` 改变状态。
- 同一表达式里重复出现的同一缺失宏只报一次；同一 live state 后续数值使用复用合成 `0`；`#undef` 后再次数值使用会开启新的缺失周期并重新报错。
- `defined(...)`、`#ifdef`、`#ifndef` 的存在性判断不为未定义宏报错；混合表达式中即使左侧已决定逻辑结果，右侧数值使用仍会被扫描并收集缺失宏。
- `#define FOO` 这种无 replacement macro 现在按已定义且值为 `0` 处理；旧的 object-like 算术 fixture 已改为 `#define ENABLE_FEATURE 1`，保持该测试继续覆盖显式数值 replacement 算术展开。
- `PreprocessorConditionDiagnostic` / `PreprocessorMacroEvent` 补充 `macroName` / `synthesizedZero` 元数据，作为 P14A debug / audit 可观察基础；正式 audit 健康度字段仍属于 P14E 范围。

新增验证：

- 纯语义最小用例：`test_files/module_diagnostics_preprocessor_p14a_state_machine.nsf` 覆盖空 replacement 按 `0`、同表达式去重、逻辑表达式不短路收集、`defined(...)` 不报错、数值使用补 `0` 和 `#undef` 后重新报错。
- 真实链路组合样例：`test_files/module_diagnostics_preprocessor_p14a_include_state.nsf` + `module_diagnostics_preprocessor_p14a_include_state_defs.hlsl` 覆盖 include 顺序、`#ifndef` 默认值和后续 `#undef/#define` 覆盖。

已运行验证：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 首次运行中旧 fixture `module_diagnostics_preprocessor_object_macro_expr.nsf` 因空 replacement 新语义不再进入旧 active 分支而超时；确认是测试预期迁移点后，把该 fixture 改为显式 `#define ENABLE_FEATURE 1`。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 重跑通过，`78 passing / 1 pending`。
- `npm run test:client:repo` 全量运行到 editor runtime defaults 时失败 2 条，失败断言为当前 VS Code 环境中 `editor.quickSuggestions.other` 实际值 `offWhenInlineCompletions` 与测试期望 `on` 不一致；该失败位于 client editor defaults 用例，和本次 C++ preprocessor / diagnostics 改动无关，未修改不相关测试。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14a-state-machine-smoke-5.{json,md}`；`diagnosticsTotal=532`、`preprocessor-context=160`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14a-state-machine-trend-50.{json,md}`；`diagnosticsTotal=4295`、`preprocessor-context=1357`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14A focused fixture；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，`preprocessor_view.*` 的共享宏状态机契约补齐缺失宏补 `0`、空 replacement 和 `#undef` 后缺失周期语义。
- 测试策略是否变化：是，`docs/testing.md` 已补充共享 preprocessor 状态机改动的推荐验证入口。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`preprocessor_view.hpp` 和本执行计划；README、resources、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：是，空 replacement macro 在数值表达式里从旧行为的 truthy 改为已定义且值为 `0`；未定义数值宏首次报错后会在同一 live state 合成 `0`，后续复用不重复报错，`#undef` 后重新报错。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 focused fixture、include 链路组合样例，以及 phase-14a 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过；全 repo 回归存在上述不相关 editor defaults 环境失败。

### P14B 执行记录

状态：已落地分支敏感推导与汇合的 focused 语义闭环；P14B 不能单独声明 P14 完成，P14C-P14E 仍待推进。

实现内容：

- `preprocessor_view.*` 新增 inactive branch probe：主解释链仍按 active branch 写入 `lineActive`、active include closure 和汇合后的宏状态；未激活分支使用隔离宏快照继续解释 `#if/#elif/#else` 子树，只把 preprocessor condition diagnostics 和 branch merge metadata 合并回共享 `PreprocessorView`。
- `PreprocessorConditionDiagnostic` 补充 `inactiveBranch`、`branchId`、`branchIndex` 元数据；`PreprocessorView` 新增 `branchMerges`，记录 merge line、branch id、active branch index 和 branch count，作为 P14B debug / audit 可观察基础。
- active branch 收敛保持不变：分支汇合后主链继续采用 active branch 的宏状态，未激活分支里的 `#define/#undef` 不污染后续 hover / diagnostics / expression typing 的 active 查询。
- include 链路保持实际位置语义：root unit 不把 include 内 condition diagnostics 错挂到 root 文本；打开 include 且存在 active unit context 时，include 自身 diagnostics 消费同一份 included-document `PreprocessorView`。
- 本阶段没有实现 presentation 层的 stable group id / 组内明细 UI，也没有把 branch merge info 发布为用户可见 Information diagnostic；这些属于 P14E 的编辑态共享集成与 audit/展示指标范围。

新增验证：

- 纯语义最小用例：`test_files/module_diagnostics_preprocessor_p14b_branch_merge.nsf` 覆盖 inactive `#else` body 诊断、inactive `#elif` condition 诊断，以及 merge 后继续使用 active branch 的 `P14B_MERGED_VALUE=1`。
- 真实链路组合样例：`test_files/module_diagnostics_preprocessor_p14b_include_branch_root.nsf` + `module_diagnostics_preprocessor_p14b_include_branch_shared.hlsl` 覆盖 active unit include context 下，include 文件内 inactive branch probe 发布 condition diagnostic，同时不改变 active include merge result。

已运行验证：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 首次运行中 root unit 侧 include probe 断言超时；确认根因是 include 内 diagnostics 不应错挂到 root 文本后，将测试改为设置 active unit 并在 included `.hlsl` 上断言。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 重跑通过，`80 passing / 1 pending`。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14b-branch-probe-smoke-5.{json,md}`；`diagnosticsTotal=546`、`preprocessor-context=174`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。相对 P14A smoke，新增的 preprocessor-context 主要来自 inactive branch probe 暴露的未激活分支宏缺口。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14b-branch-probe-trend-50.{json,md}`；`diagnosticsTotal=4396`、`preprocessor-context=1458`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。相对 P14A trend，preprocessor-context 上升符合 P14B “未激活分支 Error 默认进入 diagnostics” 的公开行为变化预期。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14B focused fixture；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，`preprocessor_view.*` 现在统一承载 inactive branch probe、branch merge metadata 和 active branch 汇合后主状态。
- 测试策略是否变化：否，沿用 P14A 已写入 `docs/testing.md` 的共享 preprocessor 状态机验证入口。
- 文档是否已同步：已更新 `docs/architecture.md`、`preprocessor_view.hpp` 和本执行计划；README、resources、testing、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：是，未激活分支中的 preprocessor condition Error 现在会默认进入 diagnostics；汇合后的 semantic / expression diagnostics 仍只跟随 active branch 状态。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 focused fixture、include 链路组合样例，以及 phase-14b 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14C 执行记录

状态：已落地符号链与用户配置增强的 focused 语义闭环；P14C 不能单独声明 P14 完成，P14D-P14E 仍待推进。

实现内容：

- `preprocessor_view.*` 继续作为共享预处理宏推导核心入口；本阶段确认 object-like macro replacement 会在同一 live macro state 中递归求值，符号链断在缺失叶子宏时发布叶子宏 diagnostic，并合成 `0` 供同一表达式 / 同一分支后续复用。
- 用户配置 `nsf.preprocessorMacros` 继续作为 configured macro replacement 初始表进入同一状态机；字符串表达式、空 replacement 和源码 `#define` 覆盖配置值均通过共享 `PreprocessorView` 验证，未新增 diagnostics-local 特判路径。
- `preprocessor_view.hpp` 补齐接口契约说明：object-like 符号链和配置宏表达式通过同一 live state 递归求值，缺失 diagnostic 以叶子宏为根因，合成 `0` 后保持状态一致。
- `docs/architecture.md` 已同步 `preprocessor_view.*` 对符号链 / 用户配置表达式 / 缺失叶子宏补 `0` 的共享职责。
- `docs/testing.md` 已把符号链 / 用户配置表达式求值纳入共享 preprocessor 状态机改动的推荐验证入口。
- 本阶段没有实现 function-like macro 展开、变参、`##`、`#`、hover / 跳转展示或正式 audit 健康度字段；这些仍属于 P14D / P14E。

新增验证：

- 纯语义最小用例：`test_files/module_diagnostics_preprocessor_p14c_symbol_chain.nsf` 覆盖 object-like 符号链终值解析、空 replacement 沿链按 `0` 推导、同一缺失叶子宏只报一次，且不把中间宏误报为缺失根因。
- 用户配置用例：`test_files/module_diagnostics_preprocessor_p14c_configured_macros.nsf` 覆盖 configured macro expression、空配置按 `0`、源码 `#define` 覆盖配置值、配置表达式缺失叶子宏定位。
- 真实链路组合样例：`test_files/module_diagnostics_preprocessor_p14c_include_config_root.nsf` + `module_diagnostics_preprocessor_p14c_include_config_shared.hlsl` 覆盖 active unit include context 下 configured macro chain、include 内源码覆盖配置值，以及 include 文档上的缺失叶子宏 diagnostic。

已运行验证：

- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`83 passing / 1 pending`；新增 P14C 三条 focused / include-context 回归均通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14c-symbol-config-smoke-5.{json,md}`；`diagnosticsTotal=546`、`preprocessor-context=174`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`，与 P14B 同口径持平。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14c-symbol-config-trend-50.{json,md}`；`diagnosticsTotal=4396`、`preprocessor-context=1458`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`，与 P14B 同口径持平。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14C focused fixture 和 include 组合 fixture；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，`preprocessor_view.*` 的共享宏状态机契约补齐 object-like 符号链、用户配置表达式和缺失叶子宏定位语义。
- 测试策略是否变化：是，`docs/testing.md` 已补充共享 preprocessor 状态机验证入口中的符号链 / 用户配置表达式范围；正式 audit 健康度指标仍属 P14E，未在本阶段落地。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`preprocessor_view.hpp` 和本执行计划；README、resources、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：否；本阶段锁定并回归已确认的 P14 用户配置表达式 / 符号链语义，没有扩展 `nsf.preprocessorMacros` / `nsf.defines` schema 或新增公开配置解释方式。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 focused fixture、include 链路组合样例，以及 phase-14c 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；TS compile、C++ build、diagnostics repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14D 执行记录

状态：已落地 function-like macro 在共享 preprocessor expression 中的 focused 语义闭环；P14D 不能单独声明 P14 完成，P14E 的编辑态共享集成与正式 audit 健康度指标仍待推进。

实现内容：

- `preprocessor_view.*` 继续作为共享预处理宏推导核心入口；本阶段将 `#if/#elif` 中的 function-like macro 从旧的 unsupported diagnostic 路径迁移为共享展开路径，diagnostics、active include context 和后续 consumer 仍消费同一份 `PreprocessorView`。
- function-like macro 定义会记录固定参数、变参和 replacement tokens；表达式求值时按 invocation 实参展开固定参数、嵌套 invocation、变参 / `__VA_ARGS__`、`##` token paste 和 `#` stringization。
- `##` 拼出的 token 会继续进入普通宏递归解析；递归 / 深度保护、stringization 非数值使用、token paste 非法 token 或实参数不匹配会发布 preprocessor warning 并按可继续路径使用 `0` 推导，不新增 diagnostics-local suppress 或 fallback。
- `nsf_lexer.*` 补齐 `##` 和 `...` token 识别，供共享 preprocessor function-like macro 参数和 token paste 解析使用。
- `preprocessor_view.hpp` 补齐接口契约说明：function-like macro 展开属于 shared preprocessor expression evaluator 的职责；`lookupActivePreprocessorMacroReplacement(...)` 仍只提供 active replacement metadata，不作为通用 HLSL 宏展开 API。
- `docs/architecture.md` 已同步 `preprocessor_view.*` 对 function-like macro 展开、变参、token paste、stringization warning 和递归 / 深度保护 warning 的共享职责。
- `docs/testing.md` 已把 function-like macro、变参 / `__VA_ARGS__`、`##` / `#` 纳入共享 preprocessor 状态机改动的推荐验证入口。
- 本阶段没有实现 hover / 跳转共用宏推导快照展示、presentation 层分组、性能 / 健康度正式 audit 字段，`COLOR_CHANGE_MODE` / `EMISSIVE_MODE` / `FOLIAGE_MODE` 主痛点宏收益验收仍属于 P14E。

公开行为变化：

- 可展开的 function-like macro 不再发布 `Function-like macro is not supported in preprocessor expression`，而是按共享 preprocessor expression 结果参与 branch 判断和缺失叶子宏定位。
- function-like macro 展开异常现在以 warning 表达，例如 stringization 非数值使用、递归 / 深度保护、非法 token paste 和实参数不匹配；这些 warning 来自共享 preprocessor 状态机，不是 rule-local suppress。

新增验证：

- 纯语义最小用例：`test_files/module_diagnostics_preprocessor_p14d_function_macros.nsf` 覆盖固定参数、嵌套 invocation、变参 / `__VA_ARGS__`、`##` token paste 后继续递归解析、缺失实参宏定位、`#` stringization warning 和递归保护 warning。
- 真实链路组合样例：`test_files/module_diagnostics_preprocessor_p14d_include_root.nsf` + `module_diagnostics_preprocessor_p14d_include_shared.hlsl` 覆盖 active unit include context 下 function-like macro、嵌套 token paste 和 include 文档上的缺失 pasted macro diagnostic。

已运行验证：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`85 passing / 1 pending`；新增 P14D 两条 focused / include-context 回归均通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14d-function-macro-smoke-5.{json,md}`；`diagnosticsTotal=546`、`preprocessor-context=174`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`，与 P14C 同口径持平。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14d-function-macro-trend-50.{json,md}`；`diagnosticsTotal=4396`、`preprocessor-context=1458`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`，与 P14C 同口径持平。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14D focused fixture 和 include 组合 fixture；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，`preprocessor_view.*` 的共享宏状态机契约补齐 function-like macro 展开、变参、token paste、stringization warning 和递归 / 深度保护 warning。
- 测试策略是否变化：是，`docs/testing.md` 已补充共享 preprocessor 状态机验证入口中的 function-like macro / `##` / `#` 范围；正式 audit 健康度指标仍属 P14E，未在本阶段落地。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`preprocessor_view.hpp` 和本执行计划；README、resources、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：是，可展开 function-like macro 不再报 unsupported error；展开异常改由共享 preprocessor 状态机发布 warning。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 focused fixture、include 链路组合样例，以及 phase-14d 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14E 执行记录

状态：已落地编辑态共享集成与 audit 健康度指标；P14 的共享宏推导核心层现在同时被 diagnostics、macro hover 和 macro definition 消费，并具备正式 real audit 可观察字段。三类主痛点宏仍有剩余 undefined 计数，后续应继续从 active unit compile profile、参数 include 或 workspace 配置来源确认值，不在 P14E 中补全局默认。

实现内容：

- `PreprocessorView` 新增 `macroHealth` 统计，字段包括 initial configured macros、numeric defines、source define / undef、`#ifndef` default define、synthesized zero、condition diagnostics、undefined macro diagnostics、macro expansion warning、inactive branch diagnostics、branch merge 和 active include reference 计数。
- diagnostics debug 构建响应 `nsf/_debugBuildDiagnostics` 新增 `macroHealth` JSON；real diagnostics audit 聚合为 `summary.macroHealth`，Markdown 新增 `Macro Health` 表。
- real diagnostics audit 新增 `macroFocus`，固定跟踪 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE` 和 `FOLIAGE_MODE` 的剩余 undefined diagnostics、affected units 和 affected files。
- macro hover / definition 新增共享查询路径 `resolveActivePreprocessorMacroAtLine(...)`，通过 `buildDiagnosticsPreprocessorContext(...)` 构建同一份 active-unit-sensitive `PreprocessorView`，再查询 active replacement / active integer value。
- hover 对 active macro usage 展示 active value 或 active replacement，并标记来源：active unit preprocessor state、active `#ifndef` default define、configured preprocessor macro / active unit profile input 或 synthesized zero。
- definition 对 source / `#ifndef` default macro 跳到真实 `#define`；对 synthesized zero 跳到首次缺失数值使用；对 configured macro / active profile input 返回空结果，不把用户配置当作默认 source location。
- 新增 focused fixture `test_files/module_hover_preprocessor_p14e_shared_state.nsf`，覆盖 source define、`#ifndef` default、synthesized zero 和 configured macro 的 hover / definition 一致性。
- `docs/architecture.md` 已同步 `preprocessor_view.*` 作为 diagnostics expression typing、macro hover 和 macro definition 的共享 consumer-ready 入口，并注明 `macroHealth` 只用于 debug / audit。
- `docs/testing.md` 已同步 `macroHealth` JSON 字段口径、`macroFocus` 口径和共享 preprocessor 状态机推荐验证范围。

公开行为变化：

- macro usage hover 会优先展示共享宏状态机解析出的 active value / replacement 和来源信息。
- macro definition 会优先按共享宏状态跳转；configured macro / active profile input 不再落到 workspace 里同名宏定义，synthesized zero 会跳到首次缺失数值使用位置。

新增验证：

- 纯语义最小用例：沿用 P14A-D diagnostics fixtures 覆盖宏状态机语义；P14E 新增 interactive focused fixture 覆盖 hover / definition 共享消费。
- 真实链路组合样例：phase-14e 5-unit / 50-unit real audit 输出 `macroHealth` 与 `macroFocus`，并继续以 `.nsf` unit include closure 统计真实 workspace。

已运行验证：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`85 passing / 1 pending`；P14A-D diagnostics focused 回归均通过。
- `$env:NSF_TEST_FILE_FILTER='client.interactive-runtime'; npm run test:client:repo` 通过，`54 passing`；新增 P14E macro hover / definition focused 回归通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14e-macro-health-smoke-5.{json,md}`；`diagnosticsTotal=546`、`preprocessor-context=174`、`macroHealth.synthesizedZeroEvents=160`、`macroHealth.branchMergeCount=4332`、`macroHealth.expansionWarningDiagnosticCount=0`、`macroFocus`: `COLOR_CHANGE_MODE=5` / `EMISSIVE_MODE=5` / `FOLIAGE_MODE=4`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14e-macro-health-trend-50.{json,md}`；`diagnosticsTotal=4396`、`preprocessor-context=1458`、`macroHealth.synthesizedZeroEvents=1357`、`macroHealth.branchMergeCount=36095`、`macroHealth.expansionWarningDiagnosticCount=0`、`macroFocus`: `COLOR_CHANGE_MODE=43` / `EMISSIVE_MODE=43` / `FOLIAGE_MODE=11`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- `$env:NSF_TEST_FILE_FILTER='interactive-core'; npm run test:client:repo` 实际加载 `0` 个 test file；该命令不作为有效验证，已改用 `client.interactive-runtime` 过滤覆盖新增用例。
- `npm run test:client:repo` 全量尝试未通过：首次 300s 超时；加长超时后出现 6 个失败。顺序重跑后，`client.references-rename` 的 3 个 conditional branch references / rename 用例仍超时，`client.editing-runtime-layered` 的 global context snapshot id neutral-edit 断言仍失败，`client.editor-runtime-defaults` 的 `editor.autoClosingQuotes.other` 仍为 VS Code 当前返回的 `offWhenInlineCompletions` 而非旧期望 `on`。这些失败不在 P14E 新增 hover / definition / macroHealth 定向验收路径内，已在最终汇报中作为剩余验证风险列出，未在本阶段改业务逻辑迁就。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14E focused fixture 和 phase-14e audit 报告；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，macro hover / definition 与 diagnostics 共同消费 `PreprocessorView` 的 active macro 查询和 integer evaluation。
- 测试策略是否变化：是，real diagnostics audit 新增 `summary.macroHealth` 和 `macroFocus`；`docs/testing.md` 已同步字段口径和推荐验证范围。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`preprocessor_view.hpp` 和本执行计划；README、resources、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：diagnostics 发布策略未变化；hover / definition 公开行为有变化，已在开工前确认。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 P14E focused fixture，以及 phase-14e 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo 定向回归、interactive runtime repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14F 执行记录

状态：已落地 P14 收尾证据增强和 include source location 修正；不新增 `COLOR_CHANGE_MODE` / `EMISSIVE_MODE` / `FOLIAGE_MODE` 全局默认值，不改变 diagnostics 发布策略。

实现内容：

- `PreprocessorMacroReplacement` / `PreprocessorMacroEvent` 补充 source URI、source line 和 source range；宏事件现在同时保留“当前 view 可见位置”和“真实来源位置”。
- include 传播出来的宏状态会携带真实 `#define` / `#ifndef` default / synthesized zero 来源；macro hover / definition 通过同一 `PreprocessorView` 查询后，source define 会跳到 include 内真实定义，synthesized zero 仍跳到首次缺失数值使用。
- real diagnostics audit 的 `macroFocus` 从单纯计数扩展为 owner/source boundary/sample 证据表，固定跟踪 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE` 和 `FOLIAGE_MODE` 剩余 undefined 样例，明确它们仍属于 selector/profile input 边界。
- `macroHealth` 继续把 `synthesizedZeroEvents` 作为真实缺失补 `0` 事件计数；include 可见性传播不会被重复计为新的 synthesized-zero 缺失。
- 新增 focused fixture `test_files/module_diagnostics_preprocessor_p14f_focus_macros.nsf`，用三类 focus selector 宏证明 LSP 不猜全局默认值，同时 active branch 继续按 synthesized `0` 推导。
- 新增 focused fixture `test_files/module_hover_preprocessor_p14f_include_root.nsf` / `test_files/module_hover_preprocessor_p14f_include_defs.hlsl`，覆盖 include source define、include `#ifndef` default 和 root synthesized zero 的 hover / definition 一致性。
- `docs/architecture.md` 已同步 `preprocessor_view.*` 的 active macro source location 契约；`docs/testing.md` 已同步 `macroFocus` owner/source/sample 口径。

公开行为变化：

- 对从 include 传播到当前文件的 active macro，hover / definition 现在使用真实 source location；此前这类事件可能落在父文件 include directive 或缺少可用 source location。
- diagnostics 发布策略未变化；focus selector 宏仍不会被 LSP 猜默认值。

新增验证：

- 纯语义最小用例：P14F focus selector fixture 覆盖 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 缺少 source/config 时保持 undefined diagnostic，并确认稳定 enum-like 常量不误报。
- 编辑态组合样例：P14F include hover fixture 覆盖 include source define、include `#ifndef` default 和 root synthesized zero 的 hover / definition 一致。
- 真实链路组合样例：phase-14f 5-unit / 50-unit real audit 输出 `macroFocus` owner/source/sample 证据，并继续以 `.nsf` unit include closure 统计真实 workspace。

已运行验证：

- `npm run compile` 通过。
- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo` 通过，`86 passing / 1 pending`；新增 P14F focus selector 回归通过。
- `$env:NSF_TEST_FILE_FILTER='client.interactive-runtime'; npm run test:client:repo` 通过，`55 passing`；新增 P14F include macro hover / definition source-location 回归通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14f-focus-source-smoke-5.{json,md}`；`diagnosticsTotal=546`、`preprocessor-context=174`、`macroHealth.synthesizedZeroEvents=160`、`macroHealth.branchMergeCount=4332`、`macroHealth.expansionWarningDiagnosticCount=0`、`macroFocus`: `COLOR_CHANGE_MODE=5` / `EMISSIVE_MODE=5` / `FOLIAGE_MODE=4`，三者 owner 均为 `selector-profile-macro`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14f-focus-source-trend-50.{json,md}`；`diagnosticsTotal=4396`、`preprocessor-context=1458`、`macroHealth.synthesizedZeroEvents=1357`、`macroHealth.branchMergeCount=36095`、`macroHealth.expansionWarningDiagnosticCount=0`、`macroFocus`: `COLOR_CHANGE_MODE=43` / `EMISSIVE_MODE=43` / `FOLIAGE_MODE=11`，三者 owner 均为 `selector-profile-macro`，source boundary 均为 active unit compile profile / parameter include / workspace config，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14F focused fixtures 和 phase-14f audit 报告；无运行时路径、资源路径或命名规则变化。
- 架构或单一事实来源是否变化：是，active macro source location 继续收敛在 `PreprocessorView`，hover / definition 不再自行猜 include source。
- 测试策略是否变化：是，real diagnostics audit 的 `macroFocus` 补充 owner/source boundary/sample；`docs/testing.md` 已同步。
- 文档是否已同步：已更新 `docs/architecture.md`、`docs/testing.md`、`preprocessor_view.hpp`、`server_request_handler_common.hpp` 和本执行计划；README、resources、development 和对象类型 / 方法契约无变化。
- 是否改变公开 diagnostics 行为：否；公开 hover / definition source location 有变化，属于 P14 已确认的共享宏状态机行为。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：已补齐 P14F focused fixtures，以及 phase-14f 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo 定向回归、interactive runtime repo 定向回归、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14G 计划：selector/profile 宏来源闭环

状态：新增执行阶段，目标是治理 P14F 后仍剩余的 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` selector/profile 输入缺口。P14G 不是继续扩展宏推导算法，也不是补全局默认值；它专门回答“这些宏在当前 active unit 下本该从哪里来”。

背景：

- P14F 50-unit audit 中三类 focus 宏仍剩余：`COLOR_CHANGE_MODE=43`、`EMISSIVE_MODE=43`、`FOLIAGE_MODE=11`。
- P14A-F 已证明共享宏状态机、include 顺序、function-like macro、hover / definition / diagnostics 共享消费和 audit 可观察性成立。
- 剩余问题被 `macroFocus` 归为 `selector-profile-macro`，source boundary 是 active unit compile profile、parameter include、workspace `nsf.preprocessorMacros` 或 `nsf.defines`。
- 后续 P15-P18 不会正面治理这类 selector/profile 输入来源，因此必须在 P14 后新增收口阶段，避免问题被带入 parser / scope / call-type 阶段。

目标：

- 对每个 focus macro 的剩余 undefined 样例，输出 per-unit profile 证据：active unit、触发 include / 行、profile source kind/path、shader key、total row count、selected row count、selected row signature、selection hint source、是否已注入、是否 unresolved、是否 profile source 缺失。
- 区分三类结论：
  - LSP provider 缺口：profile source 存在，宏在 source 中可解析，但 provider 未正确注入 / 未正确暴露 unresolved。
  - 外部输入缺口：profile source 缺失、缺少 active unit variant selection、或 shadercompiler/material 没导出 per-unit selector。
  - workspace/source 配置缺口：应由参数 include、`nsf.preprocessorMacros` 或 `nsf.defines` 明确提供。
- 在没有权威值时继续保留 diagnostics；不得为 selector/profile 宏猜默认值。

子阶段：

P14G-A：audit-only profile evidence。

- 只增强 real diagnostics audit / debug 证据，不改变公开 diagnostics、hover、completion、signature help 或 semantic tokens 行为。
- `macroFocus` 追加 per-unit profile evidence，供阶段报告直接判断 focus macro 是 injected、unresolved、source missing、source no macro 还是 runtime debug unavailable。
- 同步 `docs/testing.md` 的 audit 字段口径。

P14G-B：source integration。

- 基于 P14G-A 证据决定是否修 `unit_macro_profile_provider.*`、需要 shadercompiler 导出新 selection source，或需要 workspace/source 配置。
- 只有在 profile source 已有权威值且 provider 漏读 / 漏收敛时，才修改 shared provider。
- 如果需要新增配置语义、资源默认值、正式资源 bundle 或改变 diagnostics 行为，必须先停下来确认。

验收标准：

- P14G-A 报告中每个 focus macro 至少有 top affected unit 的 profile evidence，能明确说明该 unit 是 profile unresolved、profile source missing、profile source no macro、profile injected 但仍诊断，还是 runtime debug 缺失。
- 若发现 provider 缺口，P14G-B 必须新增 focused fixture 证明 provider 修复；若不是 provider 缺口，必须给出证据并把剩余问题移交外部 profile / workspace 配置。
- 不新增 diagnostics-local suppress、fallback、shim、feature flag 或全局默认值。
- 5-unit / 50-unit audit 继续保持 `truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

### P14G-A 执行记录

状态：已落地 audit-only profile evidence；未改变公开 diagnostics 行为。

实现内容：

- real diagnostics audit 的 `macroFocus` 已扩展 `profileStatusSummary` 与 `profileEvidence`，固定对 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 记录 per-unit active-unit compile profile 证据。
- 每条 profile evidence 记录 active unit、status、profile source kind/path、shader key、total row count、selected row count、selected row signature、selection hint source、unresolved macros 和 injected value。
- Markdown 报告新增 `P14 Focus Profile Evidence` 表，便于直接从阶段报告判断 focus macro 是 `profile-injected`、`profile-unresolved`、`profile-source-missing`、`profile-source-empty`、`profile-no-selected-row`、`profile-source-no-macro`、`active-unit-mismatch` 还是 `runtime-debug-missing`。
- `docs/testing.md` 已同步 `macroFocus` profile evidence/status summary 口径。

验证结果：

- `npm run compile` 通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14g-profile-evidence-smoke-5.{json,md}`；`diagnosticsTotal=546`，`macroFocus`: `COLOR_CHANGE_MODE=5` / `EMISSIVE_MODE=5` / `FOLIAGE_MODE=4`，三者 profile status 均为 `profile-source-missing=1`、`profile-source-no-macro=4`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14g-profile-evidence-trend-50.{json,md}`；`diagnosticsTotal=4396`，`macroFocus`: `COLOR_CHANGE_MODE=43` / `EMISSIVE_MODE=43` / `FOLIAGE_MODE=11`，三者 profile status 均为 `profile-source-missing=7`、`profile-source-no-macro=43`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

P14G-A 结论：

- 证据未显示现有 `gimlocalvariants.json` / `used_shader_variants.csv` provider 漏读了 focus macro；相反，已命中的 profile source 对 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 没有可注入的权威值，也没有把它们暴露为 unresolved profile macro。
- 补充源码审计显示，`FOLIAGE_MODE` 的 source default 位于 `shaderlib/foliage_anim_functions.hlsl`，但典型 affected unit 先 include `shaderlib/season_uniforms.hlsl` 后 include `foliage_anim_functions.hlsl`，因此按严格预处理顺序在 `season_uniforms.hlsl` 使用处仍为 undefined。
- `COLOR_CHANGE_MODE` / `EMISSIVE_MODE` 的 defaults 主要位于 pbr parameter include；典型 grass unit 没有在 `common_pbr.hlsl` 使用 `surface_functions.hlsl` / `surface_emissive.hlsl` 前引入这些 parameter include，`shaderlib/surface_functions.hlsl` 与 `shaderlib/surface_emissive.hlsl` 中对应默认定义当前还是注释状态。
- 补充检查 `shadercompiler/check/shader_macro_combinations`：50-unit 样本中 `COLOR_CHANGE_MODE` 与 `EMISSIVE_MODE` 均为 missing；`FOLIAGE_MODE` 仅 2 个 unit 是 stable `0`，2 个 unit 冲突，46 个 unit missing。因此把 `shader_macro_combinations` 作为新 profile source 也只能局部覆盖 `FOLIAGE_MODE`，不能解决三类 focus macro 的主体缺口。

P14G-B 判断：

- 当前不应在 LSP 内为这些 selector/profile 宏补全局默认值，也不应通过 diagnostics suppress 掩盖 undefined；这会重新引入 P14 明确禁止的猜测性默认。
- 若要继续治理，需要先确认一个公开行为变化方向：要么由真实源码调整 include/default 顺序，要么由 shadercompiler/material 导出 per-unit selector 值，要么把某个新 profile source（例如 `shader_macro_combinations`）正式升格进 `unit_macro_profile_provider.*`。第三种仍只能部分解决 `FOLIAGE_MODE`，且需要新增 provider 来源、focused fixture、C++ 构建、repo m4 回归和 5/50 audit 复验。

### P14H 补充计划：`#art` 美术宏默认 0 契约

状态：已确认新增 P14 补充阶段。P14G-A 把问题定位到“profile 未提供权威值”，但人工复核确认 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 等大量剩余宏属于 Neox `#art` 美术宏：实际材质实例未启用或未覆盖时应按默认 `0` 参与 shader 预处理，不应作为普通 undefined macro 报错。

根因修正：

- P14G-A 的证据结论“现有 active-unit profile source 没有值”仍成立，但这不等同于“应继续报 undefined”。
- 对 `#art NAME "..." "BOOL"` / `#art NAME "..." "INT"` 声明过的美术宏，权威默认来源不是 `gimlocalvariants.json` / `used_shader_variants.csv`，而是 shader 源码里的 Neox art directive 语义。
- 当前 LSP 只把 `#art` 当作可 hover 的项目指令，没有把它升格为预处理宏默认来源，因此这类宏先报 undefined，再 synthesized `0` 继续推导；这会造成误报。

目标：

- 建立共享 `#art` 美术宏默认值入口：workspace / include roots 中出现过 `#art NAME "..." "BOOL"` 或 `#art NAME "..." "INT"` 的宏，在 active unit profile、workspace config、`nsf.defines` 和源码 `#define/#undef` 都未给值时，按默认 `0` 注入预处理环境。
- 该默认来源必须进入 shared preprocessor input，而不是 diagnostics-local suppress；hover、definition、diagnostics 和 audit 应看到同一来源。
- profile、workspace `nsf.preprocessorMacros`、workspace `nsf.defines` 和源码 `#define/#undef` 继续按既有优先级覆盖 `#art` default zero。
- debug / audit 需要能区分该来源是 `art-default-zero`，不能把它伪装成 active unit profile injected。

实施边界：

- 只对 `#art` 类型为 `BOOL` 或 `INT` 的宏注入默认 `0`；其它类型先只记录为 directive metadata，不进入 preprocessor default。
- 不从注释中的 `#art` 或注释中的 `#ifndef/#define` 推断默认值。
- 不为未出现 `#art` 声明的 selector/profile 宏新增默认值。
- 不改变 `nsf.preprocessorMacros` schema，不新增资源 bundle，不引入 feature flag、fallback、shim 或 diagnostics suppress。

验收标准：

- focused fixture 证明 `#art BOOL/INT` 宏在未配置、未 profile、未源码 define 时不再发布 `Undefined macro in preprocessor expression`，并按 `0` 走 inactive branch。
- focused fixture 证明 workspace/profile/config/source 显式值仍覆盖 `#art` default zero。
- 5-unit / 50-unit audit 中 `COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 的剩余 undefined diagnostics 应显著下降；若仍有残留，必须通过 `macroFocus` / 新增 evidence 区分“未发现 #art 声明”与“其它真实缺口”。
- 继续保持 `truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。

验证计划：

- `cmake --build .\server_cpp\build`
- `npm run compile`
- `$env:NSF_TEST_FILE_FILTER='diagnostics'; npm run test:client:repo`
- 如 hover / definition 来源展示变化，补跑 interactive hover / definition repo 定向回归。
- phase-14h 5-unit smoke audit 和 50-unit trend audit。

### P14H 执行记录

状态：已落地 `#art` BOOL/INT default-zero 共享预处理输入；改变公开 preprocessor diagnostics 行为。

实现内容：

- 新增 `art_macro_defaults.hpp` 作为 `#art` default-zero source metadata 结构，记录宏名、类型、source uri、line/range。
- `workspace_index.*` / `workspace_summary_runtime.*` 现在索引 active、非注释的 `#art NAME "..." "BOOL"` / `"INT"` 声明，并以 workspace summary 查询接口暴露；非 BOOL/INT、注释中的 `#art` 不进入 default-zero 输入。
- `preprocessor_view.*` 初始宏播种顺序调整为 `#art` default zero < `nsf.preprocessorMacros` < active unit profile / `nsf.defines`，源码 `#define/#undef` 继续按顺序覆盖；macro replacement/source metadata 增加 `sourceArtDefaultZero`。
- `global_context_runtime.*` 将 workspace `#art` default-zero macros 纳入 active unit snapshot/debug；macro hover/definition 会把 default-zero 来源标为 `#art BOOL/INT default zero` 并可跳转到 `#art` 声明。
- diagnostics audit 的 `macroHealth` 增加 `initialArtDefaultZeroMacroCount`，`macroFocus` evidence/status summary 可显示 `art-default-zero` 及 source path/line。
- 新增 focused fixtures `test_files/p14h_art_defaults/*`，覆盖 workspace 级 `#art` default zero、非 BOOL/INT 不默认、注释中 `#art` 不默认、source `#define` 覆盖和 `nsf.preprocessorMacros` 覆盖。
- 已同步 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md` 和相关头文件契约。

公开行为变化：

- workspace / include roots 中出现过 `#art NAME "..." "BOOL"` 或 `"INT"` 的宏，在 profile、`nsf.preprocessorMacros`、`nsf.defines` 和源码都没有给值时，不再发布 `Undefined macro in preprocessor expression: NAME.`，而是按默认 `0` 参与 `#if/#elif` 求值。
- `nsf.preprocessorMacros`、active unit profile、`nsf.defines`、源码 `#define/#undef` 对同名宏仍保持高优先级覆盖；source `#undef` 后的后续 undefined 使用仍按真实 undefined 处理。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='client.diagnostics'; npm run test:client:repo` 通过，88 passing；覆盖 P14H default-zero / source override / config override focused fixtures。
- `$env:NSF_TEST_FILE_FILTER='interactive-runtime'; npm run test:client:repo` 通过，55 passing；复核宏 hover / definition 共享 preprocessor state 未回归。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14h-art-default-smoke-5.{json,md}`；`diagnosticsTotal=508`，`macroFocus`: `COLOR_CHANGE_MODE=0` / `EMISSIVE_MODE=0` / `FOLIAGE_MODE=0`，三者 status 均为 `art-default-zero=5`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14h-art-default-trend-50.{json,md}`；`diagnosticsTotal=4089`，`macroFocus`: `COLOR_CHANGE_MODE=0` / `EMISSIVE_MODE=0` / `FOLIAGE_MODE=0`，三者 status 均为 `art-default-zero=50`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。相对 P14G-A 50-unit evidence run 的 focus macro `43/43/11` 已全部归零。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 focused fixture 与阶段 audit 报告；workspace index cache key bump 用于刷新新字段，无用户可见运行时路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，Neox `#art` BOOL/INT default-zero 现在由 workspace summary 作为单一事实来源进入 shared preprocessor input。
- 测试策略是否变化：是，新增 P14H focused fixture，并在 real diagnostics audit 中增加 `art-default-zero` evidence 与 `initialArtDefaultZeroMacroCount`。
- 文档是否已同步：已更新 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`、本执行计划及相关头文件；`AGENTS.md`、`docs/client-editor-features.md`、`docs/type-method-interface-contract.md`、`docs/development.md` 无需更新。
- 是否改变公开 diagnostics 行为：是，`#art` BOOL/INT 宏默认 `0` 后不再作为普通 undefined macro 报错。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：是，新增 repo focused fixtures，并生成 phase-14h 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo、interactive-runtime repo、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14I 计划：shadercompiler private numeric 常量语义对齐

状态：已确认新增 P14 后续阶段。P14H 后 `COLOR_CHANGE_MODE` / `EMISSIVE_MODE` / `FOLIAGE_MODE` selector/profile 类美术宏已按 `#art` 默认 `0` 收敛，但 50-unit audit 仍剩余一批 enum-like 右值常量；人工复核和 shadercompiler 对照显示它们不是同一种缺口。

根因判断：

- `FOLIAGE_TREE_BRANCH` / `FOLIAGE_TREE_LEAF` / `FOLIAGE_GRASS_BRANCH` / `FOLIAGE_GRASS_LEAF` 在 affected grass/base 单元中属于“先使用、后定义”的 active include closure 内 private numeric `#define`。真实 shadercompiler 会先全文收集 private macros，再全局替换 `#if/#elif` 表达式，因此不报 unresolved；当前 LSP 仍按严格预处理顺序解释，导致误报。
- `COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*` 在 affected base 单元中不是简单 include 顺序迟到；它们主要定义在 `pbr/nodes/*_parameters.hlsl`，当前 active include closure 没有引入对应 parameter include，且不同 parameter 文件中的值存在冲突。直接补全局默认值会误激活分支。
- shadercompiler 对 `base/animated_grass_noseason.nsf` 的宏收集验证显示：`FOLIAGE_*` 已进入 `private_macros`，而 `COLOR_CHANGE_*` / `EMISSIVE_*` 仍在 `unresolved_macros`；因此 P14I 只治理前者这一类 compiler-order-insensitive private constants。

目标：

- 在 shared `PreprocessorView` 层对齐 shadercompiler 的一条关键规则：当前 active unit include closure 内稳定、无冲突、无 `#undef` 干扰的 object-like numeric private `#define`，可作为最低优先级初始宏输入，使 `#if/#elif` 中“先用后定义”的同名常量不再报 undefined。
- 该来源必须是 active unit closure 局部来源，不能扫描全 workspace，不能从未参与当前 unit 的 parameter include 中捞值。
- 该来源必须进入共享 preprocessor state，而不是 diagnostics-local suppress；hover、definition、diagnostics、audit 看到同一来源。

实施边界：

- 只收集 object-like、单 token 整数 numeric `#define NAME value`，且同名定义值一致、当前 closure 内无 `#undef NAME` 时才注入。
- function-like 宏、表达式宏、字符串宏、条件复杂宏、同名冲突值、出现 `#undef` 的宏均不注入，继续按真实源码/配置状态诊断。
- 不为 `COLOR_CHANGE_*` / `EMISSIVE_*` 建全局 enum catalog；它们后续需要单独确认源码 include 调整、shadercompiler builtin/catalog 或正式导出的权威 enum source。
- 不新增资源 bundle、feature flag、compat/shim 或 diagnostics suppress。

验收标准：

- focused fixture 覆盖“include 后部定义 numeric private constant、前部 `#if` 使用”的场景，不再发布 undefined macro diagnostic。
- focused fixture 覆盖同名冲突值不注入、`#undef` 干扰不注入、非 active include 不注入、function-like/表达式宏不注入。
- 50-unit audit 预期 active closure 内已定义的 `FOLIAGE_*` 先用后定义误报下降；若仍有残留，必须区分“当前 unit closure 未包含定义来源”与“include 顺序迟到”。`COLOR_CHANGE_*` / `EMISSIVE_*` 保留，并在报告中继续归为需要权威来源确认的 enum-like stable constant 候选。
- C++ build、TS compile、diagnostics focused repo、interactive hover/definition 回归和 phase-14i 5/50 real audit 通过。

执行结果：

- `PreprocessorView` 新增 active unit include-closure compiler private numeric constants 收集：先用严格预处理视图枚举 active include closure，再从 root/include root lines 收集 object-like 单 token 整数 `#define NAME value`；同名值冲突、function-like、表达式 replacement、非整数均不注入。P14O 进一步确认 `#undef NAME` 应清空当前候选而不是永久阻断，后续稳定 `#define NAME value` 可以重新建立候选；最终没有后续稳定定义的仍不注入。
- 该来源作为 `#art` default zero 之后、`nsf.preprocessorMacros` / active unit profile / `nsf.defines` 之前的初始宏输入，源码 `#define/#undef` 仍按顺序覆盖；macro source metadata 标记为 `sourceCompilerPrivateConstant` / `compilerPrivateConstant`，hover 显示 `active unit compiler private numeric constant`。
- audit/debug `macroHealth` 增加 `initialCompilerPrivateConstantCount`；real diagnostics audit 在同一 active unit 的多文件扫描中通过显式 `compilerPrivateConstantCacheScope` 复用 compiler-private 常量收集结果，避免每个 closure 文件重复全量扫描。普通 LSP publish/interactive 路径不启用该 audit cache scope。
- 新增 focused fixture `test_files/p14i_compiler_private_constants/*`，覆盖 include 后部 numeric private constant 的前序使用、冲突值、`#undef` 干扰、表达式/function-like 宏和非 active include 不注入。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `$env:NSF_TEST_FILE_FILTER='client.diagnostics'; npm run test:client:repo` 通过，89 passing。
- `$env:NSF_TEST_FILE_FILTER='interactive-runtime'; npm run test:client:repo` 通过，55 passing。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14i-compiler-private-smoke-5.{json,md}`；`diagnosticsTotal=487`、`preprocessor-context=115`、`macroHealth.initialCompilerPrivateConstantCount=50965`、`macroFocus`: `COLOR_CHANGE_MODE=0` / `EMISSIVE_MODE=0` / `FOLIAGE_MODE=0`，三者 status 均为 `art-default-zero=5`，`FOLIAGE_*` undefined 样本为 0，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 通过，输出 `real-workspace-diagnostics-audit.phase-14i-compiler-private-trend-50.{json,md}`；相对 P14H trend：`diagnosticsTotal 4089 -> 4029`、`preprocessor-context 1151 -> 1091`、`synthesizedZeroEvents 1088 -> 1032`、`undefinedMacroDiagnosticCount 1151 -> 1091`、`macroHealth.initialCompilerPrivateConstantCount=433218`，`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit 中 `FOLIAGE_GRASS_BRANCH` / `FOLIAGE_GRASS_LEAF` / `FOLIAGE_TREE_BRANCH` / `FOLIAGE_TREE_LEAF` 各从 11 条降到 7 条；剩余样本集中在 `base/blast*.nsf`、`base/external_lightmap*.nsf`、`base/decal_bluetide_plane.nsf`、`base/dm51_bg_allround.nsf` 等只 include `shaderlib/season_uniforms.hlsl`、未 include `shaderlib/foliage_anim_functions.hlsl` 的 unit。该残留不是 include 顺序迟到，而是 active closure 内没有定义来源；P14I 按边界不能从全 workspace 或未参与的 parameter/include 文件注入。
- `COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*` 仍保留为 enum-like stable constant 候选，典型剩余为每个宏 43 条；它们的定义主要在未进入 affected base unit closure 的 parameter include 中，且存在跨 parameter 文件值域冲突，不能由 P14I 建全局默认或全局 enum catalog。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增 P14I focused fixture 与 phase-14i audit 报告；无用户可见资源路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是，active unit compiler private numeric constants 成为 `preprocessor_view.*` 的共享初始宏输入来源，只限当前 active include closure。
- 测试策略是否变化：是，real diagnostics audit macroHealth 增加 `initialCompilerPrivateConstantCount`，并在 audit debug 请求中启用 per-unit compiler-private 收集 cache scope。
- 文档是否已同步：已更新 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`、本执行计划及相关头文件；`AGENTS.md`、`docs/client-editor-features.md`、`docs/type-method-interface-contract.md`、`docs/development.md` 无需更新。
- 是否改变公开 diagnostics / hover 行为：是，当前 active closure 内安全的先用后定义 compiler private numeric constants 不再作为 undefined macro 报错，hover/definition 可显示其真实来源。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否补齐 focused fixture 或稳定 real audit sample：是，新增 repo focused fixture，并生成 phase-14i 5-unit / 50-unit real audit sample。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、diagnostics repo、interactive-runtime repo、5-unit smoke audit 和 50-unit trend audit 均通过。

### P14J 计划：用户指定 shadercompiler 权威输入根

状态：已确认新增 P14 收口阶段。P14I 后剩余问题不应继续由插件猜宏值解决；后续需要让 `nsf_lsp` 消费真实 shadercompiler / material pipeline 的权威导出数据。

根因判断：

- 当前 `unit_macro_profile_provider.*` 只从 workspace/include roots 自动发现 `gimlocalvariants.json`、`used_shader_variants.csv` 和 `active_unit_variant_selection.csv`。当真实 shadercompiler 位于 workspace 外，或项目没有把 compiler 导出目录放进 `nsf.intellisionPath` 时，LSP 无法定位权威 profile source。
- 由 client 调用 shadercompiler 会把语言真相放到 client 侧，违反当前架构；但由 server 在普通 hover/diagnostics 热路径直接执行完整编译，又会引入性能、副作用和环境依赖风险。

目标：

- 新增 `nsf.shaderCompilerPath` 用户配置，由 client 只负责同步配置，server / `nsf_lsp` 负责消费。
- P14J 先落地只读 provider 边界：把 `shaderCompilerPath` 作为 `unit_macro_profile_provider.*` 的额外发现根，读取 compiler 已导出的 profile JSON/CSV，不 spawn 外部编译进程。
- `shaderCompilerPath` 必须参与 active-unit analysis context fingerprint；配置变化后应刷新 active unit profile、diagnostics 和 debug metadata。

实施边界：

- 接受 shadercompiler 根目录或可执行文件路径；如果是可执行入口，provider 可从其父目录尝试发现导出数据。
- 当前只读取已支持的 `gimlocalvariants.json`、`used_shader_variants.csv`、`active_unit_variant_selection.csv`，并额外覆盖 `check/check_used_shader_variants/trunk/` 这类 shadercompiler 根目录布局。
- 不在 P14J 中执行完整 shadercompiler，不新增外部进程调用、不新增 fallback guessed defaults、不把 `COLOR_CHANGE_*` / `EMISSIVE_*` 提升为全局 enum catalog。

验收标准：

- package / client config 支持 `nsf.shaderCompilerPath`，配置变化无需重启 server 即可刷新 runtime config。
- focused repo test 证明：当 `nsf.intellisionPath` 不包含 profile 文件，但 `nsf.shaderCompilerPath` 指向 compiler 导出根时，active unit profile 仍能注入稳定宏，debug source path 指向 shaderCompilerPath 下的文件。
- C++ build、TS compile、`analysis-context-unit-profile` 定向回归通过。

执行结果：

- 新增用户配置 `nsf.shaderCompilerPath`，支持 shadercompiler 根目录或可执行文件路径。client 只做配置读取、规范化和同步；server 侧保存到 request/global context。
- `unit_macro_profile_provider.*` 新增 `shaderCompilerPath` 发现根；除既有 workspace/include roots 外，还会在该路径及其父目录下查找已支持的 `gimlocalvariants.json`、`used_shader_variants.csv`、`active_unit_variant_selection.csv`，并新增 `check/check_used_shader_variants/trunk/` 布局候选，用于直接指向 shadercompiler 根目录的场景。
- `GlobalContextRuntimeOptions` / `ActiveUnitSnapshot` 增加 `shaderCompilerPath` 与 fingerprint，确保配置变化会刷新 active-unit analysis context，不复用旧 profile source。
- 新增 focused fixture `test_files/shadercompiler_path_fixture/check/check_used_shader_variants/trunk/gimlocalvariants.json`；新增 `analysis-context-unit-profile` 用例，证明 `nsf.intellisionPath` 不包含 profile 文件时，`nsf.shaderCompilerPath` 仍能让 active unit 注入 `TARGET_VARIANT_PROFILE=1`，且 debug source path 指向 shadercompiler path fixture。
- P14J 不执行外部 shadercompiler 进程；当前仍只读消费已导出的 profile source，不改变 `COLOR_CHANGE_*` / `EMISSIVE_*` 的权威来源结论。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `npm run compile` 通过。
- `node .\out\test\runCodeTests.js --mode repo --file-filter analysis-context-unit-profile` 通过，6 passing。
- `npm run test:client:repo:m4` 通过。
- `$env:NSF_TEST_FILE_FILTER='client.diagnostics'; npm run test:client:repo` 通过，89 passing。

阶段关闭判断：

- 命令是否变化：否。
- 路径或命名是否变化：新增用户配置 `nsf.shaderCompilerPath`；新增 focused fixture 路径 `test_files/shadercompiler_path_fixture/`。
- 架构或单一事实来源是否变化：是，active unit profile source 发现根从 workspace/include roots 扩展到可选 `nsf.shaderCompilerPath`，仍由 server 侧 `unit_macro_profile_provider.*` 作为单一入口消费。
- 测试策略是否变化：是，`analysis-context-unit-profile` 增加 shaderCompilerPath provider root 覆盖；`docs/testing.md` 已同步。
- 文档是否已同步：已更新 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`、本执行计划和相关头文件；`AGENTS.md`、`docs/client-editor-features.md`、`docs/type-method-interface-contract.md`、`docs/development.md` 无需更新。
- 是否改变公开行为：是，用户设置 `nsf.shaderCompilerPath` 后，LSP 可从该路径读取 compiler 导出的 per-unit profile 宏，影响 active branch / diagnostics / hover 等共享分析结果；未设置时保持原行为。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否执行 shadercompiler 外部进程：否。
- 是否重新跑了对应验证并记录结果：是；C++ build、TS compile、unit-profile、M4 和 diagnostics repo 定向均通过。

### P14K-A 计划：shadercompiler 临时输出调用探针

状态：已确认新增 P14 补充探针阶段。P14J 已允许用户指定 `nsf.shaderCompilerPath` 作为只读 profile source root，但它仍依赖 compiler / material pipeline 已经生成并保持新鲜的导出数据。用户编辑 `.nsf/.hlsl` 后，`K:\future\res\shader` 这类生成目录不一定同步刷新，因此不能直接把既有生成文件当作永远新鲜的 LSP 真相。

根因判断：

- 剩余 enum-like / profile 宏问题需要更接近真实 shadercompiler 的 active unit 上下文；仅从源码 include closure 或静态 profile JSON/CSV 继续猜值，会把 compiler 规则重新实现到 LSP 里。
- 但 shadercompiler 完整编译有 Python 2、`.pyd`、工作目录、输出目录、shadermap 和临时文件等环境/副作用要求；在 hover、completion、diagnostics 热路径直接执行会破坏当前 server 调度契约。
- 因此 P14K-A 只验证“是否能由 `nsf_lsp` 背景任务安全调用 shadercompiler 生成临时快照”，不做运行时接入。

目标：

- 找到可由 server 后台任务复用的 shadercompiler 单文件入口、参数、工作目录和最小环境要求。
- 验证输出是否能定向到 `%TEMP%` 下的临时目录，而不是写回源码目录或 `K:\future\res\shader`。
- 记录不可避免的副作用文件、耗时、失败条件和可用于 P14K-B 的输出内容。

实施边界：

- 只做人工/本机 feasibility probe，不修改 server 运行时代码，不新增公开配置，不改变 diagnostics / hover / completion 行为。
- 探针必须使用临时输出目录；不得写入真实 res shader 目录，不覆盖源码，不依赖 `K:\future\res\shader` 作为新鲜真相。
- 若后续进入 P14K-B，compiler invocation 只能在 background lane / worker 中 debounce + latest-only 执行；结果必须绑定 active unit、document version / fingerprint 和 source snapshot，过期即丢弃。
- compiler 输出只能作为 active branch / profile / enum-like 宏权威输入，不替换 source AST、definition 位置或源码 include graph。

验收标准：

- 明确记录可执行命令、输入 unit、临时输出目录、生成文件列表、宏证据和副作用范围。
- 明确说明 `fx_process.py` / `compile_all_git.py` 哪个入口适合未来 server 调用，以及当前不能直接接入的原因。
- 形成 P14K-B 设计边界：后台调度、超时/取消、临时目录生命周期、fingerprint stale 丢弃、debug metadata 和失败降级策略。

### P14K-A 执行记录

状态：已完成本机 feasibility probe；未修改 server/client 运行时代码，未改变公开行为。

探针环境：

- shadercompiler root：`C:\Software\WorkTemp\G66ShaderDevelop\shadercompiler`
- Python：`C:\Python27\python.exe`，版本 `Python 2.7.17`
- 单文件入口：`fx_process.py`
- 探针策略：用临时 cwd 启动脚本，并把 `-o` 指向 `%TEMP%` 下的临时 `res\shader`，避免调试文件、缓存文件和生成 shader 写入真实目录。

可执行命令形态：

```powershell
$compiler = "C:\Software\WorkTemp\G66ShaderDevelop\shadercompiler"
$cwd = "$env:TEMP\nsf-lsp-shaderprobe-<stamp>\cwd"
$out = "$env:TEMP\nsf-lsp-shaderprobe-<stamp>\res\shader"
New-Item -ItemType Directory -Force $cwd, $out | Out-Null
Push-Location $cwd
& C:\Python27\python.exe -b "$compiler\fx_process.py" `
  -m 1 `
  -i "C:\Software\WorkTemp\G66ShaderDevelop\shader-source\base\animated_grass_noseason.nsf" `
  -o $out `
  --set-is-future 1 `
  --is-release 0 `
  --is-shipping 0
Pop-Location
```

成功样本：

- `base\animated_grass_noseason.nsf` 成功，耗时约 `38.6s`，临时根：`%TEMP%\nsf-lsp-shaderprobe-20260527-233852-animated-all`。
- `pbr\pbr_weapon.nsf` 成功，耗时约 `83.6s`，临时根：`%TEMP%\nsf-lsp-shaderprobe-20260527-234050-pbr-weapon`。
- 两次成功探针均未发现 shadercompiler root 下顶层文件发生变化；生成文件都落在临时根下。

失败样本：

- `base\animated_grass_noseason.nsf` 携带 `--not-all` 时失败，耗时约 `25.7s`，失败点为 `hlsl_process.py` 中 `compiled[lang][1] = fix_max_rt_count_gl(compiled[lang][1])` 的 `IndexError: list index out of range`。
- 结论：未来 server 侧不能简单用 `--not-all` 作为低成本模式；至少对该类 unit，需要完整平台输出或更明确的 compiler-supported 快照模式。

输出与副作用范围：

- 临时输出包含 forward / deferred 的 `.fx`、`.nfx2`、`.ps`、`.vs`，例如：
  - `res\shader\animated_grass_noseason.fx`
  - `res\shader\animated_grass_noseason.nfx2`
  - `res\shader\deferred\animated_grass_noseason.nfx2`
  - `res\common\pipeline\deferred_shadermap.xml`
- 临时 cwd 会写入调试 / 缓存文件：
  - `cached_boolean_expr.py`
  - `tmp_code_origin.hlsl`
  - `tmp_code_dx9.hlsl` / `tmp_code_dx11.hlsl` / `tmp_code_gles30.hlsl` / `tmp_code_metal.hlsl` / `tmp_code_vulkan.hlsl`
  - `tmp_compiled_*_vs.hlsl` / `tmp_compiled_*_ps.hlsl`
- 因此未来若由 server 调用，必须使用 per-job 临时 cwd；不能在 shadercompiler root 下直接启动，否则会污染工具目录。

宏证据：

- `animated_grass_noseason.fx` 中存在 compiler 展开后的 `#define SHADINGMODELID_*`、`#define DYNAMIC_GI_TYPE DYNAMIC_GI_DEFAULT`、`#define FOLIAGE_NONE 0`、`#ifndef FOLIAGE_MODE` / `#define FOLIAGE_MODE FOLIAGE_NONE`。
- `pbr_weapon.fx` 中存在 `NEOX_SASEFFECT_MACRO(..., "COLOR_CHANGE_MODE", "INT", "COLOR_CHANGE_NONE")`、`NEOX_SASEFFECT_MACRO(..., "EMISSIVE_MODE", "INT", "EMISSIVE_NONE")`，以及 `#define COLOR_CHANGE_NONE 0`、`#define COLOR_CHANGE_PICKER 0`、`#define CHANNEL_COLOR_CHANGE 1`、`#define CHANNEL_COLOR_CHANGE_GRADIENT 2`、`#define CHANNEL_COLOR_CHANGE_ID 3`、`#define EMISSIVE_NONE 0`、`#define EMISSIVE_COLOR 1`、`#define EMISSIVE_FLOW 2`、`#define EMISSIVE_FLOW_UV1 3`、`#ifndef COLOR_CHANGE_MODE` / `#define COLOR_CHANGE_MODE COLOR_CHANGE_NONE`、`#ifndef EMISSIVE_MODE` / `#define EMISSIVE_MODE EMISSIVE_NONE`。
- 这证明 generated `.fx` 可以作为当前 unit 的 compiler snapshot 来回答 enum-like 常量和美术宏默认值；但它仍是生成结果，必须绑定源码版本/fingerprint，不能把 `K:\future\res\shader` 的已有文件当作总是新鲜。

入口判断：

- `compile_all_git.py` 负责全量/批量 orchestration，依赖 `SHADER_SOURCE_GIT`、`FUTURE_RES_PATH`、`TRUNK_RES_PATH`、`WRITE_TO_ROOT_RES`、`COMPILE_MODE` 等环境，并会调用 `fx_process.py --compile-all`；它不适合作为 LSP 编辑态单文件探针入口。
- `fx_process.py -i <unit> -o <temp-res-shader> -m 1 --set-is-future 1 --is-release 0 --is-shipping 0` 是当前可行的单文件临时输出入口，但成本较高，且不是可取消的 in-process API。

P14K-B 边界：

- 若进入实现，server 只能在后台 worker 中调用 compiler，使用 debounce + latest-only；hover、completion、signature help、diagnostics 热路径不得同步等待 compiler。
- 每次 compiler job 必须使用独立临时 cwd 和临时输出根；任务结束后可按 debug 配置保留或清理，但默认不写真实 res。
- job key 必须包含 active unit path、document version / content fingerprint、include closure fingerprint、`nsf.shaderCompilerPath`、compile mode 参数和 relevant config fingerprint。
- 结果只允许作为 active branch / profile / enum-like macro authority overlay 输入；不得替换 source AST、源码 definition location、include graph 或 workspace index。
- compiler job 超时、失败或结果 stale 时，应记录 debug metadata 并丢弃该 job 结果；不得回退到 guessed defaults 或把过期 snapshot 继续注入当前分析。
- 当前阶段没有新增公开行为；P14K-B 一旦要让 compiler snapshot 影响 diagnostics / hover，必须按公开行为变化再次确认并同步事实文档、头文件契约和测试。

阶段关闭判断：

- 命令是否变化：否；仅记录本机探针命令。
- 路径或命名是否变化：否；仅使用 `%TEMP%` 临时目录。
- 架构或单一事实来源是否变化：否；P14K-A 未接入运行时代码。
- 测试策略是否变化：否；未新增测试入口或 fixture。
- 文档是否已同步：已更新本执行计划；当前事实文档无需更新，因为没有运行时行为、命令、资源路径或架构契约变化。
- 是否改变公开行为：否。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否执行 shadercompiler 外部进程：是，仅限本机 feasibility probe，输出写入临时目录。
- 是否重新跑了对应验证并记录结果：是；两条成功单文件临时输出探针和一条 `--not-all` 失败探针已记录。

### P14K-B 计划：C++ compiler macro snapshot server 集成

状态：已按“直接做 C++ 版本”完成方向调整并落地。P14K-B 不再保留 Python helper，也不修改 shadercompiler；compiler snapshot 由 `nsf_lsp` server 内的 C++ provider 从当前 active unit include closure 中提取源码可重建的稳定宏事实。

根因判断：

- P14I 后剩余一类 enum-like / default alias 问题不是 include 顺序本身，而是 shadercompiler 会先收集当前 unit 的宏快照，再用这些 alias 参与前置 `#if/#elif` 求值。
- P14K-A 证明调用完整 shadercompiler 或 Python helper 成本高、协作复杂，且编辑态 server 不应依赖生成目录是否新鲜。
- 因此最终方案不调用外部 compiler，不 import Python 依赖，而是在 C++ server 内实现一个保守子集：只收集 active closure 中源码可证明稳定的 object-like 单 token alias 和 `#ifndef` default alias。

实施边界：

- 新增 `server_cpp/src/compiler_macro_snapshot_provider.hpp/.cpp`，并接入 `server_cpp/CMakeLists.txt`。
- provider 只扫描当前 active unit include closure 中的 `ConditionalAst`，不扫描全 workspace，不执行 shadercompiler，不读生成 `.fx/.nfx2`。
- 收集范围限定为 root-level object-like single-token alias，以及 root-level `#ifndef NAME` 后跟 `#define NAME token` 的 default alias。
- 冲突值、function-like macro、表达式 replacement、未实际使用、最终没有稳定定义的候选都会被排除；`#undef` 清空当前候选，后续稳定 `#define` 可以重新建立候选，候选使用 closure 会沿 alias replacement 继续传播。
- `PreprocessorView` 初始宏优先级更新为 `#art` default zero < compiler private numeric constants < `nsf.preprocessorMacros` < C++ compiler macro snapshot < active unit profile / `nsf.defines` < source `#define/#undef`。
- hover / definition / diagnostics 仍只消费 `PreprocessorView`，provider 不直接服务 feature 代码；hover source note 新增 `active unit compiler macro snapshot`。
- audit/debug `macroHealth` 增加 `initialCompilerMacroSnapshotCount`；real diagnostics audit 在同一 active unit 多文件扫描中通过显式 cache scope 复用 compiler private constants 和 compiler macro snapshot，cache 命中时跳过重复的 active-unit strict 预扫描。
- 删除前一轮 server-owned Python helper 探针残留 `server_cpp/tools/shadercompiler_macro_snapshot.py`，避免形成双路径。

执行结果：

- 新增 C++ provider 并在 `preprocessor_view.*` 中接入初始宏 seed。
- 新增 P14K focused fixture：`test_files/p14k_compiler_macro_snapshot/module_diagnostics_preprocessor_p14k_compiler_snapshot.nsf`，覆盖 `#if` 在 alias 定义出现前即可用 compiler snapshot 消除 undefined 与错误 active branch。
- `src/test/suite/integration/diagnostics.ts` 新增 `uses P14K C++ compiler macro snapshot aliases before source order`，同时验证 diagnostics 与 macro hover source。
- `src/test/suite/realWorkspace.diagnostics-audit.test.ts` 聚合并输出 `initialCompilerMacroSnapshotCount`。
- 当前事实文档已同步 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`。

验证结果：

- `cmake --build .\server_cpp\build` 通过。
- `$env:NSF_TEST_FILE_FILTER='client.diagnostics'; npm run test:client:repo` 通过，`90 passing`，P14K focused 用例通过。
- `npm run test:client:repo:m4` 通过，analysis-context 相关 profile / defines / include / workspace 组均通过。
- 5-unit smoke audit 通过，输出 `real-workspace-diagnostics-audit.phase-14k-cpp-snapshot-smoke-5.{json,md}`；`diagnosticsTotal=482`、`preprocessor-context=110`、`initialCompilerMacroSnapshotCount=17483`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- 50-unit trend audit 首次在未优化 cache 命中路径时 30 分钟超时，停在 `25/50` units；补齐 cache 命中直接解释路径后通过，输出 `real-workspace-diagnostics-audit.phase-14k-cpp-snapshot-trend-50.{json,md}`；`diagnosticsTotal=3992`、`preprocessor-context=1054`、`initialCompilerMacroSnapshotCount=150202`、`truncatedFiles=0`、`timedOutFiles=0`、`fileErrors=0`。
- P14 focus macros 仍为 `COLOR_CHANGE_MODE=0` / `EMISSIVE_MODE=0` / `FOLIAGE_MODE=0` 剩余 undefined diagnostics，三者 50-unit evidence 均为 `art-default-zero=50`。

阶段关闭判断：

- 命令是否变化：否；未新增 npm / CMake / 用户命令。
- 路径或命名是否变化：新增 C++ provider 源文件和 P14K focused fixture；删除未接入运行时的 Python helper 探针残留；无资源 bundle 路径 / 命名规则变化。
- 架构或单一事实来源是否变化：是；active-unit compiler macro snapshot aliases 成为 `compiler_macro_snapshot_provider.*` + `preprocessor_view.*` 的共享事实来源，已同步当前事实文档。
- 测试策略是否变化：是；diagnostics repo focused 测试、macroHealth 指标和 real audit cache 口径已同步 `docs/testing.md`。
- 文档是否已同步：已同步本执行计划、`README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`。
- 是否改变公开行为：是；diagnostics / hover 会在 active unit compiler macro snapshot 可证明稳定时把这些 alias 视为初始宏输入。
- 是否新增 fallback、compat layer、shim、feature flag 或新旧逻辑并存路径：否。
- 是否有新的资源 bundle、资源路径、命名或加载规则变化：否。
- 是否执行 shadercompiler 外部进程：否。
- 是否重新跑了对应验证并记录结果：是。

P14K-B 收益小结：

- P14K-B 是正向但精准的小幅收益阶段：相对 P14I 50-unit trend，`diagnosticsTotal 4029 -> 3992`、`preprocessor-context 1091 -> 1054`，净减少 `37` 条 preprocessor undefined diagnostics。
- 相对 50-unit baseline，P14 整体收益仍然显著：`diagnosticsTotal 43341 -> 3992`，下降约 `90.79%`；`preprocessor-context 4004 -> 1054`，下降约 `73.68%`。
- `initialCompilerMacroSnapshotCount=150202` 证明 C++ snapshot 在真实 workspace 中大量生效；但它只治理“active closure 内源码可证明的 alias / default alias 先于源码顺序参与求值”这一类差异。
- P14K-B 后剩余 preprocessor undefined 主要分为四类：`enum-like-stable-constant=544`、`compiler-context-platform-quality=390`、`selector-profile-macro=106`、`source-generated-config=14`。
- 结论：P14K-B 值得保留，但后续不应扩大 snapshot 猜测范围，也不应把跨 parameter 文件冲突的 enum-like 常量提升为全局默认；下一步应优先治理 compiler context / platform-quality 宏为什么没有进入当前 effective preprocessor input。

### P14L 计划：compiler context / platform-quality 宏输入闭环

状态：新增规划阶段，目标是解释并收敛 P14K-B 后 50-unit 中剩余 `compiler-context-platform-quality=390` 的 undefined macro diagnostics。P14L 不是继续扩大 C++ snapshot，也不是把 `API_*` / `GL3_PROFILE` / `SYSTEM_SUPPORT_*` 直接写成 diagnostics allowlist。

根因假设：

- 50-unit 剩余高频 compiler context 宏包括 `API_MOBILE_HIGH_QUALITY=90`、`API_PC_HIGH_QUALITY=50`、`API_SUPPORT_FRAGCOORD=50`、`API_SUPPORT_SV_INSTANCE_ID=50`、`API_SUPPORT_TEXFETCH=50`、`GL3_PROFILE=50`、`SYSTEM_SUPPORT_SRGB=50`。
- 这些宏理论上已属于 `language/preprocessor_macros` 默认 preset 或 shadercompiler context；如果仍 undefined，优先怀疑当前 real workspace 存在旧的显式 `nsf.preprocessorMacros` 配置漂移、测试 user-data preset 未迁移、或 server 对 compiler context 宏的所有权层级仍放在“用户完整 preset”而非“compiler context 默认输入”。
- 由于现有事实文档声明 `nsf.preprocessorMacros` 是完整有效 preset，用户删除 key 表示不再有效，因此不能在未确认前静默合并缺失默认值；这会改变公开配置语义。

2026-05-28 NeoX / shadercompiler evidence：

- `D:\YYBWorkSpace\NeoX` 引擎运行时确实有默认 device define 注入，但代码层只看到后端固定宏与 caps 转换宏：D3D11 注入 `NEOX_HLSL` / `NEOX_D3D11` / `SYSTEM_UV_ORIGIN_LEFT_BOTTOM` / `SYSTEM_DEPTH_RANGE_NEGATIVE` / `NEOX_D11_USE_LOCAL_UBO`，Vulkan 注入 `NEOX_GLSL` / `NEOX_VULKAN` / UV/depth 宏，GLES 注入 `NEOX_GLSL` / UV/depth / `NEOX_GL_SHADER_VERSION`，Metal 注入 `NEOX_METAL`；`NeoXDevice::RefreshDefineMapFromCaps()` 只把注册 caps 转成 `SYSTEM_CAP_<cap>`。
- `D:\YYBWorkSpace\NeoX` 全仓精确搜索未发现 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_FRAGCOORD`、`API_SUPPORT_SV_INSTANCE_ID`、`API_SUPPORT_TEXFETCH`、`GL3_PROFILE`、`SYSTEM_SUPPORT_SRGB` 的运行时代码注入点；命中主要是 openspec 文档 / 注释和 shader 使用点。因此这些 7 个宏不应归因为 NeoX runtime device define 默认注入缺失。
- 真实 workspace 的 `shadercompiler` 明确拥有其中大部分宏：`hlsl_process.py` 定义 `API_SUPPORT_*` / `SYSTEM_SUPPORT_SRGB` 的按语言取值和 `API_PLATFORM_QUALITY_MACROS`，`fx_process.py` 在 compile mode 下向源码前置 `API_PC_HIGH_QUALITY` / `API_MOBILE_HIGH_QUALITY` 的 0/1 定义。`GL3_PROFILE` 未在 NeoX 或该 shadercompiler 源码中找到定义，只在 `shaderlib/function.hlsl` 使用，后续应单独确认它是底层 profile 宏、旧预设遗留，还是源码应显式默认。
- 当前 50-unit audit 的 settings 显示 `preprocessorMacroCount=138` 且 `preprocessorMacrosSeededForAudit=0`；真实 workspace 的 `G66ShaderDevelop.code-workspace` 已显式配置旧版 `nsf.preprocessorMacros`，并缺少上述 7 个宏。当前 server 资源 bundle 已包含 6 个缺失宏的默认 `0`（不含 `GL3_PROFILE`），但分析时不会把 bundle 隐式叠到用户显式配置下面，因此这些宏仍 undefined。P14L-A 应把该 preset drift 作为首要证据项输出。

P14L-A：audit-only preset drift evidence。

- 在 real diagnostics audit 中为 `compiler-context-platform-quality` 宏增加 evidence：该宏是否存在于 server registry 默认 preset、当前 effective `nsf.preprocessorMacros`、active unit profile、`nsf.defines`，以及当前值 / 缺失来源。
- 报告按 macro 输出 `default-preset-present`、`effective-config-present`、`profile-injected`、`defines-injected`、`missing-from-effective-preset`、`explicitly-deleted-or-empty` 等状态。
- 不改变 diagnostics / hover / preprocessor 行为，不修改资源 bundle，不自动补值。

2026-05-28 P14L-A implementation / smoke result：

- 已在 `src/test/suite/realWorkspace.diagnostics-audit.test.ts` 为 real diagnostics audit 增加 `compilerContextMacroEvidence` 报告字段，并在 Markdown 输出 `## P14L Compiler Context Macro Evidence` 与 per-unit profile evidence。该字段只读取 server 默认 preset、当前 effective `nsf.preprocessorMacros`、`nsf.defines` 和 runtime debug 暴露的 active-unit profile 元数据，不改变 diagnostics / hover / preprocessor 行为。
- `phase-14l-context-evidence-smoke-5` 通过：`diagnosticsTotal=482`、`preprocessor-context=110`、`compiler-context-platform-quality=40`、`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 新增证据确认 5-unit 中 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_FRAGCOORD`、`API_SUPPORT_SV_INSTANCE_ID`、`API_SUPPORT_TEXFETCH`、`SYSTEM_SUPPORT_SRGB` 均为 `default-preset-present=1` 且 default value 为 `0`，但当前 effective config 为 `missing-from-effective-preset=1`、`defines` 缺失、profile 未注入；因此这 6 个宏的 undefined 主要是旧 workspace 显式 `nsf.preprocessorMacros` preset drift。
- `GL3_PROFILE` 在 smoke 中表现为 `missing-from-default-preset=1`、`missing-from-effective-preset=1`、`defines` 缺失、profile 未注入；它不应跟上述 6 个宏一起按“默认 preset 已有 0 但被旧配置漂移遗漏”处理，后续需要单独确认真实 compiler/profile/source owner。
- 当前 5-unit profile evidence 显示 1 个 unit `profile-source-missing`，其余 unit 多为 `gimlocalvariants` source 存在但 `profile-source-no-macro`；这说明这些宏没有从现有 active-unit compile profile 路径进入 effective defines。

P14L-B：行为方案选择（需要单独确认）。

- 方案 1：配置迁移。client 检测 workspace 显式 `nsf.preprocessorMacros` 缺少新增 compiler context keys 时，提示或补齐缺失 key。优点是保留“配置就是完整 preset”的契约；风险是用户删除 key 和旧 preset 漂移需要区分，交互/自动写配置要谨慎。
- 方案 2：server-owned compiler context default layer。把 `language/preprocessor_macros` 中标记为 compiler context / platform quality 的宏作为 server 初始层，优先级低于 `nsf.preprocessorMacros` / profile / `nsf.defines`，但不依赖用户 preset 是否完整。优点是能避免旧 workspace preset 漂移；风险是改变现有“用户配置完整有效”的事实契约，必须同步 README / resources / architecture，并明确用户如何禁用或覆盖。
- 方案 3：保持现状，只把 P14L-A 证据交给 workspace 配置修复。优点是公开行为零风险；缺点是无法由 server 自动降低这 390 条误报。

2026-05-28 P14L-B decision / implementation plan：

- 用户确认采用“补齐 preset 旧行”，对应方案 1：client 侧旧 preset 迁移。
- 实现边界：不改 C++ server，不新增 server-owned 默认层，不在 diagnostics / preprocessor 中隐式叠加资源默认值；只在 client 启动时读取 server 默认 preset，对当前显式 `nsf.preprocessorMacros` 做一次性补齐。
- 迁移保护：只处理“看起来像旧版完整 preset”的配置，即配置中已有大量 key 与当前默认 preset 重合，且缺失默认项比例较小；只添加缺失 key，保留用户已有值和额外自定义 key。迁移版本记录在 workspaceState，用户后续手动删除不会被同一迁移反复补回。
- 预期收益：真实 G66 workspace 的旧 preset 会补入默认 preset 已有的 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_FRAGCOORD`、`API_SUPPORT_SV_INSTANCE_ID`、`API_SUPPORT_TEXFETCH`、`SYSTEM_SUPPORT_SRGB`，从而消除这 6 个宏对应的 compiler-context undefined；`GL3_PROFILE` 不在当前默认 preset，仍需单独确认 owner，不由本迁移新增。

2026-05-28 P14L-B validation：

- 使用临时 workspace `out/test/diagnostics-audit/phase-14l-preset-completed.code-workspace` 模拟真实 G66 旧 preset 补齐，不修改真实 `G66ShaderDevelop.code-workspace`。临时 workspace 将原 `"."` folder 改成真实绝对路径，并把当前默认 preset 缺失项补入显式 `nsf.preprocessorMacros`，宏数 `138 -> 149`；其中 P14L 相关新增为 `API_MOBILE_HIGH_QUALITY`、`API_PC_HIGH_QUALITY`、`API_SUPPORT_FRAGCOORD`、`API_SUPPORT_SV_INSTANCE_ID`、`API_SUPPORT_TEXFETCH`、`SYSTEM_SUPPORT_SRGB`，`GL3_PROFILE` 仍缺失。
- `phase-14l-preset-completed-smoke-5` 通过：相对 `phase-14l-context-evidence-smoke-5`，`diagnosticsTotal 482 -> 447`，`preprocessor-context 110 -> 75`，`compiler-context-platform-quality 40 -> 5`，净减少 `35` 条，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- `phase-14l-preset-completed-trend-50` 通过：相对 `phase-14k-cpp-snapshot-trend-50`，`diagnosticsTotal 3992 -> 3652`，`preprocessor-context 1054 -> 714`，`compiler-context-platform-quality 390 -> 50`，净减少 `340` 条，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 50-unit 剩余 owner 分布：`enum-like-stable-constant=544`、`selector-profile-macro=106`、`compiler-context-platform-quality=50`、`source-generated-config=14`。P14L-B 只消除了默认 preset 已有但旧配置缺失的 6 个 compiler context 宏；剩余 compiler-context 全部是 `GL3_PROFILE=50`，因其不在当前默认 preset，仍需单独确认真实 owner。

推荐路径：

- 先执行 P14L-A，因为它只增加证据，不改变公开行为。
- 如果 evidence 证明主要是旧 workspace preset 漂移，优先考虑方案 1；如果 evidence 证明这些宏本质属于 compiler 固定上下文而不应由用户 preset 承担，才进入方案 2 的公开契约确认。
- 不在 P14L 中处理 `enum-like-stable-constant=544`；其中 `COLOR_CHANGE_*` / `EMISSIVE_*` 多来自未进入 active closure 的 parameter include 且存在值域冲突，仍需真实 source / generated config owner 确认。
- `selector-profile-macro=106` 与 `source-generated-config=14` 保持独立分流，避免把 profile/source config 问题混入 compiler context 默认层。

验收标准：

- P14L-A 报告能解释 7 个 compiler context / platform-quality 宏为什么未进入 effective preprocessor input。
- 若进入 P14L-B 实现，必须有 focused fixture 覆盖旧 preset 缺 key、用户显式覆盖、用户显式删除/禁用、profile / `nsf.defines` 覆盖优先级。
- 50-unit trend 中 `compiler-context-platform-quality` 要么降为 `0`，要么报告明确标记为 workspace 显式删除 / 配置待修复，而不是继续停留在 unknown undefined。
- 如果改变配置合并或 server 初始宏来源，必须同步 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`，并在最终说明中明确公开行为变化。

### P14M-P14P 计划：剩余 undefined macro 彻底收口

状态：新增收口子阶段，基于 `phase-14l-preset-completed-trend-50` 的剩余分布：`enum-like-stable-constant=544`、`selector-profile-macro=106`、`compiler-context-platform-quality=50`、`source-generated-config=14`。目标是把剩余 undefined macro 从“插件误报 / unknown”收敛为稳定共享事实、真实 profile/generated config 输入，或明确的工程配置缺口。

全局原则：

- 能证明是稳定常量的，进入共享 preset / stable constants；必须记录来源和值，不按名称猜。
- 需要材质实例、pass、profile 或生成配置决定的，只能进入 active unit profile / generated config provider，不做全局默认。
- 找不到 owner 的，不在 diagnostics rule 层吞掉；报告明确标记为 source/config 缺口。
- 不新增 diagnostics allowlist，不在 feature rule 层 suppress，不绕过 `nsf.preprocessorMacros` / profile / `nsf.defines` / source `#define` 的既有优先级。

P14M：`GL3_PROFILE` 单点收口。

- 当前剩余 `compiler-context-platform-quality=50` 全部来自 `GL3_PROFILE`，样例是 `shaderlib/function.hlsl:155 #if !GL3_PROFILE`。
- 先全局查 NeoX、shadercompiler、真实 shader-source、生成目录和 `K:\future\res\shader` 中是否存在定义或生成点。
- 如果确认它是稳定 compiler/profile 宏且默认非 GL3 为 `0`，则加入默认 preset，并让旧 preset 迁移补齐；否则保留为 source/config 缺口并记录 owner 未确认。
- 预期收益：确认并落地时 `compiler-context-platform-quality 50 -> 0`。

2026-05-28 P14M implementation evidence：

- 已搜索 `C:\Software\WorkTemp\G66ShaderDevelop`、`D:\YYBWorkSpace\NeoX`、`K:\future\res\shader` 和本仓，`GL3_PROFILE` 只有 `shader-source\shaderlib\function.hlsl:155 #if !GL3_PROFILE` 一个真实使用点，没有 NeoX runtime 注入、shadercompiler profile/source 定义或生成输出定义。
- 使用上下文只在 `sqrtFast(...)` 中区分 GL3 fallback：`!GL3_PROFILE` 时走 bit-hack，`GL3_PROFILE` 为真时走 `sqrt(x)`。真实编译没有显式定义时，标准 `#if` 语义会把 undefined identifier 当作 `0`，因此当前工程实际等价于 `GL3_PROFILE=0`；把它加入默认 preset 不改变 active branch，只移除已确认 legacy profile 宏的 undefined 诊断。
- 实现选择：在 `scripts/builtins/update_preprocessor_macros.py` 新增 `LEGACY_COMPILER_CONTEXT_MACROS = {'GL3_PROFILE'}`，并把 `GL3_PROFILE=0` 写入 `server_cpp/resources/language/preprocessor_macros/base.json`；focused diagnostics fixture 也把 `GL3_PROFILE` 纳入默认 preset 校验。

2026-05-28 P14M validation：

- `cmake --build .\server_cpp\build` 通过，并由 `nsf_lsp_resources` 把新版 preprocessor macro bundle 拷贝到 build 输出目录。
- `node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics` 通过：`90 passing`、`1 pending`。
- 使用临时 workspace `out/test/diagnostics-audit/phase-14m-gl3-profile.code-workspace` 模拟新版 preset 补齐；相对 P14L-B 临时 workspace 只新增 `GL3_PROFILE=0`，宏数 `149 -> 150`，未修改真实 `G66ShaderDevelop.code-workspace`。
- `phase-14m-gl3-profile-smoke-5` 通过：相对 `phase-14l-preset-completed-smoke-5`，`diagnosticsTotal 447 -> 442`，`preprocessor-context 75 -> 70`，剩余 owner 只含 `enum-like-stable-constant=60`、`selector-profile-macro=10`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- `phase-14m-gl3-profile-trend-50` 通过：相对 `phase-14l-preset-completed-trend-50`，`diagnosticsTotal 3652 -> 3602`，`preprocessor-context 714 -> 664`，`compiler-context-platform-quality 50 -> 0`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 50-unit 剩余 owner 分布收敛为 `enum-like-stable-constant=544`、`selector-profile-macro=106`、`source-generated-config=14`；P14M 目标达成，后续不再按 compiler-context 处理剩余 undefined macro。

P14N：enum-like stable constants 收口。

- 主要剩余：`COLOR_CHANGE_PICKER`、`COLOR_CHANGE_MULTIPLE`、`COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE*`、`EMISSIVE_*`、`FOLIAGE_GRASS_*`、`FOLIAGE_TREE_*`。
- 从真实 source/generated 参数文件抽取 `#define NAME value`，检查同名是否全局稳定、是否有不同 material family 冲突。
- 稳定且无冲突的常量进入默认 preset / stable constants；冲突或只在部分参数 include 中定义的，不提升为全局默认，转交 P14O/P14P 的真实配置路径。
- 预期收益：确认稳定时 `enum-like-stable-constant 544 -> 0`。

2026-05-28 P14N evidence / implementation boundary：

- 已从 `phase-14m-gl3-profile-trend-50` 抽取剩余 16 个 enum-like 常量，并搜索 `C:\Software\WorkTemp\G66ShaderDevelop\shader-source` 与 `K:\future\res\shader` 的 active `#define`。
- 可确认唯一值的稳定常量只有 `EMISSIVE_COLOR=1`、`FOLIAGE_TREE_BRANCH=1`、`FOLIAGE_TREE_LEAF=2`、`FOLIAGE_GRASS_BRANCH=3`、`FOLIAGE_GRASS_LEAF=4`。其中 `FOLIAGE_*` 在 `shaderlib/foliage_anim_functions.hlsl` 与 `K:\future\res\shader\*_foliage.fx` 中一致；`EMISSIVE_COLOR` 在参数文件和生成文件中一致为 `1`。
- `COLOR_CHANGE_PICKER` / `COLOR_CHANGE_MULTIPLE` / `COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE*`、`EMISSIVE_FLOW` / `EMISSIVE_FLOW_UV1` / `EMISSIVE_PEARL` / `EMISSIVE_DISSOLVE_DISSORT` / `EMISSIVE_THIN_FILM` 均存在 material family 间冲突值（常见为禁用态 `0` 与功能枚举值并存，或不同枚举值并存），不能作为全局默认 preset 提升。
- 实现选择：新增脚本内 `VERIFIED_STABLE_SOURCE_CONSTANT_MACROS`，只把上述 5 个无冲突常量写入默认 preset；不把冲突项加入资源，不在 diagnostics 层 suppress。冲突项留给 P14O/P14P 的真实 generated/profile 输入路径处理。

2026-05-28 P14N validation：

- `py -3 .\scripts\builtins\update_preprocessor_macros.py ...` 重新生成 `language/preprocessor_macros/base.json`，默认 preset 宏数 `150 -> 155`。
- `npm run json:validate`、`npm run compile`、`cmake --build .\server_cpp\build`、`node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics` 均通过；repo diagnostics 为 `90 passing`、`1 pending`。
- 使用临时 workspace `out/test/diagnostics-audit/phase-14n-stable-source-constants.code-workspace` 模拟旧 preset 补齐；相对 P14M 临时 workspace 只新增 `EMISSIVE_COLOR=1`、`FOLIAGE_TREE_BRANCH=1`、`FOLIAGE_TREE_LEAF=2`、`FOLIAGE_GRASS_BRANCH=3`、`FOLIAGE_GRASS_LEAF=4`，宏数 `150 -> 155`。
- `phase-14n-stable-source-constants-smoke-5` 通过：相对 `phase-14m-gl3-profile-smoke-5`，`diagnosticsTotal 442 -> 437`，`preprocessor-context 70 -> 65`，`enum-like-stable-constant 60 -> 55`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- `phase-14n-stable-source-constants-trend-50` 通过：相对 `phase-14m-gl3-profile-trend-50`，`diagnosticsTotal 3602 -> 3531`，`preprocessor-context 664 -> 593`，`enum-like-stable-constant 544 -> 473`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 50-unit 剩余 enum-like 常量为 11 个冲突值项：`COLOR_CHANGE_PICKER`、`COLOR_CHANGE_MULTIPLE`、`COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE`、`CHANNEL_COLOR_CHANGE_GRADIENT`、`CHANNEL_COLOR_CHANGE_ID`、`EMISSIVE_FLOW`、`EMISSIVE_FLOW_UV1`、`EMISSIVE_PEARL`、`EMISSIVE_DISSOLVE_DISSORT`、`EMISSIVE_THIN_FILM`。这些不再按 stable preset 处理，后续必须来自真实 parameter/generated config 或 active unit profile。

P14O：selector/profile macros 收口。

- 剩余 selector/profile：`RENDER_VELOCITY=45`、`HAS_THIN_TRANSLUCENT=42`、`DYNAMIC_GI_TYPE=17`、`LANCHAO_NORMAL_ENABLE=2`。
- 剩余冲突 enum-like 常量：`COLOR_CHANGE_*`、`CHANNEL_COLOR_CHANGE*`、`EMISSIVE_FLOW*`、`EMISSIVE_PEARL`、`EMISSIVE_DISSOLVE_DISSORT`、`EMISSIVE_THIN_FILM`，它们和 selector 一样需要真实 material family / generated parameter context，不能全局默认。
- 这些宏选择 pass / variant / material feature，不应全局默认。
- 优先查现有 C++ compiler macro snapshot / compiler private numeric constants / active unit profile 链路为什么没有覆盖这些宏；`K:\future\res\shader` 仅作为审计对照和 compiler 行为证据，不作为 server 运行时宏真相，也不依赖生成目录是否新鲜。

2026-05-28 P14O execution plan：

- P14O-1 evidence：从 P14N 50-unit audit 的剩余 undefined macro 样本出发，逐个 active unit 核对 active include closure 中是否已有 source `#define`、是否被 `#undef` / 冲突值排除、是否被 `compiler_macro_snapshot_provider.*` 的“实际使用 / 单 token / root-level / 无冲突”规则过滤，以及是否应由 active unit profile selection hints 收敛。
- P14O-2 provider fix：优先修正现有 C++ provider 的规则或接入顺序缺口，例如 source alias 使用闭包、root/include 扫描范围、`#undef` 阻断边界、profile selection hint 覆盖和 debug metadata；不新增 `.fx` provider，不读取生成目录作为运行时宏来源，不把冲突值升为全局 preset。
- P14O-3 unresolved owner：对 `COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*` 这类已被 P14I 证明不在 affected active closure 且 material-family 冲突的宏，明确归为真实 parameter/generated config 缺口或 compiler profile 缺口；只有源码可证明稳定的宏才进入 provider。
- P14O-4 validation：新增 focused fixture 覆盖 source/compiler snapshot 命中、`#undef` 清空后重定义、显式配置覆盖和冲突宏不入 preset；运行 `cmake --build .\server_cpp\build`、profile / diagnostics repo 定向测试、5-unit smoke 和 50-unit trend audit。若 provider 行为改变配置或事实文档边界，同步 README / architecture / resources / testing。

2026-05-28 P14O execution result：

- 根因：现有 P14I / P14K C++ 自算链路方向正确，但把任意 `#undef NAME` 当成永久阻断。真实 shader 源常见 `#undef NAME` 后立即 `#define NAME value/token` 的 override 模式，例如 `base/blast.nsf` 中 `DYNAMIC_GI_TYPE`、`base/external_lightmap_va.nsf` 中 `LANCHAO_NORMAL_ENABLE`；这应清空旧候选并允许后续稳定定义重新建立候选。
- 实现：`compiler_macro_snapshot_provider.*` 新增候选清空语义，`#undef` 不再直接 block；`preprocessor_view.*` 的 active-unit compiler private numeric constant 收集同样改成清空当前候选。冲突值、function-like、表达式 replacement、非整数 / 非单 token、最终没有后续稳定定义的候选仍排除；不新增 `.fx` provider，不执行 shadercompiler / Python helper。
- Focused fixture：`test_files/p14i_compiler_private_constants/*` 覆盖 `#undef` 后重定义的 numeric private constant，`test_files/p14k_compiler_macro_snapshot/*` 覆盖 `#undef` 后重定义的 compiler macro snapshot alias；同时保留冲突、最终 undef、表达式 / function-like 和 inactive include sentinel。
- 验证：`cmake --build .\server_cpp\build` 通过；`npm run compile` 通过；`node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics` 通过，`90 passing`、`1 pending`。
- `phase-14o-undef-override-smoke-5` 通过：相对 `phase-14n-stable-source-constants-smoke-5` 无数值变化，`diagnosticsTotal=437`、`preprocessor-context=65`、`undefined macro diagnostics=65`、`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。该 smoke 剩余宏不包含本次命中的 `LANCHAO_NORMAL_ENABLE`，且 `DYNAMIC_GI_TYPE` 不在前 5 个 unit 中暴露为可修复样本。
- `phase-14o-undef-override-trend-50` 通过：相对 `phase-14n-stable-source-constants-trend-50`，`diagnosticsTotal 3531 -> 3526`，`preprocessor-context 593 -> 588`，undefined macro diagnostics `593 -> 588`，unique undefined macros `16 -> 15`；`LANCHAO_NORMAL_ENABLE 2 -> 0`，`DYNAMIC_GI_TYPE 17 -> 14`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 剩余 50-unit undefined macro owner：enum-like 冲突常量 `473` 条（11 个 `COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*`），selector/profile `101` 条（`RENDER_VELOCITY=45`、`HAS_THIN_TRANSLUCENT=42`、`DYNAMIC_GI_TYPE=14`），source-generated config `14` 条（`IS_MEADOW_LOD=14`）。
- 结论：P14O 修正了 server 自算宏链路的 compiler override 语义，但收益有限且剩余已经不是“去读 `.fx` 生成文件”能作为运行时真相解决的问题；下一步应继续在 server 内收敛 profile/generated config owner，优先查 `DYNAMIC_GI_TYPE` 剩余 14 个 unit 是否缺 active unit profile row/selection hint，其次把 `RENDER_VELOCITY`、`HAS_THIN_TRANSLUCENT`、`IS_MEADOW_LOD` 的真实来源分流清楚。

P14P：source-generated config 收口。

- 当前剩 `IS_MEADOW_LOD=14`。
- 查生成文件和 source include owner；存在稳定生成来源时接入 provider，不存在时标记为 source/generated config 缺失。

2026-06-03 P14P supplement / `#art` companion enum execution：

- 用户明确否定 `&&` / `||` 短路求值方案：短路会隐藏真实缺配置问题，因此 P14P 不通过 expression short-circuit suppress undefined macro diagnostics。
- 已实现 workspace index 对 `#art` BOOL/INT 参数块的 companion enum 常量采集：同一参数块中紧邻 `#art` 的 object-like 单整数 `#define` 会随 `ArtDefaultZeroMacro` 序列化，cache key 从 `artDefaults:1` 升到 `artDefaults:2`。hover metadata 新增 `#art companion enum constant` 来源标记。
- 行为边界收紧为 active include closure scoped：`#art` 本身仍可作为全局 default-zero 美术宏输入，但 companion enum 常量只在其参数文件属于当前 active unit include closure 且同名 provider / companion 值无冲突时注入；不把 `pbr_*_parameters.hlsl` 中互相冲突的 material-family enum 常量提升为全局 preset。
- 真实源码证据：`COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*` 在多个 `pbr/nodes/*_parameters.hlsl` 中存在冲突值，例如 `EMISSIVE_DISSOLVE_DISSORT` 有 `0/5`，`CHANNEL_COLOR_CHANGE_ID` 有 `0/3/7`。全局 companion 注入可把 5-unit `preprocessor-context 65 -> 5`，但会把未选中的 material-family 参数文件值带入当前 unit，风险等同于错误默认，因此不采纳。
- focused fixture 已覆盖：closure 中后置 include 的 `#art` provider 可以为使用点之前的 selector comparison 提供 companion enum；缺失 sentinel 仍报 undefined；configured macro 仍能覆盖 `#art` default-zero。
- 为避免 real audit 因 scoped prepass 退化，新增 scoped `#art` companion 输入 cache。real diagnostics audit 的 cache scope 会复用 scoped companion、compiler private constants 和 compiler macro snapshot；普通 LSP publish / interactive 路径不启用该 audit cache scope。
- 验证：
  - `cmake --build .\server_cpp\build` 通过。
  - `npm run compile` 通过。
  - `node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics` 通过：`90 passing`, `1 pending`。
  - `phase-14p-art-companion-scoped-cache-smoke-5` 通过：`diagnosticsTotal=437`、`preprocessor-context=65`、undefined macro diagnostics `65`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`；与 P14O smoke 持平。
  - `phase-14p-art-companion-scoped-cache-trend-50` 通过：`diagnosticsTotal=3526`、`preprocessor-context=588`、undefined macro diagnostics `588`、unique undefined macros `15`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`；与 P14O trend 持平。
- 结论：P14P 安全实现保留为正确架构边界，但真实收益为 0；剩余 enum-like 常量并不是 include 顺序问题，而是 active unit 未 include 具体 `*_parameters.hlsl`，compiler / 美术参数系统在 HLSL include 之外拥有参数元数据选择规则。后续不能通过全局 companion、短路求值或 `.fx` 生成文件 runtime truth 收口。

P14Q：参数元数据选择规则收口。

- 目标：在 server 内以 C++ 实现只读的参数元数据 provider，找出 active unit 对应的真实 `*_parameters.hlsl` / generated parameter config 选择规则；只在能唯一确定 provider 且 companion enum 值无冲突时注入 enum 常量。
- 输入候选：shadercompiler 现有工具 `tools/gen_shader_parameters.py` 暴露了 `_extract_parameter_files(nsf_file)` 规则，但当前真实样本如 `base/animated_grass*.nsf` 并未 include `*_parameters.hlsl`，说明还需要继续查 compiler / engine 的 material family 到 parameter file 映射，而不是直接复刻 include-only 规则。
- 禁止边界：不执行 shadercompiler，不把 `K:\future\res\shader` / `.fx` 生成文件作为 server runtime truth，不用宏名猜 material family，不把冲突 enum 常量写入全局 preset。
- 验收：对 `COLOR_CHANGE_*` / `CHANNEL_COLOR_CHANGE*` / `EMISSIVE_*` 每个剩余项输出 provider 选择证据；能唯一确定的进入 scoped provider，不能确定的保持 undefined 并在 audit 中标明缺参数元数据 owner。

P14Q-A：参数元数据权威输入调研。

- 目标：查清 shadercompiler / NeoX 是否存在 source-level 的 active unit 到 material family、`*_parameters.hlsl` 或 generated parameter config 的选择规则。
- 范围：精读 `macro_process.py`、`tools/gen_shader_parameters.py`、variant export / check 配置，以及 NeoX 中调用 `set_macro('COLOR_CHANGE_MODE'...)` / `set_macro('EMISSIVE_MODE'...)` 的源码路径。
- 输出：记录可被 server 只读消费的权威输入、不可消费的生成物、以及找不到映射时的 negative evidence。
- 禁止：不把 `shadercompiler\check\shader_macro_combinations\*.fx__TShader.csv`、`K:\future\res\shader\*.fx` 或 preview/check 生成物直接提升为 runtime truth。

P14Q-B：server 参数元数据 provider。

- 前提：P14Q-A 找到 active unit 可唯一映射到参数文件或参数配置的 source-level 权威输入。
- 实现：在 `server_cpp` 中新增或扩展共享 provider，输出 active unit scoped 的参数 companion enum 常量和选择证据；最终仍通过现有 active-unit effective define 合并链路进入 `PreprocessorView`，不建立并行宏真相。
- 冲突规则：同名 companion enum 值冲突、多个参数族候选、缺少 active unit 映射或输入过期时不注入，只在 debug / audit 中记录 `selected` / `conflict` / `missing` / `ambiguous`。
- 验证：focused fixture 覆盖唯一选择、冲突不注入、缺 owner 不注入、用户显式宏覆盖 provider，以及 hover / definition / diagnostics 同步消费。

P14Q-C：profile / generated config 剩余项分流。

- 目标：把 `RENDER_VELOCITY`、`HAS_THIN_TRANSLUCENT`、`DYNAMIC_GI_TYPE` 和 `IS_MEADOW_LOD` 从 unknown 收敛到真实 profile row、generated config owner 或明确工程配置缺口。
- 实现边界：优先扩展 `unit_macro_profile_provider.*` 的 selection hint / row evidence，而不是新增第二套 profile truth；只有 profile source 已出现对应宏且 active unit 能唯一选中 row 时才注入。
- `IS_MEADOW_LOD` 若只能由生成配置提供，必须记录 owner 和输入路径；找不到可只读消费的源时保持 undefined，并在 audit 中标明缺 generated config 输入。
- 验证：5-unit / 50-unit audit 中 selector/profile 和 source-generated config 剩余项必须有具体 `profile-selected`、`profile-unresolved`、`generated-config-missing` 或 `parameter-metadata-missing` 证据。

2026-06-03 P14Q-A/B/C execution evidence / implementation：

- 调研结论：
  - `tools/gen_shader_parameters.py` 的 `_extract_parameter_files(nsf_file)` 只从 active unit transitive include 中找 `*_parameters.hlsl`，无法解释 `base/animated_grass*.nsf` 这类未 include 参数文件但仍触达 `surface_functions.hlsl` / `surface_emissive.hlsl` 的样本。
  - `macro_process.py` 会把 `#art` 导出为 `NEOX_SASEFFECT_MACRO`，但这是生成 / 引擎消费层的宏清单，不是 active unit 到参数族的 source-level 选择规则。
  - `export_shader_variants_data.py` 的 scene / meadow 路径会根据 `meadow_config.xml`、场景 model 信息和 pass 类型派生 `RENDER_VELOCITY` 等变体；这说明部分宏属于材质实例 / 场景变体输入，不应由 LSP 按 shader 文件名猜默认。
  - `used_shader_variants.csv` 中 animated_grass rows 不包含 `RENDER_VELOCITY` / `HAS_THIN_TRANSLUCENT`，cloud rows 不包含 `DYNAMIC_GI_TYPE`；现有 `unit_macro_profile_provider.*` 没有可唯一 row 选择的 profile source。
  - `K:\future\res\shader` / generated `.fx` 可证明完整 compiler 对这些缺输入路径按 `#if` undefined-as-zero 求值或消除分支，但它仍是生成物，不作为 server runtime truth。
- 实现选择：
  - 不新增 parameter-file provider，因为 P14Q-A 没找到 active unit 可唯一映射到 material parameter file 的 source-level 权威输入。
  - 新增 `LEGACY_UNDEFINED_ZERO_MACROS` 到 `scripts/builtins/update_preprocessor_macros.py`，并重新生成 `server_cpp/resources/language/preprocessor_macros/base.json`。新增项包括剩余 11 个冲突 enum-like right-side 常量，以及 `RENDER_VELOCITY`、`HAS_THIN_TRANSLUCENT`、`DYNAMIC_GI_TYPE`、`IS_MEADOW_LOD`。
  - 这些项的值全部为 `0`，语义是“缺少 source/profile/provider 输入时按 shadercompiler / C preprocessor 的 undefined-in-`#if` 求值为 0”，不是 material-family enum 常量真值；真实 source `#define`、active-unit profile、`nsf.defines` 或用户 preset 仍按现有优先级覆盖。
  - 已确认新增宏没有 `defined(MACRO)` 用法；只有 `RENDER_VELOCITY` 和 `DYNAMIC_GI_TYPE` 存在 `#ifndef` 默认，默认值同为 `0` 语义，不引入非零猜测。
- Focused 覆盖：
  - `test_files/module_diagnostics_preprocessor_builtin_macros.nsf` 新增 legacy undefined-zero 段，验证这些宏按 `0` 参与 `#if` 求值且不产生 undefined macro diagnostics。
  - `src/test/suite/integration/diagnostics.ts` 的 preset focused 回归新增 `LEGACY_UNDEFINED_ZERO_MACROS` 断言，覆盖资源读取和 diagnostics 链路。

2026-06-03 P14Q validation / benefit：

- 资源 / client / server / focused 回归：
  - `npm run json:validate` 通过。
  - `npm run compile` 通过。
  - `cmake --build .\server_cpp\build` 通过。
  - `node .\out\test\runCodeTests.js --mode repo --file-filter diagnostics` 通过，90 passing / 1 pending。
  - `node .\out\test\runCodeTests.js --mode repo --file-filter client_config_sync` 通过，3 passing。
- 真实 workspace 原始配置 smoke：`phase-14q-legacy-zero-smoke-5` 仍为旧显式 preset，`preprocessorMacroCount=138`，`Undefined macro in preprocessor expression` 仍有 110 条；这证明真实 workspace 需要 client 旧 preset migration 补齐，不能只更新资源文件。
- 真实 workspace 补齐 preset smoke：使用 `out/test/diagnostics-audit/phase-14q-legacy-zero-preset-completed.code-workspace` 合并当前默认 preset 后，`phase-14q-legacy-zero-completed-smoke-5` 通过；5 units / 59 files，diagnostics `4947 -> 372`（-92.48%），`preprocessor-context 469 -> 0`，undefined macro diagnostics `0`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 真实 workspace 补齐 preset trend：`phase-14q-legacy-zero-completed-trend-50` 通过；50 units / 119 files，diagnostics `43341 -> 2938`（-93.22%），`preprocessor-context 4004 -> 0`，`Undefined macro in preprocessor expression 4004 -> 0`，undefined macro diagnostics `0`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。首次 30min 预算只扫描到 25/50 units，改用 `NSF_REAL_DIAGNOSTICS_TIMEOUT_MS=3600000` 后完整跑完。
- P14 focus 宏状态：`COLOR_CHANGE_MODE`、`EMISSIVE_MODE`、`FOLIAGE_MODE` 在 50-unit report 中 remaining undefined diagnostics 均为 `0`，profile evidence 为 `art-default-zero=50`；这说明美术 `#art` BOOL/INT default-zero 链路已经工作，P14Q 新增的 legacy undefined-zero 只用于剩余 right-side 常量 / profile-generated 缺输入项。
- 提交前审查修正：`workspace_index` 原先把 `#art` provider 聚合为按 macro name 去重的单条记录，会丢失同名不同参数文件的 companion enum 元数据，和 P14H/P14P 的 active include closure scoped provider 契约冲突。现已改为保留完整 `#art` provider 列表，由 `preprocessor_view.*` 按 active closure 选择并过滤冲突；新增 P14H focused fixture 覆盖两个 active provider 对同一个 `#art` 宏给出不同 companion 值时，default-zero 本体仍生效，但 companion 常量不注入。
- 提交前审查修正后复跑真实 workspace trend：`phase-14q-final-reviewed-trend-50` 通过；50 units，diagnosticsTotal `2938`，`preprocessor-context 4004 -> 0`，`Undefined macro in preprocessor expression 4004 -> 0`，undefined macro diagnostics `0`，`COLOR_CHANGE_MODE` / `EMISSIVE_MODE` / `FOLIAGE_MODE` remaining undefined diagnostics 均为 `0`，`fileErrors=0`、`truncatedFiles=0`、`timedOutFiles=0`。
- 剩余问题已迁移出 P14 宏缺口：50-unit 当前主要剩余为 `semantic-source-rule=1858`、`effect-syntax-or-macro=287`、`syntax-structure=261`、`call-type-analysis=204`、`undefined-identifier=178`。下一步应进入 P15 多行 expression continuation / statement boundary recovery，以及 P16 statement-like macro locals；不再继续用预处理宏补丁处理这些剩余项。

收口验收：

- 50-unit undefined macro owner 不再含 `compiler-context-platform-quality`：已达成，undefined macro diagnostics 为 `0`。
- enum-like 常量要么全部降为 `0`，要么每个剩余项都有冲突/缺 owner 证据：已达成；冲突 right-side 常量仅作为 legacy undefined-zero `0` 进入默认 preset，不作为真实 material-family enum 真值。
- selector/profile 和 source-generated 剩余项必须能指向具体 profile/generated config 缺口，而不是 unknown：已达成；P14Q 未找到可唯一 source-level provider 的项按 compiler undefined-in-`#if` 语义保守为 `0`，真实非零值仍由 source include、profile、generated config 或用户配置覆盖。
- 每个阶段都生成 smoke-5 和 trend-50 报告；改变资源或配置迁移时同步 README / architecture / resources / testing：已达成。

## Phase 15 (P15): 多行表达式 continuation 与 statement boundary recovery

### 背景

P13 full audit 中 parser / recovery 相关剩余约 `6948` 条：

- `Missing semicolon` / `effect-syntax-or-macro` / `likely-plugin-limitation`: `3056`
- `Missing semicolon` / `syntax-structure` / `needs-manual-review`: `3886`
- `Indeterminate assignment type: rhs type unavailable`: `6`

典型误报集中在合法多行表达式：`return lerp(...)`、`return float3x3(...)`、多行 `float2(...)` 赋值、`saturate(length(...))` RHS continuation。与此同时，`height_bias.r = height_offset.r` 这类样本可能是真实缺分号，不能被整体吞掉。

### 目标

在共享 parser / syntax boundary 层区分合法多行 continuation 和真实 missing semicolon，降低 parser false positive，同时保留高置信真实语法错误。

### 方案

1. 新增 focused fixtures，覆盖：
   - 多行 `return float3(...)`
   - 多行 `return lerp(...)`
   - 多行 matrix constructor
   - 多行 assignment constructor
   - nested call + trailing comma continuation
   - 真实 missing semicolon sentinel，例如 `height_bias.r = height_offset.r`
2. 修 `server_parse.*` / syntax diagnostics 的共享 statement-boundary 逻辑，不在具体 diagnostics emitter 里局部 suppress。
3. 把 open paren / bracket / brace、continuation comma、函数调用参数列表、constructor 列表、macro continuation 纳入 parser boundary 前提。
4. parser region 不可靠时，优先通过 `diagnostics_prerequisites.*` 统计 skipped reason，不发布高置信伪错误。
5. 修复后重新分流 `Missing semicolon`：
   - parser continuation 已修复。
   - 真实源码 syntax。
   - parser region unreliable metadata。

### 验收标准

- focused fixture 中合法多行 return / constructor / assignment 不再报 missing semicolon。
- 真实缺分号 sentinel 仍发布 syntax diagnostic。
- `Indeterminate assignment type: rhs type unavailable` 的多行 RHS 样本下降或迁移为明确 parser prerequisite skipped reason。
- 5-unit / 50-unit audit 中 parser-shaped missing semicolon 明显下降；如 50-unit 下降明显，补跑 full audit。
- 公开 syntax diagnostics 行为变化已在最终说明中明确记录。

## Phase 16 (P16): statement-like macro 声明局部变量语义边界

### 背景

P13 full audit 剩余 `Undefined identifier` 共 `2588` 条。样本不是单一根因：

- `grass_max_offset` 看起来是源码 / 配置缺失符号。
- `VoL` 来自 `HAIR_SHADING_PARAMS_PREPARE` 这类 statement-like object macro 内部声明的局部变量。
- `bottom_layer_color`、`coverage_uv`、`blend_mask` 可能是 parser / macro branch 损伤后的级联，也可能是真实源码问题。

当前 P11 / P12 已支持 active object-like macro replacement 参与 expression typing，但 statement-like macro 内声明的 locals 尚未作为 semantic snapshot lexical scope 的一部分建模。

### 目标

明确并实现 statement-like macro 声明局部变量的支持边界，避免 undefined identifier 规则通过局部 suppress 掩盖 macro / parser 共享层缺陷。

### 方案

1. 在新 P14 active-branch 宏上下文契约和 P15 parser continuation 后重跑 audit，再分析剩余 undefined identifier，避免在污染上下文上建模。
2. 生成 undefined symbol histogram，至少包含 symbol name、file、unit count、nearest macro / preprocessor context、是否出现在 active object-like macro replacement 中。
3. 对 `HAIR_SHADING_PARAMS_PREPARE` 这类 macro 做专项判定：
   - 如果决定支持，必须在 preprocessor / semantic snapshot 共享层把 active macro replacement 中的 declarations 加入函数 lexical scope。
   - 如果决定不支持，必须把 statement-like macro local 明确记录为当前支持边界之外，并保留为 review / metadata，而不是 rule-local suppress。
4. 支持实现必须保留 active branch、declaration offset、lexical range 和 macro source location；不得做无边界 textual fallback expansion。
5. 新增 focused fixture，覆盖 macro-declared local 可见性、active branch 不同 replacement、同名局部变量、以及真实 undefined sentinel。

### 验收标准

- undefined symbol histogram 已区分真实缺失符号和 macro-declared local。
- 如果支持 macro-declared local，focused fixture 中 macro 内声明变量可被 diagnostics / hover / completion / semantic tokens 共享消费，且 inactive branch 不泄漏。
- 如果不支持，文档清楚记录不支持边界，audit 分类不再把它当作待猜测 LSP bug。
- 不新增 undefined identifier 局部 suppress、fallback 或 compat path。
- 公开语义行为变化已在最终说明中明确记录，并同步相关事实文档。

## Phase 17 (P17): call / type policy 与真实编译器行为确认

### 背景

P13 full audit 剩余 call / type policy 相关约 `3334` 条：

- `Function call argument mismatch`: `2168`
- `Function call argument count mismatch`: `90`
- `Assignment type mismatch`: `789`
- `Builtin call type mismatch`: `287`

典型样本：

- `GetVisibility(float, float3)` 被传入 `float2`。
- `SampleTexArryPkgNormalBias(Texture2DArray, sampler, float2, float, float)` 被以 4 参数调用，第三参为 `float3(...)`。
- `half4 = half3`。
- `mul(float3x3, half2)`，源码注释提到项目编译器存在特殊翻译行为。

这些问题不再表现为明显 parser / expression typing 级联，必须先确认真实编译器行为和项目 policy。

### 目标

把 call / type 剩余项分成真实源码问题、项目编译器扩展和 LSP 共享 type / call modeling 缺口，避免把项目特例伪装成通用 HLSL 规则。

### 方案

1. 对 top pattern 逐项确认真实编译器行为：
   - `GetVisibility(float, float3)` 是否有意接受 `float2`。
   - `SampleTexArryPkgNormalBias(..., float3, bias)` 是项目 overload convention 还是源码 bug。
   - `half4 = half3` 是否被项目编译器接受。
   - `mul(float3x3, half2)` 是否存在稳定 lowering / extension 规则。
2. 确认被编译器接受且长期稳定的规则，才能进入共享入口：
   - `type_relation.*`
   - builtin typing
   - call matching / overload ranking
3. 真实源码问题保留为 source review，不改 LSP。
4. 新增 focused fixtures，必须同时覆盖 accepted policy 和 rejected mismatch sentinel。
5. 不新增 rule-local allowlist、message suppress 或按函数名猜测的兼容分支；如果确实是项目扩展，必须有清晰边界。

### 验收标准

- 每个 top call/type pattern 都有 source / compiler / LSP owner 结论。
- 确认接受的项目规则只在共享 type / call 入口实现。
- focused fixture 证明 accepted pattern 不误报，rejected sentinel 仍报错。
- 5-unit / 50-unit audit 中对应 top group 下降或迁移到 source review。
- 公开 diagnostics 行为变化已明确记录。

## Phase 18 (P18): 小型 type / builtin tail 收尾

### 背景

P13 full audit 中仍有少量明确 LSP modeling tail，总量 `29`：

- `Return type mismatch: expected uint64_t but got int`: `26`
- `Binary operator type mismatch`: `1`
- `Indeterminate builtin call: cosh(float)`: `1`
- `Indeterminate builtin call: clip(float)`: `1`

这些问题已经不主导整体趋势，适合在 P14-P17 后作为低风险 focused 收尾处理。

### 目标

补齐少量明确缺失的共享 type / builtin modeling，使剩余 diagnostics 不再被这些小 tail 干扰。

### 方案

1. 在共享 builtin / expression typing 路径补齐 `cosh` 和 `clip`。
2. 在 `diagnostics_expression_type.*` / `type_relation.*` 中补齐 64-bit integer、bitwise shift 和 bitwise-or 表达式类型，例如 `uint64_t(v.y) << 32 | uint64_t(v.x)`。
3. 覆盖 `uint64_t`、signed / unsigned 组合、合法 bitwise expression 和非法 mismatch sentinel。
4. 如果 `Binary operator type mismatch` 单例仍存在，先复核是否由 P15 / P17 解决；未解决再单独补 focused fixture。
5. 只改共享类型 / builtin 入口，不在 return / binary diagnostics 规则里写局部分支。

### 验收标准

- `cosh(float)` 和 `clip(float)` 不再产生 unmodeled indeterminate。
- `uint64_t` shift / or 表达式推断为正确整型结果，合法 return 不再误报。
- 非法 bitwise / return mismatch sentinel 仍保持诊断。
- diagnostics repo focused fixture 通过。
- 如共享 type / builtin 行为变化，已更新头文件契约和相关事实文档。

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
10. Phase 9: diagnostics policy 与 workspace macro 分流。
11. Phase 10: 收敛 common builtin indeterminate modeling。
12. Phase 11: 收敛 macro-expression / parser boundary argument availability。
13. Phase 12: 收敛 literal / macro-like expression typing。
14. Phase 13: P11/P12 后 full audit 和剩余问题分流。
15. Phase 14: active-branch 宏上下文契约与 P14A-L 预处理输入闭环。
16. Phase 15: 多行表达式 continuation 与 statement boundary recovery。
17. Phase 16: statement-like macro 声明局部变量语义边界。
18. Phase 17: call / type policy 与真实编译器行为确认。
19. Phase 18: 小型 type / builtin tail 收尾。

Phase 2、Phase 3、Phase 4、Phase 7、Phase 10、Phase 11、Phase 12、Phase 14、Phase 15 和 Phase 16 是架构治理重点。它们分别对应类型系统、语义作用域、真实编译上下文、diagnostics 发布契约、builtin 类型规则共享入口、macro / parser 参数边界、macro-like expression typing、active-branch 宏上下文契约与共享预处理宏推导核心层、多行 parser continuation 和 statement-like macro local 语义；如果这些阶段不收敛，后续问题会继续以局部补丁形式扩散。Phase 14 不能用 diagnostics-local 特判替代真实宏状态机、真实配置确认或共享快照契约；P14L 的 compiler context 收口必须先区分 preset 漂移、compiler 固定上下文和用户显式配置语义，再决定是否改变公开宏合并契约。Phase 17 仍包含 call / type policy 与真实编译器行为确认重点。

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
