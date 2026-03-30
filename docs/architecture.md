# 架构总览

本文档描述当前仓库的实际结构和单一事实来源。它不是规划稿，而是给维护者和 AI 用的当前事实说明。

如果任务是在讨论“如何提升编辑态即时反馈 / per-keystroke feedback”，请同时参考 `docs/human-ai/realtime-feedback-design.md`。注意：该文位于 human-ai 协作区，属于目标架构提案，不是当前事实。

如果任务涉及对象类型、对象方法、array texture 坐标规则，或 `label: Type name` / `label: expr` 这类参数标签语法，请同时参考 `docs/type-method-interface-contract.md`。注意：该文是当前事实文档，定义该链路的共享契约，并记录已确认的当前偏差。

## 顶层结构

- `client/`: VS Code 扩展客户端，负责启动语言服务、同步配置、消费 LSP 能力
- `server_cpp/`: C++ 语言服务端，负责解析、索引、诊断、悬停、补全、签名帮助、语义高亮
- `server_cpp/resources/`: 服务端运行时资源 bundle
- `src/test/`: VS Code 集成测试入口
- `test_files/`: 仓库模式测试夹具
- `scripts/`: 门禁脚本、资源生成脚本、资源校验脚本
- `docs/human-ai/`: 人类与 AI 协作设计稿、任务背景、方案权衡与共享 skill 沉淀；不属于当前事实文档

## 模块职责与头文件契约

- 当前仓库默认依赖模块边界 + `*.hpp` 头文件契约来帮助维护者和 AI 理解代码，不应只靠 `*.cpp` 实现细节反推接口意图。
- 新增或拆分模块/类时，优先保持单一职责；一个类型/模块只承载一个主要责任，避免把请求编排、状态所有权、缓存管理、资源加载、文本渲染等多种主职责长期堆叠在同一入口。
- 对历史原因形成的大文件，当前事实不是“必须立即重构”，但后续增量修改时不应继续扩大职责范围；更推荐沿现有共享入口做最小必要的职责下沉。
- 对外暴露或跨文件复用的 `*.hpp` 应至少说明：模块/类职责、关键输入输出、持有状态或缓存、主要 public API、调用前提，以及明确的非职责范围。
- 当实现行为依赖特殊约束时，例如 latest-only、last-good snapshot、schema 校验、workspace summary 封装边界，这些约束应优先写在对应头文件说明中，避免调用方只能从实现推断。

## 接口分层视图

当前仓库里的“接口”不是只有一种形态。单一职责的含义是“一个模块只负责一类主问题”，不是“一个模块只能有一种接口类型”。

当前推荐按下面几层理解接口：

| 层次 | 主要载体 | 面向对象 | 负责什么 | 典型例子 |
| --- | --- | --- | --- | --- |
| 资源/数据接口 | `resources/*`、schema、静态配置 | 资源维护者、生成脚本、加载器 | 定义原始数据结构与字段语义 | `server_cpp/resources/types/*` |
| 共享模块 API 接口 | `*.hpp` public API | 其他 C++ 模块 | 暴露跨模块调用入口与调用前提 | `type_model.hpp`、`language_registry.hpp` |
| consumer-ready 语义接口 | 共享模块返回值与查询语义 | hover / completion / diagnostics 等 consumer | 提供“拿来就能用”的共享判定，不要求调用方再解释底层字段 | `getTypeModelSampleCoordDim(...)` |
| 请求/渲染接口 | request handler、rendering/runtime 入口 | LSP 请求编排层、编辑器反馈层 | 组合共享语义，输出 LSP 结果、文案和诊断 | `server_request_handlers.*`、`hover_markdown.*` |
| 事实/验证接口 | 当前事实文档、测试夹具、集成断言 | 维护者、AI、reviewer | 说明当前契约并把它制度化、可回归验证 | `README.md`、`docs/{architecture,resources,testing}.md`、`docs/type-method-interface-contract.md`、`src/test/suite/*` |

