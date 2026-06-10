# HLSL language/type resource full-coverage execution plan

本文档是 HLSL keyword / type / language fact 全量资源化的可执行方案。目标是把官方 HLSL 语言事实尽量纳入共享资源和 registry，消除 completion、semantic tokens、diagnostics、parser 中散落的本地硬编码表，同时避免把 type 混入 `language/keywords`。

## 总原则

- 一个事实入口只承载一种语义：keyword、scalar type、object type、system semantic、builtin function、object method 分开维护。
- 抓取脚本只负责官方事实和结构化资源生成；产品语义推导仍由共享 registry / type model / diagnostics helper 负责。
- 所有生成脚本必须先写临时文件，运行 schema / coverage 校验，通过后再替换正式资源。
- 不新增 fallback、compat layer 或双写路径；旧硬编码表在共享入口替代后删除。
- 每个里程碑结束都要更新对应事实文档，不能只把结论留在实现里。
- 新增资源 bundle、新增公开 completion / hover / semantic token 行为，或把 effect/state 类型纳入 type model 前，必须先拿到用户确认。

## M0 当前状态基线

### 目标

确认当前资源、脚本和消费层状态，固定后续比较基线。

### 方案

- 记录当前资源入口：
  - `server_cpp/resources/language/keywords`
  - `server_cpp/resources/types/object_types`
  - `server_cpp/resources/types/object_families`
  - `server_cpp/resources/language/semantics`
  - `server_cpp/resources/builtins/intrinsics`
  - `server_cpp/resources/methods/object_methods`
- 记录当前生成脚本：
  - `npm run builtins:update`
  - `npm run keywords:update`
  - `npm run types:update`
- 搜索消费层散落表：
  - `rg -n "typeKeywords|Texture2D|SamplerState|float4|min16|isBlockType|getHlslScalarVectorMatrixTypeNames" server_cpp/src src/test`
- 产出一份 baseline 摘要：资源条目数、硬编码表位置、当前测试状态。

### 验收标准

- 明确列出所有硬编码 type / keyword / object type 表位置。
- 明确哪些资源已经有生成脚本，哪些还没有。
- 本阶段新增 diff 只包含计划/记录文档，除非用户确认开始实现。

### 性能标准

- 本阶段不改变运行时行为。
- 不需要性能测试；只需保证未触发构建产物或资源内容变化。

## M1 官方事实盘点与分类

### 目标

从 Microsoft Learn 和现有资源抽出尽量全的 HLSL language/type 事实，并按现有架构边界分类。

### 方案

- 从 Microsoft Learn HLSL keywords 页面抽取：
  - 普通 language keywords。
  - scalar / vector / matrix type bases 和展开 pattern。
  - object/resource types。
  - effect state / shader stage / stream / patch types。
- 从 Microsoft Learn HLSL semantic 页面或现有 `language/semantics` 资源抽取 system semantics 覆盖差异。
- 从现有 `builtins/intrinsics` 和 `methods/object_methods` 确认 builtin function / object method 已有覆盖范围。
- 形成分类表：
  - `language/keywords`: 控制流、限定符、effect 语法词、非 type reserved keyword。
  - `types/scalar_types`: scalar/vector/matrix 公式型类型。
  - `types/object_types`: texture/buffer/sampler/patch/resource object types。
  - `types/effect_types` 或 `types/object_types`: effect state、shader stage、stream types，需在本阶段做归属决策。
  - `language/semantics`: `SV_*` 和常见 project-facing semantics。

### 验收标准

- 输出分类清单，至少包含：
  - keyword count。
  - scalar/vector/matrix generated count。
  - object/resource type count。
  - effect/state/stream/shader-stage type count。
  - semantic count。
- 对每个分类写清目标资源入口和消费层。
- 明确不进入资源的内容及原因，例如仅是文档 alias、过时语法或当前产品不消费。

### 性能标准

- 本阶段只做盘点，不改变运行时。
- 抓取脚本实验运行单次应在 60 秒内完成；失败时不得写正式资源。

## M2 资源边界与 schema 落地

### 目标

为全量 type coverage 建立清晰资源边界，避免 keyword/type 混淆。

### 方案

- 保持 `language/keywords` 只包含非 type keyword。
- 新增或确认 `types/scalar_types` bundle：
  - 记录 scalar bases。
  - 记录是否可生成 vector / matrix。
  - 记录维度范围，默认 `1..4`。
  - 记录 alias / legacy status，例如 `half`、`min16float`。
- 扩展 `types/object_types` 覆盖：
  - Texture / RWTexture。
  - Buffer / RWBuffer。
  - StructuredBuffer / RWStructuredBuffer。
  - ByteAddressBuffer / RWByteAddressBuffer。
  - SamplerState / SamplerComparisonState。
  - InputPatch / OutputPatch。
  - legacy `texture` / `sampler`。
- 决策 effect/state/stream/shader-stage types：
  - 如果需要参与 completion / hover / semantic token type 分类，则建立 `types/effect_types` 或纳入 object type schema。
  - 如果只作为 reserved keyword，不进入 type model。
- 更新 `resource_registry.*`、`type_model.*` 或新增 `scalar_type_model.*`，使资源可被共享消费。
- 如果新增 `types/scalar_types` 或 `types/effect_types`，先提交 schema / registry 方案说明并等待确认；确认后再进入实现。

### 验收标准

- 所有新增 bundle 都是 `base.json` / `override.json` / `schema.json` 布局。
- `scripts/json/validate_resources.js` 校验新增 bundle。
- 新增或修改 `*.hpp` 时，同步补齐模块职责、public API、缓存语义、调用前提和非目标范围。
- 文档更新：
  - `docs/resources.md`
  - `docs/architecture.md`
  - 必要时 `docs/type-method-interface-contract.md`
- 不存在 type token 被写入 `language/keywords` 的情况。

### 性能标准

- registry 初始化只在资源加载阶段发生，不进入 per-request 热路径重复解析 JSON。
- 新增资源加载不得让 server 启动资源加载时间增加超过 20ms 的常规本机量级；如果无法直接量化，至少用 repo tests 和 server 启动 smoke 确认无明显延迟。

## M3 生成脚本全量化

### 目标

为每个事实入口建立同风格生成脚本，减少手工漏项。

### 方案

