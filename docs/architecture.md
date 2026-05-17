# 架构总览

本文档描述当前仓库的模块边界、请求链路和单一事实来源。它是当前事实文档，不是设计规划稿。

相关入口：

- 编辑态即时反馈目标架构提案：`docs/human-ai/realtime-feedback-design.md`
- 对象类型 / 对象方法共享契约：`docs/type-method-interface-contract.md`
- client 编辑器壳层能力：`docs/client-editor-features.md`

## 顶层结构

- `client/`: VS Code 扩展客户端，负责启动语言服务、同步配置、消费 LSP 能力和展示状态
- `server_cpp/`: C++ LSP 服务端，负责解析、索引、诊断、悬停、补全、签名帮助和语义高亮
- `server_cpp/resources/`: 服务端运行时资源 bundle
- `src/test/`: VS Code 集成测试入口
- `test_files/`: 仓库模式测试夹具
- `scripts/`: 门禁、资源、构建和打包脚本
- `docs/`: 当前事实文档和专题文档
- `docs/human-ai/`: 人类与 AI 协作沉淀，默认不属于当前事实来源

## 架构原则

- client 不保存语言语义真相；HLSL 关键字、builtin、对象类型、对象方法和诊断规则以 server 侧共享模块为准。
- request 层负责编排和渲染，不应复制底层资源规则或直接绕过共享查询模块。
- 资源、共享模块、request handler、测试和文档共同构成事实接口；行为承诺不能只存在实现细节里。
- 新增或拆分模块时保持单一职责；历史大文件被触达时，优先沿已有边界下沉职责。
- `*.hpp` 是局部接口契约；public API、缓存语义、调用前提或所有权边界变化时必须同步更新头文件说明。

## 接口层次

| 层次 | 主要载体 | 责任 |
| --- | --- | --- |
| 资源/数据接口 | `server_cpp/resources/*`、schema | 定义原始语言数据结构与字段语义 |
| 加载接口 | `resource_registry.*` | 解析 bundle 路径、读取 JSON、执行 schema 校验 |
| 共享模块 API | `*.hpp` public API | 暴露跨模块查询入口、状态所有权和调用前提 |
| consumer-ready 语义接口 | `type_model.*`、`semantic_snapshot.*` 等查询结果 | 提供下游可直接消费的共享判定 |
| 请求/渲染接口 | request handler、rendering/runtime 模块 | 组合共享语义并输出 LSP 结果 |
| 事实/验证接口 | 当前事实文档、测试夹具、集成断言 | 固化当前契约并提供回归验证 |

约束：

- 能留在资源层的问题，不提前下沉到 request 层。
- 能由共享模块统一回答的问题，不让 consumer 重复解释。
- request 层可以组合语义，但不重新发明语言知识。

## 请求链路

1. VS Code 激活 `client/out/extension.js`。
2. client 默认使用插件内置 C++ server；`nsf.serverPath` 仅作为开发调试覆盖。
3. client 通过 stdio 启动 server，并同步配置、workspace 状态和文档变更。
4. server 在 request handler 层分发 LSP 请求。
5. 具体能力优先消费 current-doc runtime、deferred runtime、workspace summary 和共享 registry。
6. client 接收 LSP 结果并交给 VS Code 呈现。

## Client 边界

当前客户端主要负责：

- server lifecycle、restart 和 `onReady` 装配
- 配置同步，包括 include 路径、defines、diagnostics、inlay hints、semantic tokens 和 metrics
- 预处理宏 preset 首次填充与配置同步；client 从 server 共享 registry 读取默认 preset，写入工作区 `nsf.preprocessorMacros` 后再作为普通用户配置传给 server
- 状态栏、trace 输出、诊断和索引状态展示
- 测试模式下的固定启动和内部命令
- 编辑器壳层挂接，如语言注册、language configuration 和 snippets

当前拆分入口：