约束：

- 能留在资源层的问题，不要提前下沉到 request 层。
- 能由共享模块统一回答的问题，不要让 consumer 重复解释。
- request 层应优先消费 consumer-ready 语义接口，而不是直接读取底层资源字段。
- 文档与测试不是实现细节，但它们也是接口的一部分，因为它们定义了“当前仓库承诺什么”。

判断一个模块是否健康时，优先看：

1. 是否仍然只有一个主责任
2. 该主责任下的不同接口层次是否清晰
3. 下游是否绕过共享接口直接读更底层的表示

## 请求链路

1. VS Code 激活 `client/out/extension`
2. 客户端优先使用内置 server；如果配置了高级设置 `nsf.serverPath`，则用它覆盖默认路径
3. 客户端用 stdio 启动 C++ server
4. server 在 `server_cpp/src/requests/server_request_handlers.cpp` 中分发 LSP 请求
5. 具体能力通过共享模块和 registry 提供数据与判定
6. 结果再由客户端呈现给编辑器

## 客户端职责

当前客户端事实位于 `client/src/extension.ts`：

- 启动/重启 C++ server
- 同步配置项，如 `nsf.intellisionPath`、defines、diagnostics、inlay hints、semantic tokens、metrics
- 管理状态栏、trace 输出、诊断和索引状态展示
- 在测试模式下按固定方式启动语言客户端
- 当前 `extension.ts` 主要承担 activate / lifecycle wiring 与高层装配；LanguageClient 启动 / 重启 / `onReady` lifecycle 已下沉到 `client_runtime_host.*`；active unit 状态与 `selectUnit` 逻辑已下沉到 `client_active_unit.*`；editor feedback / indexing / inlay provider 主体已下沉到 `client_editor_feedback.*`；editor/document 事件注册已下沉到 `client_editor_events.*`；runtime config / git-storm watcher / 配置变更事件注册已下沉到 `client_runtime_events.*`；用户状态命令实现已下沉到 `client_status_commands.*`；命令注册、watched files、metrics、indexing 状态 helper、status UI 文案与配置同步逻辑已分别下沉到 `client_user_commands.*`、`client_internal_commands.*`、`client_watched_files.*`、`client_metrics.*`、`client_indexing_status.*`、`client_status_ui.*`、`client_config_sync.*`

客户端不是语义真相来源。HLSL 关键字、builtin、对象类型、对象方法和诊断规则都应以 server 侧共享模块为准。

## Client 编辑器壳层真相来源

以下能力属于 VS Code client / manifest / 静态配置层，不属于 LSP server 语义真相：

- 语言扩展名归属
- `language-configuration.json`
- snippets
- 注释切换、自动配对、包裹、`wordPattern`、回车续写、folding markers

当前推荐真相来源：

- `package.json`
  - `contributes.languages`
  - `contributes.grammars`
  - `contributes.snippets`
  - `contributes.configuration`
- `syntaxes/nsf.language-configuration.json`
  - 编辑器壳层行为规则
- `snippets/nsf.code-snippets`
  - 片段内容
- `client/src/extension.ts`
  - client 运行时默认 shader 扩展名兜底
- `server_cpp/src/app/main.cpp`
  - server 启动默认 shader 扩展名兜底

如果任务明确涉及这些 client 专有能力，当前事实文档以 `docs/client-editor-features.md` 为准。

## 服务端职责

当前服务端事实位于 `server_cpp/`：

- `app/main.cpp`
  - server 启动与全局调度入口
  - 当前已区分 interactive request lane 与 background request lane
  - `didOpen/didChange/didClose`、active unit 变化、配置变化、workspace summary 变化都会回流到 `document_owner.*`
  - diagnostics background queue / latest-only 调度 / worker runtime 当前已下沉到 `app/main_background_refresh.*`
  - didChange 分类、include graph cache 与 occurrence/definition 辅助当前已下沉到 `app/main_did_change_classification.*`、`app/main_include_graph_cache.*`、`app/main_occurrence_helpers.*`