- 保留：
  - `npm run builtins:update`
  - `npm run keywords:update`
  - `npm run types:update`
- 新增或调整：
  - `npm run scalar-types:update`: 从官方 keyword 页的 numeric expansion pattern 生成 `types/scalar_types/base.json`。
  - `npm run effect-types:update`: 如果 M2 决定 effect/state/stream/shader-stage type 单独资源化，则生成对应 bundle。
  - `npm run semantics:update`: 如果 M1 发现 `language/semantics` 覆盖不足，则从官方 system semantics 页面生成或补齐。
- 所有脚本统一：
  - `httpGet` retry。
  - 优先 `?accept=text/markdown`。
  - HTML alternate parser 仅用于 Microsoft Learn markdown 端点不可用时的抓取输入替代，不改变运行时资源消费语义。
  - required token coverage。
  - count 下限。
  - 临时文件校验后 rename。
- `package.json` 增加 npm script。
- `docs/resources.md` 记录每个脚本的输入、输出和校验方式。

### 验收标准

- 每个脚本可独立运行，且只更新对应 bundle。
- 脚本失败不会留下 `.tmp` 或半写正式资源。
- `npm run json:validate` 通过。
- `git diff` 可清晰看出每个 bundle 的来源和边界。

### 性能标准

- 单个抓取脚本目标运行时间小于 60 秒。
- 全部语言资源生成脚本串行运行目标小于 5 分钟。
- 资源校验目标小于 10 秒。

## M4 消费层收敛

### 目标

删除 completion、semantic tokens、parser、diagnostics 中散落的本地 type/keyword 表，让共享 registry 成为单一事实来源。

### 方案

- Completion：
  - `getHlslScalarVectorMatrixTypeNames()` 改为消费 scalar type registry。
  - object/effect type completion 改为消费 type model。
- Semantic tokens：
  - 删除 `semantic_tokens.cpp` 中本地 `typeKeywords` 表。
  - 使用 shared type registry 判定 `type`。
  - keyword 判定仍只消费 `language/keywords`。
- Parser / declaration helpers：
  - `extractFxBlockDeclarationHeaderShared`、definition occurrence helper 等 block type 判断改为共享 type-name query。
  - parser 不直接读取 JSON，也不在热路径触发 registry 初始化；如低层 parser 不适合直接依赖 `type_model.*`，新增轻量 `hlsl_type_names.*` / `scalar_type_model.*` 共享入口或由调用方注入预加载 predicate。
- Diagnostics / type_desc：
  - scalar/vector/matrix 识别消费 scalar type registry。
  - object kind 识别消费 type model。
  - legacy `texture` / `sampler` 兼容通过 type model / type relation 表达。
- Hover：
  - type hover 如需新增，走 type registry，不走 keyword hover。

### 验收标准

- `rg` 不再找到新旧并存的本地 type keyword 大表。
- `float4` semantic token 是 `type`，不是 `keyword`。
- `Texture2D` / `SamplerState` semantic token 是 `type`，不是 `keyword`。
- `return` / `if` / `groupshared` semantic token 仍是 `keyword`。
- `texture` / `sampler` legacy 类型仍支持 completion、hover、signature help / diagnostics 相关对象方法路径。
- keyword hover 不再负责 type token。

### 性能标准

- Completion item assembly 不因资源化产生显著回退；repo/perf 中 static item assembly p95 不超过当前基线 20%。
- Semantic tokens 对单文档扫描不做 per-token JSON lookup；type 判断应使用预加载 set/map。
- Parser / diagnostics 热路径不得触发资源加载；资源必须在 registry 初始化后缓存。

## M5 测试覆盖

### 目标

用 repo tests 固化新的资源边界和公开行为。

### 方案

- 新增/扩展 focused tests：
  - keyword completion 包含 `discard`、`groupshared`、`packoffset`。
  - keyword completion 不把 `float4`、`Texture2D`、`SamplerState` 当 keyword 断言。
  - scalar type completion 包含 `float4`、`uint4x4`、`min16float2`。
  - object type completion 包含 `Texture2D`、`SamplerState`、`texture`、`sampler`。
  - semantic tokens 中 scalar/object type 均分类为 `type`。
  - legacy `texture.Sample(...)` / `sampler` 兼容既有 object method diagnostics。
- 新增 script tests 或 resource validation coverage：
  - generated keyword 不含 type leak。
  - scalar generated count 等于公式预期。
  - object type required list 全覆盖。

### 验收标准

- `npm run json:validate` 通过。
- `cmake --build .\server_cpp\build` 通过。
- focused repo tests 通过。
- `npm run test:client:repo` 通过；如出现偶发宿主退出，按 `docs/testing.md` 重跑确认。
- 如果改动 semantic tokens 或 completion item kind，补跑对应 focused semantic-token / completion tests。

### 性能标准

- 新增 tests 不显著拉长 repo suite；focused tests 单文件目标小于 30 秒。
- repo suite 总耗时不超过当前基线 20%；超出时需说明是测试覆盖增加还是产品路径回退。

## M6 真实工程验证

### 目标

确认真实 G66 shader workspace 中关键字/type 缺口收敛，并且没有引入高噪诊断或交互延迟。

### 方案

- 针对真实文件抽样：
  - `shaderlib/deferred_gbuffer_functions.hlsl`
  - 常见 `.nsf` active unit。
  - 包含 legacy `texture` / `sampler` 的 include。
- 验证 completion：
  - 文件开头和函数体内 scalar/object type 候选存在。
  - keyword 候选不混入 type kind。
- 验证 semantic tokens：
  - `float4` / `Texture2D` / `SamplerState` / `texture` 为 type。
  - `return` / `if` / `groupshared` 为 keyword。
- 运行 5-unit smoke diagnostics audit。
- 如涉及 diagnostics/type relation 变化，再跑 50-unit trend audit。

### 验收标准

- 真实文件中本轮关注 token 均可由正确资源入口解释。
- 未新增明显错误 diagnostics top message。
- real audit 的 undefined identifier / call type mismatch 没有因 type 分类变化异常上升。

### 性能标准

- 真实文件 completion / hover 体感无明显延迟。
- 5-unit smoke audit 不新增 timeout / truncated file。
- 如跑 50-unit trend audit，diagnostics 总量和 top canonical messages 的变化必须可解释。