- `client_runtime_host.*`: LanguageClient 启动、重启和 ready lifecycle
- `client_active_unit.*`: active unit 状态与 `selectUnit`
- `client_editor_feedback.*`: editor feedback、indexing 与 inlay provider 主体
- `client_editor_events.*`: editor/document 事件注册
- `client_runtime_events.*`: runtime config、git-storm watcher 和配置变更事件注册
- `client_user_commands.*` / `client_internal_commands.*`: 用户命令与测试内部命令
- `client_watched_files.*`、`client_metrics.*`、`client_indexing_status.*`、`client_status_ui.*`、`client_config_sync.*`: 对应单一职责 helper

编辑器壳层能力的当前事实以 `docs/client-editor-features.md` 为准。

## Server 边界

### 进程与调度

- `app/main.cpp`: server 启动、全局调度、interactive/background lane 区分、request-scoped 调度归因遥测，以及文档 / active unit / 配置 / workspace summary 事件回流
- `app/main_background_refresh.*`: diagnostics background queue、latest-only 调度和 worker runtime
- `app/main_did_change_classification.*`: didChange 分类
- `app/main_diagnostics_audit_debug.*`: real-workspace diagnostics audit 专用内部 debug 请求；只读取 workspace summary include closure 和 diagnostics 构建结果，不发布 diagnostics，也不改变公开 LSP 行为
- `app/main_include_graph_cache.*`: include graph cache
- `app/main_occurrence_helpers.*`: occurrence / definition 辅助
- `crash_handler.*`: server 进程级 SEH / signal / terminate 崩溃处理入口；`installCrashHandler(...)` 只登记处理器和记录目标日志路径，正常启动不得创建 `nsf_lsp_crash.log`；crash log、stacktrace 和 minidump 只应在真实崩溃路径或显式 debug wait 路径中写出

### 文档运行时

- `document_owner.*`: 打开文档的单 owner 串行入口；`didOpen`、`didChange` 和 analysis-context refresh 都应先切 `document_runtime.*`，其中 `didOpen` / analysis-context refresh 主动预热 current-doc semantic snapshot，`didChange` 保持输入线程轻量并交给后续交互请求按需构建最新 snapshot
- `document_runtime.*`: `AnalysisSnapshotKey`、`ActiveUnitSnapshot`、changed ranges、current / last-good / deferred snapshot 的统一所有者
- `interactive_semantic_runtime.*`: current-doc interactive 查询入口；请求热路径读取 current、last-good、shared-visible、deferred 和 workspace summary，不在热路径现建 snapshot
- `interactive_visibility_runtime.*`: active unit include closure 约束下的 cross-file visible symbols
- `deferred_doc_runtime.*`: deferred snapshot、semantic tokens、document symbols、full diagnostics、full-document inlay hints cache 和 current-doc AST 物化
- `inlay_hints_runtime.*`: full-document inlay hints 构建、慢路径参数补参与 range 过滤
- `immediate_syntax_diagnostics.*`: fast syntax diagnostics 入口

### 语义与查询

- `server_parse.*`: 共享行级 declaration/header 解析、`for` initializer declaration 解析、宏定义头解析、注释/字符串剥离后的 shared line scan、多行 nesting 前后状态，以及 missing-semicolon syntax boundary 判断；多行函数 / control header、NSF metadata / effect block、表达式 continuation 和 macro-only recovery 区域的高置信缺分号判定应在这里统一收敛
- `conditional_ast.*`: 每文件预处理结构真相
- `preprocessor_view.*`: active branch、branch signature、配置预处理宏初始化和 include 链宏传播求值
- `expanded_source.*`: line-preserving active-only 展开与基础 line source map
- `hlsl_ast.*`: 顶层 HLSL AST 骨架、include、function、struct/cbuffer/typedef、全局声明和 inline include 元数据
- `semantic_snapshot.*`: 共享语义快照构建入口，产出函数、overload、参数、lexical local scope / 局部变量、struct 字段、全局类型等语义；local 查询必须同时满足 declaration offset 和 half-open lexical scope range，不能只按 brace depth 近似可见性
- `call_query.*`、`callsite_parser.*`、`symbol_query.*`、`member_query.*`、`declaration_query.*`: 请求间共享查询 helper

### 能力与资源