- `requests/server_request_handlers.cpp`
  - LSP 请求编排层
  - 负责把 completion、hover、signature help、diagnostics、semantic tokens 等请求接到 current-doc runtime / 共享模块
  - 当前 inlay hints 已改为委托 `inlay_hints_runtime.*` 与 `deferred_doc_runtime.*`，不再由 handler 内部持有 full-document inlay cache
  - 当前仍保留 struct hover 的 inline-include fragment 特殊兜底，用于补全当前快照尚未物化的字段列表（基于 `hlsl_ast.*` 的 inline include 元数据）
- `document_owner.*`
  - 每个打开文档的 owner 编排入口
  - 当前负责把 current-doc runtime 的更新、snapshot 切换、interactive snapshot 预热与分析上下文刷新统一串行到单文档 owner API 上；interactive snapshot 预热已并入 owner 持有的单文档串行区
- `document_runtime.*`
  - 每个打开文档的 current-doc runtime 容器
  - 管理 `AnalysisSnapshotKey`、`ActiveUnitSnapshot`、changed ranges、immediate syntax snapshot、interactive snapshot、last-good interactive snapshot、deferred doc snapshot
- `deferred_doc_runtime.*`
  - deferred doc runtime 入口
  - 当前负责 deferred snapshot、semantic tokens full cache、document symbols cache、full diagnostics cache、full-document inlay hints cache，以及 current-doc AST 物化
  - 后台 worker 当前会预热 semantic tokens full、document symbols、full diagnostics，并按配置预热 full-document inlay hints；document symbols 不再回退 legacy 文本扫描
- `inlay_hints_runtime.*`
  - inlay hints runtime 入口
  - 当前负责 full-document inlay hints 构建、慢路径参数补参与 range 过滤，并与 deferred doc runtime 共享 full cache
- `immediate_syntax_diagnostics.*`
  - 即时语法 diagnostics 入口
  - 当前 fast diagnostics 优先走这里，先发布缺分号、括号/注释/预处理配对等低成本结果
- `interactive_semantic_runtime.*`
  - current-doc interactive semantic runtime
  - 当前由 `didOpen/didChange` 与分析上下文刷新主动预热 interactive snapshot；请求热路径只读取 current snapshot / last-good snapshot / deferred snapshot
  - 当前已为 hover、completion、signature help、当前文档短路径 definition、member access 提供 current-doc first + last-good snapshot 优先级
  - 普通 completion 当前会优先合并 interactive snapshot 中的 locals / params / top-level functions / globals / structs，并在需要时再补 workspace summary 候选
- `workspace_summary_runtime.*`
  - workspace summary runtime 边界层
  - 当前作为 `workspace_index.*` 的运行时封装入口，统一暴露 cross-file summary 查询、indexed include closure 查询、reverse include closure 查询与 version 变化
- `server_parse.*`
  - 共享行级解析 helper
  - 提供 declaration / struct / cbuffer / typedef / UI metadata header 等轻量文本解析，供 `declaration_query.*`、`hlsl_ast.*` 与 `workspace_index.*` 复用
  - 当前也负责 `#define` 宏定义头的共享解析，供 hover 在对象宏 / 函数式宏 / 普通函数之间做一致分类
  - 当前也提供注释/字符串剥离后的 shared line scan 与多行 `(`/`[` nesting 结果，供 fast/full diagnostics 的 missing semicolon 规则共用
- `diagnostics/diagnostics.cpp`
  - 诊断主入口
  - 当前已按职责拆成 facade / semantic rules / expression type / symbol type / emit / preprocessor / syntax / indeterminate 等实现文件；`diagnostics/diagnostics.cpp` 保持 build orchestration，`diagnostics/diagnostics_semantic.*` 负责语义规则收集，`diagnostics/diagnostics_expression_type.*` 与 `diagnostics/diagnostics_symbol_type.*` 承担类型推导与符号类型查询辅助