## M7 收尾与文档同步

### 目标

删除旧路径，沉淀维护规则，保证后续不会回到散落补表。

### 方案

- 删除或改造旧硬编码表。
- 更新文档：
  - `README.md`: 如用户可见命令或能力摘要变化。
  - `docs/architecture.md`: 单一事实来源和 request/registry 消费关系变化。
  - `docs/resources.md`: bundle、schema、生成脚本变化。
  - `docs/testing.md`: 新增推荐测试矩阵或坑点。
  - `docs/type-method-interface-contract.md`: 若 type model/object method 契约变化。
- 最终输出关闭检查：
  - 命令是否变化。
  - 路径或命名是否变化。
  - 架构或单一事实来源是否变化。
  - 测试策略是否变化。
  - 文档是否已同步。

### 验收标准

- 所有变更都有对应文档入口。
- `rg` 不存在已废弃局部表。
- 最终报告包含根因、实际改动、为何符合架构、验证、文档更新。

### 性能标准

- `npm run test:client:repo` 通过。
- 涉及 real/workspace 性能路径时，按 M6 完成 smoke/trend 验证。
- 没有新增热路径 fallback、重复查询或 request-time JSON 解析。

## 推荐执行顺序

1. M0/M1：只盘点，不改行为。
2. M2/M3：先建立资源和脚本，跑 `npm run json:validate`。
3. M4：逐个消费层替换硬编码表，每替一个入口跑 focused tests。
4. M5：补测试并跑 repo suite。
5. M6：真实工程 smoke / trend。
6. M7：文档、删除旧路径、最终关闭检查。

## 最小命令集

M3 完成前：

```powershell
npm run keywords:update
npm run types:update
npm run json:validate
cmake --build .\server_cpp\build
npm run test:client:repo
```

M3 新增 scalar type bundle 后：

```powershell
npm run keywords:update
npm run types:update
npm run scalar-types:update
npm run json:validate
cmake --build .\server_cpp\build
npm run test:client:repo
```

如果启用 real diagnostics smoke：

```powershell
$env:NSF_REAL_DIAGNOSTICS_AUDIT = "1"
$env:NSF_REAL_DIAGNOSTICS_MAX_UNITS = "5"
$env:NSF_REAL_DIAGNOSTICS_TIMEOUT_MS = "600000"
$env:NSF_REAL_DIAGNOSTICS_REPORT_LABEL = "phase-hlsl-resource-smoke-5"
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit
```

## 2026-06-09 execution record

### M0/M1 baseline

- 当前资源入口仍为既有 bundle：`language/keywords`、`language/directives`、`language/semantics`、`language/preprocessor_macros`、`types/object_types`、`types/object_families`、`types/type_overrides`、`builtins/intrinsics`、`methods/object_methods`。
- 当前资源计数：`language/keywords` 64 entries，`types/object_types` 28 entries，`types/object_families` 11 families，`language/semantics` 12 entries，`builtins/intrinsics` 139 entries，`methods/object_methods` 13 entries，`language/preprocessor_macros` 170 entries。
- 当前生成脚本：`npm run builtins:update`、`npm run keywords:update`、`npm run types:update`。本轮已验证 `keywords:update` 和 `types:update` 可独立运行，均先写临时文件、调用 focused resource validation，再替换正式资源。
- `keywords:update` 和 `types:update` 的写入路径已收敛为“临时文件写入 -> focused validation -> rename”；validation、rename 或后续异常失败时会清理对应 `.tmp` 文件，避免半写资源残留。
- 当前 `language/keywords` 已排除 `float4`、`Texture2D`、`SamplerState`、`texture` 等 type token；当前 `types/object_types` 已覆盖 texture / buffer / sampler / patch object types，并包含 legacy `texture` / `sampler`。
- `scripts/json/validate_resources.js` 已补强 keyword coverage：代表性 keyword 必须存在，scalar / vector / matrix / object type token 不得进入 `language/keywords`。
- `types/object_families` 已补齐 legacy `texture` / `sampler` family members；`scripts/json/validate_resources.js` 已补强 object type / family 双向一致性校验，要求每个 object type 引用已知 family，且 family members 与 object type 的 `family` 字段一致。
- `scripts/json/validate_resources.js` 已补强重复 name 与 cross-reference 校验：builtins、keywords、directives、semantics、preprocessor macros、object methods 和 object types 不允许重复 entry name；object family `compatibleWith` 必须引用已知 family。
- 仍缺独立 scalar/vector/matrix type 资源入口；`float4`、`uint4x4`、`min16float2` 这类公式型 type 仍主要由 consumer 本地生成或正则识别。

### Hardcoded table inventory

- `server_cpp/src/requests/server_request_handler_completion.cpp`: `getHlslScalarVectorMatrixTypeNames()` 本地枚举 scalar bases 并生成 vector/matrix completion。
- `server_cpp/src/semantic_tokens.cpp`: `typeKeywords` 本地表同时混合 scalar/vector/matrix 与少量 object types。
- `server_cpp/src/callsite_parser.cpp`: 本地 scalar/vector/matrix base 判断用于 callsite/inlay parsing。
- `server_cpp/src/type_desc.cpp`: 本地 alias、object kind、vector/matrix 正则和 object type kind 映射。
- `server_cpp/src/type_relation.cpp` / `server_cpp/src/type_desc.hpp`: `ObjectTypeKind` 目前只表达 `Texture2D` / `SamplerState` 兼容层级，尚未消费完整 `type_model.*` object family。
- `server_cpp/src/server_parse.cpp` 和 `server_cpp/src/app/main_occurrence_helpers.cpp`: `isBlockType` 本地表用于 FX/block declaration 解析。
- `server_cpp/src/inlay_hints_runtime.cpp`: `kBaseTypeCandidates` 本地 object type 候选用于慢路径参数解析。

### M2 recommended boundary

推荐进入 M2 时新增 `types/scalar_types` bundle，而不是把 scalar/vector/matrix 展开写入 `language/keywords` 或塞进 `types/object_types`：