- `server_request_handlers.hpp` / `requests/server_request_handlers.cpp`: LSP 请求编排层；`ServerRequestContext` 携带只读文档 / 配置快照和 attribution-only 的 request queue/context-build 耗时；interactive miss 可以查询 `workspace_summary_runtime.*`，但不应重新引入 include-graph 直扫或全工作区现算热路径
- `hover_markdown.*` / `hover_rendering.*`: hover 内容渲染；对象方法规则查询会从 `methods/object_methods` 读取签名模板，并通过 `type_model.*` 展开 `{floatCoord}` / `{intCoordPlus1}` 等参数形状，供 hover、signature help 和 diagnostics 共享消费
- `completion_rendering.*`: completion item 拼装
- `diagnostics/*`: diagnostics facade、semantic rules、expression type、symbol type、emit、preprocessor、syntax 和 indeterminate 分层；semantic diagnostics 的 local symbol、duplicate local declaration、`for` initializer 可见性和基础 block-flow 诊断基于函数内 lexical scope stack，duplicate local 只在同一 lexical scope 且 active preprocessor branch 重叠时发布；object method call diagnostics 必须消费 `hover_markdown.*` 展开的资源参数和 `type_relation.*`，不得在 rule 内本地推导 array texture 坐标或复制 sampler / coord 兼容规则
- `diagnostics_prerequisites.*`: semantic diagnostics 高置信发布前提契约；统一表达 active unit ready、include closure ready、preprocessor context reliable、parser region reliable、semantic snapshot available、local scope reliable 和 expression type available。semantic source、expression type、call type 和 undefined identifier 规则必须通过该共享入口判断前提；当前提不满足时跳过高置信 diagnostics，只在 debug / audit metadata 中累计 skipped reason，不新增 fallback、shim 或旧逻辑兜底。
- `diagnostics_expression_type.*`: diagnostics 共享表达式类型 helper；负责类型 token 归一化、builtin call 类型规则、numeric literal token-span 解析和表达式结果类型推断。builtin call 的元素类型合并和 mixed signedness 判断应通过 `type_relation.*` 的 usual arithmetic conversion 产生合法转换 warning，不能把 `int` / `uint` 混用伪装成 builtin mismatch。common builtin 规则包括同型返回的 `log/log2/log10/round/radians/degrees/ddx/ddy/fwidth`，返回 bool 的 `all/any`，矩阵转置 `transpose`，以及 call-only 的 `sincos`；参数类型无法推断时仍发布 indeterminate metadata，而不是回退到猜测类型。numeric literal 解析必须在该共享入口按官方 HLSL numeric literal 语法处理，因为当前 lexer 会把 decimal point 和 exponent sign 切为 punctuation token，semantic diagnostics 不应在规则层复制后缀判断；合法 exponent、leading/trailing dot、octal / hex integer、`h/H/f/F/l/L` 浮点 suffix 和 `u/U/l/L` 整数 suffix 组合都应在这里统一判定。implementation-only `ll/ull` 整数 suffix 作为历史写法只应由共享入口产生 warning 并推荐 `l/ul`，真正不符合语法的 suffix 继续作为 error。
- `type_desc.*`: diagnostics / overload 共享轻量类型形状解析；负责把 HLSL scalar / vector / matrix / object token 和常见 macro-like numeric alias（如 `MaterialFloat3`、`MaterialHalf4x4`）归一为 `TypeDesc`。它不负责完整 typedef 展开、用户 struct 建模或对象方法坐标维度，这些仍由 semantic snapshot、symbol query 和 `type_model.*` 负责。
- `type_relation.*`: diagnostics / overload 共享 HLSL 隐式转换模型；以官方 standard conversion sequence、usual arithmetic conversions 和 overload conversion rank 为基础，结构化返回 compatible / incompatible、conversion kind、cost 和 risky implicit conversion warning。assignment、return、user function argument、builtin argument、object method argument 和 binary operator diagnostics 不应再各自复制 half/float、scalar splat、component-wise conversion、truncation、signedness 或 boolean conversion 判断。合法但有风险的隐式转换发布独立 warning；找不到官方转换序列时才发布 type mismatch error。
- `semantic_tokens.*`: 语义高亮主入口；在 deferred semantic snapshot 可用时消费 `SemanticSnapshot` 输出变量角色、声明和修改信号，fallback 仍保持词法扫描；comment / string 着色继续由 TextMate grammar / 编辑器壳层负责；`.nsf/.hlsl` 走同一 LSP semantic-token 路径，`.hlsl` 的 active-unit-sensitive 上下文来自当前 `.nsf` include context，`.nsf` 仍是唯一 active/root unit
- `hlsl_builtin_docs.*`: builtin 函数 registry 和文档 / 签名查询
- `language_registry.*`: language/keywords、language/directives、language/semantics、language/preprocessor_macros 的统一加载与查询
- `type_model.*`: 对象类型、对象族、兼容关系和 consumer-ready 维度查询
- `resource_registry.*`: bundle 路径解析、JSON 加载和 schema 校验