- `semantic_tokens.*`
  - 语义高亮主入口
- `conditional_ast.*`
  - 每文件预处理结构真相
  - 保留条件分支与嵌套结构，供 `preprocessor_view.*` 解释执行
- `preprocessor_view.*`
  - 共享预处理视图
  - 基于 `conditional_ast.*` 提供 active line、branch signature、条件求值诊断
  - 在提供 include 上下文时，会把 active `#include` 链路中的 `#define/#undef` 宏传播纳入同一求值状态
- `expanded_source.*`
  - line-preserving 的 active-only 展开入口
  - 提供展开文本与基础 line source map，供 branch-sensitive 文本扫描复用
- `hlsl_ast.*`
  - 顶层 HLSL AST 骨架
  - 负责 include、function、struct/cbuffer/typedef、全局声明的 active-source 级建模
  - 当前也记录 struct body inline include 路径元数据，供 inline-include hover 兜底复用
- `semantic_snapshot.*`
  - 共享语义快照构建入口
  - 基于 active-source + `hlsl_ast.*` 产出函数签名、overload、参数类型、局部变量类型、struct 字段/字段类型、顶层全局类型语义，并复用 `semantic_cache.*`
  - 当前同时作为 `interactive_semantic_runtime.*` 与 deferred doc runtime 的共享语义底座
  - 当前也负责 current-document struct hover / struct member hover 需要的字段类型与字段行号元数据
- `hover_markdown.*`
  - HLSL 关键字、指令、语义、对象方法的 hover 渲染
- `hlsl_builtin_docs.*`
  - builtin 函数 registry 和文档/签名查询
- `language_registry.*`
  - language/keywords、language/directives、language/semantics 的统一加载与查询
- `type_model.*`
  - 对象类型、对象族、兼容关系查询
- `resource_registry.*`
  - bundle 路径解析、JSON 加载、schema 校验

## 当前共享模块

以下模块的定位是当前事实，应优先复用，不要在新代码里复制规则：