- `types/scalar_types/base.json`: 记录 scalar base、是否生成 vector / matrix、维度范围、legacy/min-precision 标记和文档短描述。
- 新增 `scalar_type_model.*` 或 `hlsl_type_names.*`：server 启动资源加载后构建缓存 set/list，暴露 `isHlslScalarVectorMatrixTypeName(...)`、`getHlslScalarVectorMatrixTypeNames()`、`parseHlslNumericTypeShape(...)` 这类 consumer-ready API。
- `type_model.*` 继续只负责 object/resource type family、坐标维度和 object method 相关查询；不把 numeric scalar/vector/matrix 语义塞入 object type model。
- effect/state/stream/shader-stage tokens 暂不纳入 type model；本轮保持它们作为 reserved keyword，除非后续明确要让它们参与 type completion / hover / semantic-token type 分类。

建议 `types/scalar_types/schema.json` 字段：

- `name`: scalar base token，例如 `float`、`uint`、`min16float`。
- `kind`: `bool` / `integer` / `floating` / `special`，用于 diagnostics/type relation 后续消费。
- `generateVector`: 是否生成 `T1..T4`。
- `generateMatrix`: 是否生成 `T1x1..T4x4`。
- `vectorDimensions`: 默认 `[1, 2, 3, 4]`。
- `matrixRows` / `matrixColumns`: 默认 `[1, 2, 3, 4]`。
- `legacy`: 是否为 legacy / compatibility 类型。
- `minPrecision`: 是否为 min precision 类型。
- `documentation`: 简短说明，供后续 type hover 或 docs 使用。

建议首批 base entries：`bool`、`int`、`uint`、`dword`、`half`、`float`、`double`、`min16float`、`min10float`、`min16int`、`min12int`、`min16uint`、`matrix`、`vector`、`void`、`string`。其中 `void` / `string` / `matrix` / `vector` 不生成 vector/matrix 展开；`bool` 是否生成 matrix 需按官方事实和现有 parser/diagnostics 行为确认后再定。

建议 public API：

- `bool isScalarTypeModelAvailable()` / `const std::string &getScalarTypeModelError()`。
- `const std::vector<std::string> &getHlslScalarTypeBaseNames()`。
- `const std::vector<std::string> &getHlslScalarVectorMatrixTypeNames()`。
- `bool isHlslScalarVectorMatrixTypeName(const std::string &typeName)`。
- `bool parseHlslScalarVectorMatrixTypeShape(const std::string &typeName, HlslScalarTypeShape &outShape)`。

API 使用边界：

- completion 只消费 generated names。
- semantic tokens / parser declaration helper 只消费 type-name predicate。
- diagnostics/type_desc 消费 parsed shape，避免重复正则和 base list。
- object/resource type 仍走 `type_model.*`；共享 `hlsl_type_names.*` 如果存在，只组合 scalar model + object type model，不拥有资源加载。

触发确认项：新增 `types/scalar_types` bundle 会改变资源路径和单一事实来源；后续 M4/M5 还会改变 completion / semantic tokens 等公开行为。因此继续实现前需要确认该 M2 边界。

### Verification run

- `npm run keywords:update`
- `npm run types:update`
- `npm run json:validate`
- `Get-ChildItem -Path server_cpp\resources -Recurse -Filter *.tmp`

### M2/M4 implementation record

- 新增 `server_cpp/resources/types/scalar_types/` bundle，保持 `base.json` / `override.json` / `schema.json` 布局。首批 base entries 为 `bool`、`double`、`dword`、`float`、`half`、`int`、`matrix`、`min10float`、`min12int`、`min16float`、`min16int`、`min16uint`、`string`、`uint`、`vector`、`void`。
- 新增 `scripts/language/update_hlsl_scalar_types.js` 和 `npm run scalar-types:update`。脚本从 Microsoft Learn HLSL keywords 页面抽取官方 base token，并用 schema / coverage validation 后再替换正式资源；`half` / `double` 的 vector/matrix 展开资格记录为脚本内 curated evidence，因为官方 expansion 段当前只列部分 numeric base。
- 新增 `server_cpp/src/scalar_type_model.*`，通过 `resource_registry.*` 加载 scalar type bundle，并缓存 base names、generated names 和 parsed shape。`type_model.*` 继续只负责 object/resource type family 与对象方法相关语义。
- `document_runtime.*` 的 resource model hash 已纳入 `types/scalar_types`，资源变化会参与 analysis key / cache 失效。
- completion 删除本地 scalar/vector/matrix 生成表，改为消费 `getHlslScalarVectorMatrixTypeNames()`。
- semantic tokens 删除本地 `typeKeywords` 表，改为组合 `scalar_type_model.*` 与 `type_model.*` 判定 type token。
- callsite parser 删除本地 scalar base / digit suffix 逻辑，构造调用识别改为消费 scalar shape，并排除 `void` / `string` / generic `matrix` / `vector` 这类 special base。
- `type_desc.*` 删除 vector/matrix regex base list，改为通过 scalar shape 解析 numeric/bool scalar/vector/matrix；`float1` 等 1 维 vector 在 `TypeDesc` 中按 scalar 归一，matrix canonical string 保留 `1..4` 维。
- `diagnostics_expression_type.*` 的 scalar/vector/matrix helper 改为消费 scalar model；`int64_t` / `uint64_t` 仍保留为实现层项目 64-bit alias，没有提升到资源。
- `type_relation.*` 补齐 min precision / `dword` base 的 numeric family 判定，避免 `type_desc.*` 识别出新资源类型后关系层仍把它们当 unknown。
- 事实文档已同步：`docs/resources.md`、`docs/architecture.md`、`docs/testing.md`、`docs/type-method-interface-contract.md`。

### M2/M4 verification

- `npm run scalar-types:update`
- `npm run json:validate`
- `cmake --build .\server_cpp\build`
- `npm run test:client:repo`

备注：第一次 `npm run test:client:repo` 在 5 分钟工具预算内超时且无失败摘要；按 `docs/testing.md` 偶发/时长限制处理后，第二次以更长预算完整通过，命令退出码为 0。VS Code 启动日志中出现 `Error mutex already exists` 噪声，但未导致测试失败。

### M5 focused coverage record