### Workspace

- `workspace_summary_runtime.*`: `workspace_index.*` 的运行时边界层，统一暴露 cross-file summary、indexed include closure、reverse include closure 和 version 变化
- `workspace/workspace_index.*`: workspace summary facade 和 owner wiring
- `workspace/workspace_index_cache.*`: cache 路径、磁盘 load/save 和旧索引兼容迁移
- `workspace/workspace_index_scan.*`: path 归一化、include-closure 扫描和 file-to-meta 解析
- `workspace/workspace_index_scheduler.*`: rebuild、file-watch update、后台线程和并行索引调度
- `workspace/workspace_index_extract.*`: struct / definition 提取，包括 FX/NSF metadata-block 全局变量声明
- `workspace/workspace_index_reverse_include.*`: reverse include 聚合
- `workspace/workspace_index_internal.*`: 序列化、反序列化和基础内部结构

## 当前调度契约

- interactive lane: completion、hover、signature help、当前文档短路径 definition
- background lane: semantic tokens、inlay hints、references、rename、document symbols、workspace symbol
- background 请求统一 latest-only + cancellation；过期 analysis key 的结果只能 drop，不能发布。
- current-doc semantic snapshot 在 `didOpen`、active unit 变化、配置变化和 workspace summary version 刷新后主动预热；`didChange` 只同步维护最新文档和 runtime key，completion / hover / signature help 等交互请求按需 build 或 promote 最新 current-doc snapshot，fast diagnostics worker 按 latest-only 构建并存储最新 local structural snapshot，fast diagnostics publish 启用时再由该 snapshot 发布 local-structural diagnostics，避免逐字符输入时在 server 输入线程上重建每个中间版本。
- request worker 写入 `ServerRequestContext` 的 queue wait / context build / request document version / debug wall-clock timestamp / didChange 输入线程重叠摘要只用于 debug 和 replay 归因，不参与 completion、hover、signature help、diagnostics 等公开行为决策。completion replay 归因可在现有 LSP completion params 上附加 `nsfDebugRequestId` 和 client send-start timestamp 调试字段，server 只把它们写入 completion debug snapshot/history；这些字段不得参与候选生成、排序、过滤或触发行为。
- 预处理宏 preset 属于配置输入：`nsf.preprocessorMacros` 是完整有效 preset 表，`nsf.defines` 和源码 `#define/#undef` 按顺序覆盖；preset fingerprint 必须参与 active-unit / semantic cache 复用判断。
- 小范围 syntax-only 编辑和纯注释编辑优先让 immediate syntax diagnostics 抢占热路径。
- fast diagnostics 会先发布 immediate syntax，再异步补 full diagnostics；等待 full 结果时可保留 last-good full diagnostics，避免无关语义波浪线被整份清空。
- diagnostics payload 在构建返回和发布层合并后按 document URI、range、message、code 和 source 去重；同一位置的不同原因应通过不同 code/source 保持可区分。
- diagnostics mode 只改变发布策略，不复制类型知识：共享 type relation 仍负责判断兼容、mismatch 和风险等级；合法但有风险的隐式转换 warning 只在 `full` 对应的 `typeConversionRiskWarningsEnabled` 下发布，`balanced` 仍保留真正的 assignment / return / call mismatch 错误。
- client completion request coordinator 只调度 identifier-prefix auto-trigger / quick suggest 请求：发往 LSP 前按 coordinator key 做短窗口 latest-only 合并；发往 LSP 后如果同 key 前进前缀或新的 completion key 使旧 auto-trigger visible request 过期，旧 visible provider promise 会立即 neutral resolve，并取消 coordinator 持有的 underlying `next(...)` token；已启动的 `next(...)` 只做 detached cleanup 和指标记录。同 key prefix shrink 不走 stale supersession。只有已归类为 identifier auto-trigger 的请求会刷新 quick-suggest burst recent 状态，单独的显式 `Invoke` 不会种下后续 coalescing。显式用户触发、`.` member completion、retrigger 和无法安全归类的请求自身直接绕过 coordinator，server completion 候选、排序和文档渲染仍由 server 侧共享语义入口决定。
- inlay hints 优先复用 deferred doc runtime 的 full-document cache；对 indexing 抖动、请求取消、瞬态 RPC 错误或短暂空结果，client 侧可续用 last-good hints，避免编辑过程中前台提示闪空。
- workspace warm-cache 启动时，`workspaceSummaryRuntimeIsReady()` 表示“当前可查询”，不等同于后台校验已经全部完成。

