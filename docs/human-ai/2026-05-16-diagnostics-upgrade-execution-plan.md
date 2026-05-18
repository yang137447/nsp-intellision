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

Phase 2、Phase 3、Phase 4、Phase 7、Phase 10、Phase 11 和 Phase 12 是架构治理重点。它们分别对应类型系统、语义作用域、真实编译上下文、diagnostics 发布契约、builtin 类型规则共享入口、macro / parser 参数边界和 macro-like expression typing；如果这些阶段不收敛，后续问题会继续以局部补丁形式扩散。

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