- 扩展 `src/test/suite/integration/interactive-core.ts` 的基础 completion 用例，断言 keyword 候选包含 `discard`、`groupshared`、`packoffset`，scalar/vector/matrix type 候选包含 `float4`、`uint4x4`、`min16float2`，object/legacy type 候选包含 `Texture2D`、`SamplerState`、`texture`、`sampler`。
- 扩展 `src/test/suite/integration/deferred-doc.ts` 的 semantic token role 用例，断言 `float4` / `float2` / `Texture2D` / `SamplerState` 分类为 `type`，并断言 `groupshared` / `if` / `return` 仍分类为 `keyword`。
- 扩展 `test_files/module_semantic_token_roles.nsf` 和 `.hlsl`，为 semantic token focused coverage 提供 object type、groupshared 和 branch keyword 样例。

### M5 verification

- `npm run compile`
- `node .\out\test\runCodeTests.js --mode repo --file-filter client.interactive-runtime`
- `node .\out\test\runCodeTests.js --mode repo --file-filter client.deferred-doc-runtime`
- `npm run test:client:repo`

### M6 real workspace verification

- 5-unit smoke:
  - Command: `NSF_REAL_DIAGNOSTICS_AUDIT=1 NSF_REAL_DIAGNOSTICS_MAX_UNITS=5 NSF_REAL_DIAGNOSTICS_TIMEOUT_MS=600000 NSF_REAL_DIAGNOSTICS_REPORT_LABEL=phase-hlsl-resource-smoke-5 node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.diagnostics-audit`
  - Result: passed, `5` units scanned, `59` files scanned, diagnostics `199`, wait timeouts `0`, truncated files `0`, timed-out file builds `0`.
  - Report: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-hlsl-resource-smoke-5.md`.
- 50-unit trend:
  - First run with the old `1800000ms` budget timed out after scanning `25/50` units. This was a validation budget issue, not an assertion failure.
  - Rerun command used `NSF_REAL_DIAGNOSTICS_TIMEOUT_MS=4200000`.
  - Result: passed, `50` units scanned, `119` files scanned, diagnostics `1604`, wait timeouts `0`, truncated files `0`, timed-out file builds `0`.
  - Baseline trend summary: diagnostics total `43341 -> 1604` (`-96.30%`), files with diagnostics `78 -> 8`, preprocessor-context diagnostics `4004 -> 0`, expression-type-analysis `23841 -> 50`, call-type-analysis `3337 -> 161`.
  - Report: `out/test/diagnostics-audit/real-workspace-diagnostics-audit.phase-hlsl-resource-trend-50.md`.
- `docs/testing.md` 已把 50-unit trend 推荐 timeout 从 `1800000` 更新为 `4200000`，避免当前 G66 workspace 上重复触发预算不足。

## 2026-06-10 hover/type-token defect audit

### Trigger

真实 workspace 中用户在 `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\uber_fx_common.nsf` hover 类型 token 时发现两类异常：

- hover `half3` 时显示 `groupshared half3` / `Type: groupshared`，并跳到 `common_pipeline\wave_particles_vert_blur_cs.nsf:33`。
- hover `float4` 时显示 `Texture2D float4` / `Type: Texture2D`，并跳到 `common_pipeline\add_surfel_to_scene.nsf:53` 这类 `Texture2D<float4>` 声明。

这不是 HLSL 语义预期。`half3` / `float4` 是 scalar/vector type token，不应被当作普通 symbol definition；`groupshared` 是 storage-class keyword，不是类型；`Texture2D<float4>` 中的 `float4` 是对象模板元素类型，不是变量名。

### Real workspace scan result

`uber_fx_common.nsf` 本文件包含 9 类 scalar/vector type token。与真实 workspace 中当前可触发误索引的声明形态求交后，风险如下：

- `groupshared` 声明误索引影响：
  - `float2`: `uber_fx_common.nsf` 50 occurrences；首个假定义来源 `common_pipeline\frame_prediction_reconstruct_screen_mesh.nsf:21`
  - `half`: 23 occurrences；首个假定义来源 `common_pipeline\voxel_ddgi_gather_radiance_cs.nsf:293`
  - `float`: 13 occurrences；首个假定义来源 `common_pipeline\build_hierarchical_surfel_lumin_cs.nsf:69`
  - `half4`: 11 occurrences；首个假定义来源 `common_pipeline\voxel_ddgi_gather_radiance_cs.nsf:292`
  - `half3`: 10 occurrences；假定义来源 `common_pipeline\wave_particles_vert_blur_cs.nsf:33`
  - `float3`: 3 occurrences；首个假定义来源 `common_pipeline\atmosphere_multi_scattering_lut_cs.nsf:28`
  - `half2`: 1 occurrence；假定义来源 `common_pipeline\wave_particles_vert_blur_cs.nsf:34`
- object template 声明误索引影响：
  - `float4`: `uber_fx_common.nsf` 47 occurrences；template sites 139；首个假定义来源 `common_pipeline\add_surfel_to_scene.nsf:53`
  - `float`: 13 occurrences；template sites 20；首个假定义来源 `common_pipeline\build_hierarchical_surfel_lumin_cs.nsf:31`
  - `half4`: 11 occurrences；template sites 7；首个假定义来源 `common_pipeline\clear_buffer.nsf:28`
  - `float3`: 3 occurrences；template sites 6；首个假定义来源 `common_pipeline\atmosphere_multi_scattering_lut_cs.nsf:3`

代表性源代码：

```hlsl
// common_pipeline/wave_particles_vert_blur_cs.nsf:33
groupshared half3 gCache[BLOCK_SIZE_YV][BLOCK_SIZE_XV + 2 * MAX_PIXR];

// common_pipeline/add_surfel_to_scene.nsf:53
Texture2D<float4> t_surfel_diffuse_atlas : Texture0;