- `call_query.*`: 调用点相关共享查询
- `callsite_parser.*`: 调用点文本定位与参数位次解析共享入口
- `symbol_query.*`: 符号目标共享查询
- `member_query.*`: 成员访问共享查询
- `declaration_query.*`: 声明位置共享查询
- `document_owner.*`: 单文档 owner 编排入口
- `document_runtime.*`: current-doc runtime、analysis key 与 last-good snapshot 的统一所有者
- `deferred_doc_runtime.*`: deferred doc snapshot、semantic tokens/document symbols/full diagnostics/full-document inlay hints cache 的统一入口
- `inlay_hints_runtime.*`: inlay hints full-document 构建、慢路径补参与 deferred full-cache 复用入口
- `immediate_syntax_diagnostics.*`: fast syntax diagnostics 共享入口
- `interactive_semantic_runtime.*`: current-doc interactive semantic 查询入口
- `server_parse.*`: 共享行级 declaration/header 解析，当前已被 `declaration_query.*`、`hlsl_ast.*` 与 `workspace_index.*` 复用
- `hover_rendering.*`: 通用 hover 结构化渲染
- `completion_rendering.*`: 通用 completion item 拼装
- `diagnostics/diagnostics_expression_type.*`: diagnostics 侧表达式 / literal / builtin type 推导共享实现
- `diagnostics/diagnostics_symbol_type.*`: diagnostics 侧符号类型 / struct member 类型查询共享实现
- `conditional_ast.*`: 共享预处理结构解析，保留每文件的条件分支与嵌套关系
- `preprocessor_view.*`: 基于 `conditional_ast.*` 的共享预处理 active branch / branch signature 查询，供 diagnostics、definition、局部类型相关查询，以及同文件条件分支 symbol family 聚合复用
- `expanded_source.*`: 基于 `preprocessor_view.*` 的 line-preserving active-only 展开与基础 line source map，供类型与声明相关的 branch-sensitive 文本扫描复用
- `hlsl_ast.*`: 基于 active-source 的顶层 HLSL AST 骨架，当前已接入 `full_ast.*` 与函数签名索引
- `semantic_snapshot.*`: 基于 active-source 与 `hlsl_ast.*` 的共享语义快照，当前已接入函数签名/overload 查询、参数类型、局部变量类型、struct 字段、字段类型、顶层全局类型，以及 current-document struct hover / struct member hover / 部分 member completion 解析
- `workspace_summary_runtime.*`: workspace summary runtime 封装入口
- `workspace/workspace_index.*`: 还承担 include 反向依赖查询，供 include 文件在无 active unit 时发现候选 root unit，并支撑 hover 的候选定义摘要、definition 的多目标结果，以及 references 的多上下文聚合；file watch 回流当前也依赖它提供 reverse include closure，只刷新直接变更或命中的 open docs
  - 当前 `workspace/workspace_index.cpp` 主要保留 facade、summary query 与最小 owner wiring；cache 路径、磁盘 load/save 与旧索引兼容迁移已下沉到 `workspace/workspace_index_cache.*`；path 归一化、include-closure 扫描与 file-to-meta 解析 helper 已下沉到 `workspace/workspace_index_scan.*`；rebuild / file-watch update / 后台线程与并行索引调度已下沉到 `workspace/workspace_index_scheduler.*`；struct/definition 提取与 reverse-include 聚合 helper 已下沉到 `workspace/workspace_index_extract.*`、`workspace/workspace_index_reverse_include.*`；序列化/反序列化与基础内部结构位于 `workspace/workspace_index_internal.*`

## 单一事实来源

当前推荐的真相来源如下：

- HLSL builtin 函数：`hlsl_builtin_docs.*` + `server_cpp/resources/builtins/intrinsics/`
- HLSL 关键字、预处理指令、系统语义：`language_registry.*` + `server_cpp/resources/language/`
- HLSL 对象类型/对象族：`type_model.*` + `server_cpp/resources/types/`
- HLSL 对象方法：当前由 `hover_markdown.*` 读取 `server_cpp/resources/methods/object_methods/`
- 资源 bundle 基础规则：`resource_registry.*`

如果某个能力需要新增语言知识，优先扩展这些共享入口，而不是在 feature 代码里临时加一份表。

## 资源到能力的关系

- `builtins/intrinsics`
  - hover builtin 文档
  - signature help builtin 函数签名
  - completion builtin 名称
  - diagnostics builtin 识别
- `language/keywords`
  - completion 关键字
  - hover 关键字说明
  - diagnostics 关键字识别
  - semantic tokens 关键字高亮
- `language/directives`
  - `#include` 等预处理指令 completion/hover
- `language/semantics`
  - `SV_*` 系统语义 hover/识别
- `types/*`
  - texture/sampler/buffer 家族识别
  - 成员方法匹配
  - diagnostics 类型兼容辅助
- `methods/object_methods`
  - texture-like/buffer-like 方法签名与文档

## 当前编辑优先运行时事实