关键头文件契约：

- `server_cpp/src/server_request_handlers.hpp`: request layer 调度边界
- `server_cpp/src/document_owner.hpp`: open document 单 owner 串行入口
- `server_cpp/src/document_runtime.hpp`: analysis key、active unit 和 snapshot 复用前提
- `server_cpp/src/interactive_semantic_runtime.hpp`: current-doc interactive 查询顺序
- `server_cpp/src/deferred_doc_runtime.hpp`: deferred latest-only 合并和 stale work drop 规则
- `server_cpp/src/diagnostics_prerequisites.hpp`: semantic diagnostics rule prerequisite 和 skipped-reason 统计契约

## 单一事实来源

- HLSL builtin 函数：`hlsl_builtin_docs.*` + `server_cpp/resources/builtins/intrinsics/`
- HLSL 关键字、预处理指令、系统语义、默认预处理宏填充 preset：`language_registry.*` + `server_cpp/resources/language/`
- 有效用户预处理宏 preset：`nsf.preprocessorMacros` + `preprocessor_view.*`
- HLSL 对象类型 / 对象族：`type_model.*` + `server_cpp/resources/types/`
- HLSL 对象方法：`hover_markdown.*` + `server_cpp/resources/methods/object_methods/`
- 资源 bundle 规则：`resource_registry.*` + `docs/resources.md`
- 对象类型 / 方法共享契约：`docs/type-method-interface-contract.md`
- 编辑器壳层能力：`package.json`、`syntaxes/nsf.language-configuration.json`、`snippets/nsf.code-snippets` + `docs/client-editor-features.md`

如果某个能力需要新增语言知识，优先扩展这些共享入口，而不是在 feature 代码里临时加表。

## 资源到能力

- `builtins/intrinsics`: hover builtin 文档、signature help、completion 名称、diagnostics builtin 识别
- `language/keywords`: completion、hover、diagnostics 和 semantic tokens
- `language/directives`: 预处理指令 completion / hover
- `language/semantics`: `SV_*` 系统语义 hover / 识别
- `language/preprocessor_macros`: 用于首次填充 `nsf.preprocessorMacros` 工作区设置的默认 preset；包含 shadercompiler builtin 宏以及 system / device / API support / platform quality 编译上下文宏名，编译上下文宏默认保守值为 `0`，真实 target / compile mode 值由 workspace 配置或 `nsf.defines` 覆盖；`#if` / `#elif` 表达式求值、active branch 判断和预处理宏 diagnostics 消费设置同步后的完整宏表
- `types/*`: texture / sampler / buffer 家族识别、成员方法匹配和 diagnostics 类型兼容辅助
- `methods/object_methods`: texture-like / buffer-like 方法签名与文档

## 更新本文档

以下变化必须同步更新本文档：

- client 和 server 职责边界变化
- 新增、删除或重命名当前事实共享模块
- builtin / language / type / method 的单一事实来源变化
- request lane、snapshot 复用、workspace summary 或关键缓存契约变化
- 需要维护者按新边界理解的头文件接口契约变化