// sfx/uber_fx_common.nsf:242
half3 new_basecolor = lerp(...);
```

### Root cause

当前 hover / definition fallback 暴露出两个系统性问题：

1. `workspace_index_extract.cpp` 维护了一套独立的简易 declaration scanner，用“第一个非 qualifier identifier 是 type、下一个 identifier 是 name”推断定义。它不完整理解 HLSL storage qualifiers 和 object template angle-depth，因此会把：
   - `groupshared half3 gCache` 误记录为 `name=half3, type=groupshared`
   - `Texture2D<float4> tex` 误记录为 `name=float4, type=Texture2D`
2. `server_parse.cpp` / `workspace_index_extract.cpp` / `type_desc.cpp` 存在多套 qualifier 判断。`groupshared` 已进入 `language/keywords`，但不是所有 declaration/type helper 都把它视为 storage qualifier，导致 completion / semantic tokens 正确而 hover / workspace index 错误。
3. hover 对当前 word 没有先判断“这是 type token 本身”，而是在普通 symbol definition fallback 中继续查 workspace summary。只要 workspace index 里已有假 definition，类型 token hover 就会跳到无关文件。

这属于 resource/type 收敛后暴露出的剩余边界问题：scalar/object type facts 已有共享入口，但 declaration parsing、workspace indexing 和 hover fallback 尚未完全消费同一套事实。

## M8 perfect solution: type-token hover and declaration-index convergence

### Goal

让 type token、storage qualifier、object template declaration 在 hover、definition、workspace summary 和 semantic consumers 中使用同一套共享事实，彻底避免把 HLSL 类型名当普通 symbol，且不新增 fallback / compat layer / 双路径。

目标行为：

- hover `half3` / `float4` / `uint4x4` 这类 scalar/vector/matrix token 时，渲染为 HLSL type token，不查 workspace definition。
- hover `Texture2D` / `SamplerState` / `Buffer` 等 object type token 时，渲染为 HLSL object type token，不查 workspace definition。
- hover `groupshared` 时走 keyword hover；`groupshared` 不作为 type 出现在任何 declaration/type result 中。
- `groupshared half3 gCache[...]` 索引为 `name=gCache, type=half3[]...` 或至少 `name=gCache, type=half3`，不产生 `name=half3`。
- `Texture2D<float4> tex` 索引为 `name=tex, type=Texture2D`，并保留 display type `Texture2D<float4>` 供 hover 展示；不产生 `name=float4`。
- object method / diagnostics 仍消费 normalized base object type，例如 `Texture2D`，不把 `Texture2D<float4>` 当新的 object family。

### Architecture changes

1. Introduce one shared declaration-type parser contract.
   - Keep `extractDeclarationsInLineShared(...)` as the single parser-facing entry.
   - Extend `ParsedDeclarationInfo` with optional `displayType` and, if needed, `normalizedType`.
   - `type` remains consumer-ready normalized semantic type.
   - `displayType` preserves source spelling such as `Texture2D<float4>` for hover code block rendering.
   - The parser must understand top-level `,` / `;`, `: semantic`, `= initializer`, `[]` declarator suffixes, `<...>` object template arguments, and storage/type qualifiers.

2. Centralize qualifier facts.
   - `nsf_lexer.*` owns `isQualifierToken(...)` as the shared lexical qualifier predicate.
   - Add `groupshared` and any confirmed HLSL storage/interpolation qualifiers used by declaration parsing.
   - Remove or rename local qualifier tables in `type_desc.cpp` and other consumers where possible; if a consumer needs a narrower predicate, name it by its narrower semantics so it cannot drift silently.
   - Do not move qualifiers into `types/scalar_types`; storage qualifiers remain language keywords / declaration modifiers.

3. Make workspace index consume shared declaration parsing.
   - Replace the local top-level variable extraction in `workspace_index_extract.cpp` with `extractDeclarationsInLineShared(...)`.
   - For each parsed declaration, record `decl.name` and normalized `decl.type`.
   - For cbuffer/member extraction, use the same shared declaration result so array suffix and template angle-depth behave identically across workspace summary and current-doc semantic snapshot.
   - Continue using explicit aggregate/macro extraction for `struct` / `cbuffer` / `#define`; only declaration-shaped variable/function parsing is unified.

4. Add type-token hover short-circuit.
   - In `server_request_handler_hover.cpp`, before workspace definition fallback, check:
     - `isHlslScalarVectorMatrixTypeName(word)`
     - `isHlslScalarTypeBaseName(word)`
     - `getTypeModelObjectFamily(word, family)`
   - If matched, render a type hover from `scalar_type_model.*` or `type_model.*`.
   - This type hover must not attach `Defined at` from workspace summary.
   - Keyword hover remains separate: `groupshared` still renders as keyword, not type.

5. Normalize object template type spelling.
   - Parse `Texture2D<float4> t` as:
     - normalized semantic type: `Texture2D`
     - display type: `Texture2D<float4>`
     - template argument metadata: optional future field, not required for this fix unless diagnostics later need it.
   - Parse `Buffer<uint> b` / `RWTexture2D<float4> outTex` with the same rule.
   - Do not add `Texture2D<float4>` entries to `types/object_types`; object family remains keyed by base type.

6. Clean stale index effects by rebuild, not by compatibility code.
   - After the implementation, run real workspace index rebuild or tests with fresh user-data/cache.
   - Do not add runtime filters that hide bad cached definitions forever; fix extraction and let index rebuild produce correct facts.

### Implementation steps

1. Update `nsf_lexer.*`
   - Add `groupshared` to `isQualifierToken(...)`.
   - Audit current qualifier list against HLSL storage/interpolation modifiers used in the codebase.

2. Update `server_parse.*`
   - Extend `ParsedDeclarationInfo` fields and header comments.
   - Teach `findBaseTypeBeforeToken(...)` or its replacement to collect object template spelling while returning normalized object base type.
   - Ensure `<...>` contents never become declarator candidates.
   - Keep array suffix handling after the declarator, not after the type token.

3. Update `workspace_index_extract.cpp`
   - Remove local type/name token scanner for top-level declarations and cbuffer members.
   - Reuse `extractDeclarationsInLineShared(...)`.
   - Preserve existing special handling for `#define`, struct/cbuffer declarations, technique/pass, metadata declarations and FX block declarations.

4. Update hover rendering path.
   - Add a small helper such as `tryRenderHlslTypeTokenHover(...)` in `server_request_handler_hover.cpp` or a dedicated hover helper.
   - Render scalar/vector/matrix type shape from `scalar_type_model.*`.
   - Render object type family from `type_model.*`.
   - Use `displayType` for declaration hover code blocks when present.

5. Update tests and fixtures.
   - Add fixture lines:
     - `groupshared half3 SharedHalf3Cache[4];`
     - `Texture2D<float4> TemplateTexture;`
     - `Buffer<uint> TemplateBuffer;`
     - local `half3 new_basecolor = 0;`
   - Add hover assertions:
     - hover over `half3` type token does not contain `Type: groupshared`
     - hover over `half3` type token does not contain unrelated `Defined at`
     - hover over `SharedHalf3Cache` shows type `half3` (array suffix acceptable if current contract preserves it)
     - hover over `float4` inside `Texture2D<float4>` does not report `Type: Texture2D`
     - hover over `TemplateTexture` code block shows `Texture2D<float4> TemplateTexture` and semantic type/family remains `Texture2D`
   - Add workspace summary/index assertion if a helper exists; otherwise verify through hover/definition behavior.