- current-doc 状态现在由 `document_owner.*` 串行编排，并统一挂在 `document_runtime.*`
- `ActiveUnitSnapshot` 当前不仅保存 fingerprint，还保存 workspace/include/defines 上下文，以及 open active-unit 的 version/epoch；`interactive_semantic_runtime.*` 与 `deferred_doc_runtime.*` 会共同消费这份前提，并据此判定非 active 文档上的 shared-context 复用是否仍然安全
- active unit 的 include closure 当前已改为基于 `preprocessor_view.*` 解释得到的 active include 链路，而不是纯文本 `#include` 递归
- interactive snapshot 当前会在 `didOpen/didChange`、active unit 变化、配置变化与 workspace summary version 刷新后主动预热；对括号/分号等小范围 syntax-only 编辑，以及纯注释编辑，会优先让 immediate syntax diagnostics 抢占热路径；对纯注释编辑，本次 `didChange` 还可以跳过 deferred-doc 重建与 full diagnostics 立即重排；请求热路径不再按需现建 snapshot
- immediate syntax / full diagnostics 当前用于分支 gating 的 `preprocessor_view.*` 也会加载当前文档可解析 include 链中的宏状态，避免 active include-controlled branch 上的缺分号等语法问题被误判成 inactive branch 而跳过
- immediate syntax / full diagnostics 当前对 missing semicolon 的共享判断还会消费 `server_parse.*` 的 branch-aware 多行 `(`/`[` nesting 结果；active 多行构造/调用表达式内部的续行不应因为 closing `);` 落在后续行而被误报
- interactive runtime 当前会消费 `changedRanges`：对注释/空白类 changed window 优先做 last-good incremental promote，避免无语义编辑触发整份 snapshot 重建
- 成员 completion 当前会优先消费 interactive / last-good / deferred snapshot 中的 typed struct fields；workspace summary 只作为缺失时的补充回退，并会过滤预处理指令行，避免把宏名误收成 struct 成员
- workspace file watch 回流当前只会对直接变更或 reverse-include 命中的打开文档刷新 analysis key，避免无关 open docs 被整批脏化
- fast diagnostics 当前优先发布 immediate syntax 结果，full diagnostics 再异步补齐
- interactive 请求当前优先级高于 background 请求：
  - interactive: completion、hover、signature help、definition
  - background: semantic tokens、inlay hints、references、rename、document symbols
- definition / hover / signature help 的编辑热路径当前已不再依赖 workspace scan / include graph 直扫兜底，而是优先 current-doc runtime、deferred doc snapshot 与 workspace summary
- function / symbol hover 的 `Defined at` 与 overload location 当前由 request/rendering 层统一输出可点击文件链接，并按签名 + 位置去重 overload 列表项
- request 层当前会先结合 current-doc 定义位置与 workspace `kind=14` 宏定义结果，把普通 `#define` 渲染为 macro hover；只有 macro-generated function 这类展开后产出真实函数声明的模式仍走 function hover
- function hover 的 overload 列表当前只消费 current-doc / shared semantic snapshot / workspace summary 已产出的函数语义；request 层不再额外补一份 per-definition fallback 签名，缺失项应回到共享语义链路修正
- include 文件的 hover 现在会先汇总 candidate unit 的解析结果，再按 distinct definition locations 判定是否 truly ambiguous；只有跨 unit 收敛到多个不同定义位置时才显示 include-context ambiguous 提示与 candidate definitions 分组列表
- include 文件的 hover 如果所有 candidate units 最终收敛到同一个定义位置，则不会追加 include-context 摘要；如果只有部分 candidate units 收敛到定义，则只显示 partial-resolution 提示，不平铺 candidate definitions 列表
- include-context ambiguous 分支当前也已改为基于 workspace summary 的 indexed include closure 过滤候选定义，不再单独直扫 include graph
- references / rename 当前也已改为基于 workspace summary 的 indexed include closure 收集 active occurrences，不再先经 include-graph 定义定位反推扫描范围
- Lane C（background）请求当前统一 latest-only + cancellation，覆盖 inlay hints、semantic tokens full/range、document symbols、references、prepareRename、rename、workspace symbol
- deferred doc runtime 当前会缓存 current-doc AST、semantic snapshot、semantic tokens full、document symbols、full diagnostics 与 full-document inlay hints；document symbols 构建已优先复用 current-doc AST / semantic snapshot，仅对 technique/pass 保留轻量文本补充，不再回退 legacy 顶层文本扫描
- inlay hints 当前会优先复用 deferred doc runtime 的 full-document hints，再按请求 range 过滤；慢路径补参成功后会主动失效当前文档的 inlay full cache
- deferred doc runtime 后台任务当前也会按配置预热 full diagnostics 与 full-document inlay hints，减少首次请求命中的临时构建
- workspace summary 入口当前统一经由 `workspace_summary_runtime.*`，再由它封装 `workspace_index.*`
- workspace index 在 startup 命中持久化磁盘 cache 时，会先发布该 summary 供 cross-file 查询复用；warm-cache 启动不再先重建当前 active unit 的整条 include closure，再在后台按 `mtime/size` 做校验、脏文件重建与新文件探测；如果命中的是已确认不兼容的旧索引 cache，会自动清理该 cache 并执行一次完整重建，避免后续每次启动都被坏 cache 判成全量脏化；`workspaceSummaryRuntimeIsReady()` 的含义因此是“当前可查询”，不等同于“后台校验已全部结束”
- active unit 切换当前会触发一次新的 workspace index 重排；索引会优先处理当前 active unit 的 include closure，并在检测到更晚的 active unit / rebuild 请求后尽快中止旧批次，让新的 active unit 尽快进入可查询状态，其余工作区扫描继续留在后台
- semantic tokens 当前只负责 keyword / number / macro / function / variable / type / property / operator 等语义项；comment / string 着色不再由 semantic tokens 负责
- metrics 当前已补齐 method p50/p95/p99，以及 interactive/deferred runtime 的 snapshot wait、latest-only merge、merge hit/miss 指标，统一通过 `nsf/metrics` 通知上报

### 调度契约对应头文件

下面这些 `*.hpp` 现在也承担当前事实中的局部接口契约，review 时不应只靠 `*.cpp` 反推：

- `server_cpp/src/server_request_handlers.hpp`
  - request layer 的当前调度边界
  - interactive 高优先级路径：`completion`、`hover`、`signature help`、当前文档短路径 `definition`
  - background latest-only + cancellation 路径：`semantic tokens`、`inlay hints`、`document symbols`、`references`、`prepareRename`、`rename`、`workspace symbol`
  - request handler 在 interactive miss 时可以查 `workspace_summary_runtime.*`，但不应重新引入 include-graph 直扫或全工作区现算热路径

- `server_cpp/src/document_owner.hpp`
  - open document 的单 owner 串行入口
  - `didOpen` / `didChange` / analysis-context refresh 都必须先切 `document_runtime.*`，再预热 interactive snapshot
  - snapshot publish 也应通过 owner API 进入，避免请求线程直接并发写 runtime 状态

- `server_cpp/src/interactive_semantic_runtime.hpp`
  - current-doc interactive 查询顺序契约：`current -> last-good -> deferred -> workspace summary`
  - `last-good` 只允许在 stable context 指纹不变时复用
  - `workspace summary` 只能补 miss，不应覆盖已命中的 current-doc 主结果

- `server_cpp/src/deferred_doc_runtime.hpp`
  - deferred doc runtime 的后台 latest-only 合并契约
  - 同一 `uri` 的新任务会替换尚未构建的旧 pending 任务
  - worker 发现 analysis key 已过期时只能 drop stale work，不能发布过期最终结果

- `server_cpp/src/document_runtime.hpp`
  - `AnalysisSnapshotKey`、`ActiveUnitSnapshot` 与 `DocumentRuntime` 的共享上下文契约
  - interactive / deferred snapshot 必须围绕同一份 analysis key 发布与复用
  - active unit、include closure、defines、workspace summary version、resource model 的变化必须先落到这里，再决定 snapshot 是否还能复用

## 需要同步更新本文档的场景

出现以下情况时，必须更新本文档：

- client 和 server 的职责边界变化
- 新增共享查询模块并成为事实入口
- builtin/language/type/method 的单一事实来源变化
- 关键请求链路发生改变