6. Update docs only if implementation changes public behavior.
   - `docs/architecture.md`: declaration parser / workspace index now share type parsing contract.
   - `docs/testing.md`: record the hover/type-token regression test requirement.
   - `docs/type-method-interface-contract.md`: if `displayType` vs normalized object base type becomes an explicit object template contract.

### Acceptance tests

Required commands after implementation:

```powershell
npm run compile
cmake --build .\server_cpp\build
node .\out\test\runCodeTests.js --mode repo --file-filter client.interactive-runtime
node .\out\test\runCodeTests.js --mode repo --file-filter client.deferred-doc-runtime
npm run test:client:repo
```

Real workspace smoke:

```powershell
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.smoke
```

Before release:

```powershell
npm run gate:d3
```

Optional targeted manual checks in `uber_fx_common.nsf`:

- hover `half3` on `half3 new_basecolor` at line 242.
- hover `float4` on representative type tokens.
- hover `Texture2D` / `float4` in a `Texture2D<float4>` declaration such as `common_pipeline\add_surfel_to_scene.nsf:53`.
- hover `gCache` in `common_pipeline\wave_particles_vert_blur_cs.nsf:33`.

### Non-goals

- Do not model platform-specific groupshared packing/alignment behavior in hover. The source type remains `half3`; any backend alignment to `float4`-like storage is outside this LSP type-token fix.
- Do not add `Texture2D<float4>` as a separate object type resource.
- Do not hide bad workspace definitions with ad hoc hover filters while leaving workspace extraction wrong.
- Do not add compatibility paths for old index cache format beyond existing cache invalidation/rebuild behavior.

### Confirmation note

This fix changes public hover behavior and workspace definition results for type tokens and malformed indexed declarations. Under `AGENTS.md`, implementation requires explicit user confirmation before code changes.

### 2026-06-10 confirmation and implementation sync

用户已确认继续修复该公开 hover / definition 行为问题。本节把 M8 从方案态同步为可执行关闭方案，后续验收以这里为准。

#### Final solution

完美解法不是在 hover 层过滤几个坏结果，而是让“类型事实、声明解析、workspace 索引、hover fallback”共享同一条语义链：

1. Type token 先被识别为语言类型事实。
   - `half3` / `float4` / `uint4x4` 这类 scalar/vector/matrix token 由 `scalar_type_model.*` 识别。
   - `Texture2D` / `SamplerState` / `Buffer` 这类 object type token 由 `type_model.*` 识别。
   - 类型 token hover 在进入 workspace symbol fallback 前短路，渲染为 `(HLSL type)` 或 `(HLSL object type)`，不附带 `Defined at`。
   - `groupshared` 仍走 keyword hover，不进入 type model。

2. 声明解析统一收敛到 `server_parse.*`。
   - `ParsedDeclarationInfo` 保留 normalized semantic `type`，并新增 source-facing `displayType`。
   - `Texture2D<float4> tex` 的 normalized type 是 `Texture2D`，display type 是 `Texture2D<float4>`。
   - `<...>` 内的 template argument 不参与 declarator name 识别，因此 `float4` 不会被索引成变量名。
   - declarator array suffix 仍归到声明名一侧，避免数组、矩阵和 object template 混淆。

3. qualifier 事实补齐并避免漂移。
   - `groupshared` 明确作为 storage qualifier 参与声明解析，不能被当作 type。
   - 后续若还发现 HLSL storage / interpolation qualifier 漂移，应优先补共享 qualifier predicate，而不是在 workspace index 或 hover 层局部特判。

4. workspace index 不再维护独立变量声明扫描器。
   - 顶层变量和 cbuffer member 提取消费 `extractDeclarationsInLineShared(...)`。
   - `groupshared half3 gCache[...]` 只产生 `gCache` 定义，type 为 `half3` 或带数组 suffix 的 consumer-ready type；不产生 `half3` 定义。
   - `Texture2D<float4> tex` 只产生 `tex` 定义，type 为 `Texture2D`，display type 仅用于 hover 展示；不产生 `float4` 定义。
   - FX/resource block 既有专用识别保留，但与共享声明解析去重，避免同一行重复记录。

5. hover 展示同时满足用户可读和语义稳定。
   - hover 变量名时，code block 优先使用 `displayType`，例如 `Texture2D<float4> SuiteTemplateTex`。
   - hover 语义类型行仍使用 normalized `Type: Texture2D`，保证 object method、diagnostics 和 family query 继续消费稳定 base type。
   - current-doc、interactive definition 和 workspace definition fallback 都使用同一个 display/normalized contract。

6. 清理方式是重建正确索引，而不是兼容旧错索引。
   - 不新增 cache compatibility、runtime hide filter、双写路径或 fallback。
   - 实现完成后通过 repo tests / real smoke 使用新构建产物和新 workspace summary 验证。

#### Concrete implementation targets

- `server_cpp/src/server_parse.hpp`
  - `ParsedDeclarationInfo` 增加 `displayType`。
  - 暴露 `findDisplayTypeOfIdentifierInDeclarationLineShared(...)`。
- `server_cpp/src/server_parse.cpp`
  - object template declaration 解析返回 normalized `type` 与 source-facing `displayType`。
  - `extractFxBlockDeclarationHeaderShared(...)` 的 block type 判断消费 `type_model.*`。
- `server_cpp/src/nsf_lexer.cpp`
  - `groupshared` 进入 qualifier predicate。
- `server_cpp/src/type_desc.cpp`
  - 同步 qualifier 处理，避免 expression/type helper 把 storage qualifier 当 type。
- `server_cpp/src/workspace/workspace_index_extract.cpp`
  - 顶层变量和 cbuffer member 提取消费 shared declaration parser。
  - 跳过已由 FX block 专用路径记录的同名 declaration。
- `server_cpp/src/requests/server_request_handler_hover.cpp`
  - type-token hover short-circuit 位于 keyword hover 之后、builtin / workspace fallback 之前。
  - declaration hover 使用 `displayType` 展示，使用 normalized `type` 作为语义类型。
- `test_files/module_decls.nsf`
  - 增加 object template 和 `groupshared half3` 回归夹具。
- `src/test/suite/integration/interactive-core.ts`
  - 增加 type-token hover 不被 workspace symbol 污染的回归。
  - 增加 object template display type 与 normalized type 并存的回归。

#### Required regression assertions

- hover `half3` type token:
  - contains `(HLSL type)`
  - does not contain `Type: groupshared`
  - does not contain unrelated workspace `Defined at`
- hover `float4` inside or near object template declarations:
  - does not contain `Texture2D float4`
  - does not contain `Type: Texture2D` unless the hovered symbol is the object variable itself
- hover `SuiteTemplateTex`:
  - code block uses `Texture2D<float4> SuiteTemplateTex`
  - semantic type remains `Type: Texture2D`
- hover `SuiteSharedHalf3Cache`:
  - symbol name is the cache variable, not `half3`
  - type is `half3` or the current declaration parser's array-suffix-preserving equivalent
- workspace definition / hover fallback:
  - no fake symbol named `half3` from `groupshared half3 ...`
  - no fake symbol named `float4` from `Texture2D<float4> ...`

#### Verification matrix

Minimum after code changes:

```powershell
cmake --build .\server_cpp\build
npm run compile
node .\out\test\runCodeTests.js --mode repo --file-filter client.interactive-runtime
node .\out\test\runCodeTests.js --mode repo --file-filter client.deferred-doc-runtime
```

Repository confidence:

```powershell
npm run test:client:repo
```

Performance / release confidence:

```powershell
npm run test:client:perf
npm run gate:d3
```

Real workspace smoke for the original report:

```powershell
node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.smoke
```

Manual probes in `uber_fx_common.nsf`:

- `sfx\uber_fx_common.nsf`: hover `half3` at `half3 new_basecolor`.
- `sfx\uber_fx_common.nsf`: hover representative `float4` type tokens.
- `common_pipeline\wave_particles_vert_blur_cs.nsf:33`: hover `half3` and `gCache`.
- `common_pipeline\add_surfel_to_scene.nsf:53`: hover `Texture2D`, `float4`, and the declared texture name.

#### Close criteria

M8 can be closed only when:

- repo focused interactive hover tests pass,
- deferred/runtime tests pass if shared parser or semantic consumers were touched,
- `npm run test:client:repo` passes,
- performance test passes when hover/workspace fallback hot path or indexing behavior changed,
- `npm run gate:d3` passes before claiming release/package-ready state,
- docs are synchronized:
  - `docs/architecture.md` records shared declaration parser / scalar type model relationship,
  - `docs/testing.md` records the necessary validation matrix,
  - `docs/type-method-interface-contract.md` remains clear that object template display type is not a new object type resource.

#### 2026-06-10 close sync

M8 has been implemented and validated against the required closure path.

Actual fixes:

- `server_parse.*` now exposes normalized declaration `type` plus source-facing `displayType`.
- Object template declarations such as `Texture2D<float4> tex` keep `Texture2D` as the semantic type and `Texture2D<float4>` as display spelling.
- FX block declaration scanning skips `<...>` contents and consumes shared type facts instead of local block-type tables.
- `groupshared` is treated as a storage qualifier in shared qualifier/type parsing.
- Workspace index variable/member extraction consumes `extractDeclarationsInLineShared(...)` instead of maintaining an independent type/name scanner.
- Workspace index cache semantic version was bumped to invalidate previously bad indexed facts.
- Hover now short-circuits known scalar/vector/matrix and object type tokens before builtin/workspace symbol fallback.
- Declaration hover renders `displayType` in code blocks while preserving normalized `Type:` output for semantic consumers.
- Regression fixtures and integration tests cover storage-qualified declarations, object templates, type-token hover, and fake-symbol absence in workspace debug data.
- `hover_smoke_test.py` now waits long enough for deferred diagnostics in clean gate runs, with `NSF_HOVER_SMOKE_WAIT_SECONDS` as an override.

Validation completed:

```powershell
cmake --build .\server_cpp\build
npm run compile
node .\out\test\runCodeTests.js --mode repo --file-filter client.interactive-runtime
node .\out\test\runCodeTests.js --mode repo --file-filter client.deferred-doc-runtime
npm run test:client:repo
py -3 .\server_cpp\tools\hover_smoke_test.py
npm run gate:d3
npm run test:client:perf
npm run package:vsix
```

Observed results:

- Focused interactive runtime passed with the new M8 hover/workspace-index assertions.
- Focused deferred-doc runtime passed after restart/indexing readiness waits were stabilized.
- Repository integration suite passed.
- `npm run gate:d3` passed after the smoke diagnostic wait fix.
- `npm run test:client:perf` passed with `26 passing`, `3 pending`.
- `npm run package:vsix` produced `nsf-lsp-1.0.1.vsix`.

Release/package state:

- The M8 fix is in a release-ready validation state from gate, performance and package evidence.
- The generated package is `D:\YYBWorkSpace\GitHub\nsp-intellision\nsf-lsp-1.0.1.vsix`.
- Remaining release process is ordinary review/versioning/staging, not a known technical blocker from this fix.

#### Client grammar / server keyword boundary

Follow-up boundary check:

- `syntaxes/hlsl.tmGrammar.json` is a client-side TextMate grammar. It gives VS Code immediate regex-based baseline coloring when a file opens.
- TextMate scopes are not semantic truth. They do not understand include closure, macro state, declaration parsing, workspace symbols, or type inference.
- HLSL keyword facts remain server-owned in `server_cpp/resources/language/keywords` and are consumed through `language_registry.*`.
- Server keyword/type resources drive semantic tokens, hover, completion, diagnostics and workspace index behavior.
- Keeping keyword semantics in server resources does not make first-paint coloring slow, because TextMate coloring is available before semantic tokens arrive.
- If `hlsl.tmGrammar.json` contains keyword/type regexes, they are only an editor-color fallback and may be approximate; they must not become a second source of truth.

Fact docs synchronized:

- `docs/client-editor-features.md` now records that TextMate grammar is client shell behavior and not the keyword/type semantic authority.
