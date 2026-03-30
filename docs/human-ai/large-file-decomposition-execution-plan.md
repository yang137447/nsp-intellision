# 大文件拆解执行方案（单一职责 / AI 可维护性）

本文档定义本项目围绕“按单一职责原则拆解工程大文件”的执行方案、阶段边界、文件级拆分建议与验证要求。

重要说明：

- 本文档是执行提案，不是当前事实。
- 本文档位于 `docs/human-ai/`，属于人类与 AI 协作设计稿区域。
- 当前事实仍以 `README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md` 为准。
- 本文档关注“如何逐步拆解、如何验收、什么情况下停止确认”；默认不承担实时进度看板职责，但在明确要求交接时可附带短期进度快照。
- 本文档默认遵循当前 `AGENTS.md` 约束：最小改动、单一事实来源、先根因后实现、不得顺手扩展为无关重构。

## 1. 目标与问题定义

### 1.1 目标

本方案的目标不是为了“把文件变小而变小”，而是把当前已经存在的架构边界，落实到更易维护、更易被 AI 理解和增量修改的物理文件布局上。

如果压缩成一句话，本方案要把当前仓库从“架构边界清楚但部分实现文件过大”推进到“薄入口 + 清晰职责模块 + 头文件契约可直接指导 AI 修改”的状态。

### 1.2 当前问题

当前仓库的事实文档已经明确要求：

- 优先保持单一职责
- 复用共享模块和单一事实来源
- 头文件承担局部接口契约
- 增量修改时不要继续把新职责直接塞进历史大文件

但从当前实现分布看，仍存在多处超大文件：

| 文件 | 当前规模（约） | 当前主要问题 |
| --- | --- | --- |
| `server_cpp/src/diagnostics.cpp` | `5300+` 行 | 规则收集、类型推导、预处理分析、diagnostic 输出混在一起 |
| `src/test/suite/client.integration.groups.ts` | `4300+` 行 | 大量 suite 注册与具体断言实现堆在一个文件里 |
| `server_cpp/src/main.cpp` | `3400+` 行 | 入口编排、helper、局部缓存、文本扫描逻辑混杂 |
| `server_cpp/src/workspace_index.cpp` | `3300+` 行 | index facade、磁盘 cache、扫描、提取、反向 include、调度混杂 |
| `server_cpp/src/server_request_handlers.cpp` | `3100+` 行 | request dispatch、各类 handler、渲染辅助、上下文 fallback 混杂 |
| `client/src/extension.ts` | `2300+` 行 | activate/deactivate、配置同步、命令、UI、状态处理都在一个入口 |

### 1.3 为什么这对 AI 特别重要

对人类维护者，大文件主要带来 review 和定位成本。对 AI 协作者，额外还会带来：

- 上下文负载过高：为了改一个点，需要把大量无关逻辑一起读进上下文
- 误改风险增加：同文件内多个职责靠近时，更容易改对局部、破坏全局
- 局部兜底冲动变强：AI 在大文件里更容易“顺手补一个判断”，而不是沿共享模块扩展
- 验证范围判断变差：边界不清时，更难判断最小必要测试集
- 并行协作变差：多人或多次 AI 增量修改容易集中撞在同一文件

因此，本方案的收益重点不是代码风格，而是：

- 提升 AI 对模块责任的稳定理解
- 降低误改和局部规则复制
- 提高“最小正确改动”的成功率

## 2. 设计原则与边界

### 2.1 拆解原则

所有拆解任务都必须满足：

- 一个模块只承载一个主责任
- 薄入口文件只负责编排，不再承载具体领域细节
- 共享语义规则仍回到既有共享模块，不在新文件里复制一份规则
- `*.hpp` 继续承担局部接口契约，说明职责、输入输出、调用前提与非目标
- 拆解优先服务“增量维护”和“AI 易读性”，不是为了抽象层数更多

### 2.2 非目标

本方案明确不追求：

- 一次性重写整个 server / client 架构
- 为了目录整齐而先搬迁所有文件
- 为了通用性新增 adapter、compat layer、fallback 或双路径
- 顺手统一命名风格、注释风格、目录风格
- 把一个复杂责任切成很多没有清晰契约的碎文件

### 2.3 执行策略

推荐按“两阶段策略”执行：

1. 第一阶段先做逻辑拆分，不急于搬目录。
2. 第二阶段在逻辑边界稳定后，再考虑目录整理。

原因：

- 当前 `server_cpp/CMakeLists.txt` 显式列出每个 `src/*.cpp`
- 当前测试入口也有固定装配方式
- 如果一开始同时做“职责拆分 + 目录迁移 + include 修复 + 构建改造”，风险会显著上升

## 3. 总体阶段规划

### 3.1 阶段顺序

推荐按下面顺序推进：

1. 测试聚合文件拆解
2. request handler 拆解
3. diagnostics 拆解
4. workspace index 拆解
5. client extension 拆解
6. main.cpp 缩薄
7. 逻辑稳定后再评估目录迁移

这个顺序的原因是：

- 先拆测试，能先降低后续改造的验证维护成本
- request / diagnostics / workspace index 是 server 侧最大的 AI 负担来源
- client extension 影响面偏广，但逻辑风险低于 server 核心语义
- main.cpp 适合作为最后的“薄入口收尾”，而不是最先重构的起点

### 3.2 每阶段统一退出标准

每个阶段完成时，至少要满足：

1. 主责任边界比改造前更清晰，而不是只换文件名
2. 原公开行为不变，未引入 fallback / adapter / 双路径
3. 对应 `*.hpp` 契约和注释已同步
4. 最小必要验证已执行并记录
5. 如果模块事实边界变化，已同步更新事实文档
6. 如果只是 proposal / 执行方案更新，不把其误写进当前事实文档

### 3.3 验收标注规则

为避免把“已经发生物理拆分”误写成“当前执行包已经完成”，本方案统一使用以下验收标注：

- `[已验收完成]`
  - 表示当前执行包已经满足 `3.2` 的统一退出标准，可以按本计划口径计入“已完成”
  - 允许仍存在收益递减的后续细化项，但这些细化项不再阻塞当前执行包验收
- `[部分完成]`
  - 表示已经有明确的物理拆分、职责下沉或目录迁移成果
  - 但至少还有一项 `3.2` 退出标准尚未满足，或薄入口目标尚未成立，因此不得标记为“已完成”
- `[未开始]`
  - 表示尚未发生足以改变模块责任边界的实际落地实现

补充约束：

- 本文后续凡写“完成”，默认指 `[已验收完成]`
- 如果只是“关键 helper 已拆出”“主体已落地”“目录已迁移一部分”，必须显式标记为 `[部分完成]`
- 第二阶段目录迁移的验收只代表“目录布局目标在当前约定范围内落地”，不自动等同于对应第一阶段执行包已经 `[已验收完成]`

## 4. 第一阶段：逻辑拆分执行包

第一阶段只新增同目录文件，不优先新建子目录，以降低构建和 include 调整成本。

### 4.1 执行包 A：测试聚合文件拆解

目标文件：

- `src/test/suite/client.integration.groups.ts`

目标：

- 将“所有 suite 的实现都塞在一个文件中”的状态，改成“按架构域分拆实现 + 薄聚合入口”的状态。

建议拆分：

- `src/test/suite/client.integration.runtime-config.ts`
- `src/test/suite/client.integration.interactive-core.ts`
- `src/test/suite/client.integration.interactive-signature.ts`
- `src/test/suite/client.integration.diagnostics.ts`
- `src/test/suite/client.integration.deferred-doc.ts`
- `src/test/suite/client.integration.analysis-context.ts`
- `src/test/suite/client.integration.workspace-summary.ts`
- `src/test/suite/client.integration.references-rename.ts`
- 保留 `src/test/suite/client.integration.groups.ts` 作为薄聚合导出入口

边界要求：

- `*.test.ts` 入口壳文件先保留，不调整测试发现机制
- 各实现文件按“一个测试主题域”聚合，不再交叉承载多种场景
- 共享 helper 继续沉淀到 `test_helpers.ts`，不要在拆分后把相同等待逻辑重新手写多份

最小验证：

- `npm run compile`

验收判据：

- `client.integration.groups.ts` 已退化成薄聚合入口，不再承载大段测试实现
- `*.test.ts` 入口壳仍保留，测试发现机制未被顺手改写
- 测试主题实现已按架构域拆到独立文件，而不是把旧文件简单横切成更多混合主题文件
- 共享等待 / 配置 / 断言 helper 未在新文件里重复手写，仍优先沉淀到 `test_helpers.ts`
- 最小验证已记录；如测试组织方式已成为维护事实，`docs/testing.md` 已同步

何时需要同步事实文档：

- 如果测试入口组织方式、推荐定向运行方式、helper 约定变化到会影响维护流程时，同步更新 `docs/testing.md`

### 4.2 执行包 B：request handler 拆解

目标文件：

- `server_cpp/src/server_request_handlers.cpp`

保留入口：

- `server_cpp/src/server_request_handlers.hpp`
- `server_cpp/src/server_request_handlers.cpp`

建议新增：

- `server_cpp/src/server_request_handler_common.hpp`
- `server_cpp/src/server_request_handler_common.cpp`
- `server_cpp/src/server_request_completion.cpp`
- `server_cpp/src/server_request_hover.cpp`
- `server_cpp/src/server_request_definition.cpp`
- `server_cpp/src/server_request_signature_help.cpp`
- `server_cpp/src/server_request_references_rename.cpp`
- `server_cpp/src/server_request_symbols.cpp`

职责边界：

- `server_request_handlers.cpp`
  - 只保留 method dispatch、handler 表、极少量请求层公共 metrics
- `server_request_handler_common.*`
  - 仅承载多个 request family 共享、且属于 request 层的 helper
- 各 `server_request_*.cpp`
  - 只处理对应 LSP 能力，不承载其他 request family 的具体逻辑

禁止项：

- 不要把共享语义规则从 `interactive_semantic_runtime.*`、`workspace_summary_runtime.*`、`member_query.*`、`symbol_query.*` 复制到 request 文件
- 不要新建 request 层的“万能 utils”并再次吸纳跨职责逻辑

最小验证：

- `cmake --build .\\server_cpp\\build`
- `npm run test:client:repo`

验收判据：

- `server_request_handlers.cpp` 已收敛为薄入口，只保留 dispatch、handler 表和极少量 request 层公共 metrics
- 具体 LSP family 已拆到独立实现文件，且每个文件不再承载其他 request family 的主体逻辑
- request 层共享 helper 仅保留 request 边界内的公共能力，没有重新长成“万能 utils”
- `server_request_handlers.hpp` 已能直接表达当前 request layer 调度边界与调用前提
- 最小验证已记录；如 request layer 事实边界发生变化，`docs/architecture.md` 已同步

何时需要同步事实文档：

- 如果 request layer 职责边界、调度契约、热路径查询顺序发生变化，同步更新 `docs/architecture.md`

### 4.3 执行包 C：diagnostics 拆解

目标文件：

- `server_cpp/src/diagnostics.cpp`

保留入口：

- `server_cpp/src/diagnostics.hpp`
- `server_cpp/src/diagnostics.cpp`

建议新增：

- `server_cpp/src/diagnostics_internal.hpp`
- `server_cpp/src/diagnostics_preprocessor.cpp`
- `server_cpp/src/diagnostics_expression_type.cpp`
- `server_cpp/src/diagnostics_symbol_type.cpp`
- `server_cpp/src/diagnostics_rules_syntax.cpp`
- `server_cpp/src/diagnostics_rules_semantic.cpp`
- `server_cpp/src/diagnostics_emit.cpp`

职责边界：

- `diagnostics.cpp`
  - 仅保留 facade、阶段装配与总入口
- `diagnostics_preprocessor.cpp`
  - 仅负责 diagnostics 使用的预处理视图构建和相关 helper
- `diagnostics_expression_type.cpp`
  - 仅负责表达式类型推导
- `diagnostics_symbol_type.cpp`
  - 仅负责符号与成员类型查询的辅助逻辑
- `diagnostics_rules_syntax.cpp`
  - 仅负责括号、注释、基础 syntax 类规则收集
- `diagnostics_rules_semantic.cpp`
  - 仅负责语义规则收集
- `diagnostics_emit.cpp`
  - 仅负责 diagnostic JSON 生成、code / reason data 装配

禁止项：

- 不要顺手把 `immediate_syntax_diagnostics.*` 的职责并过来
- 不要把 `type_eval.*`、`type_model.*` 里的共享事实再次抄到 diagnostics 子模块

最小验证：

- `cmake --build .\\server_cpp\\build`
- `npm run test:client:repo`

验收判据：

- `diagnostics.cpp` 已收敛成 facade / build orchestration 入口，不再继续吸纳具体规则实现
- 表达式类型、符号类型、语义规则、syntax 规则、emit / reason data 组装等责任已拆到独立模块
- 未把 `immediate_syntax_diagnostics.*` 或其他共享事实重新并回 diagnostics 子模块
- 相关 `*.hpp` 已能说明 diagnostics 分层边界与关键输入输出
- 最小验证已记录；若 diagnostics 分层或验证建议已经成为维护事实，`docs/architecture.md` / `docs/testing.md` 已同步

补充验证建议：

- 如果改动触及 full diagnostics 调度、cache 或性能路径，再补 `npm run test:client:perf`

何时需要同步事实文档：

- 如果 diagnostics 分层、共享入口或推荐验证矩阵变化，同步更新 `docs/architecture.md` 和 `docs/testing.md`

### 4.4 执行包 D：workspace index 拆解

目标文件：

- `server_cpp/src/workspace_index.cpp`

保留入口：

- `server_cpp/src/workspace_index.hpp`
- `server_cpp/src/workspace_index.cpp`

建议新增：

- `server_cpp/src/workspace_index_internal.hpp`
- `server_cpp/src/workspace_index_cache.cpp`
- `server_cpp/src/workspace_index_scan.cpp`
- `server_cpp/src/workspace_index_extract.cpp`
- `server_cpp/src/workspace_index_reverse_include.cpp`
- `server_cpp/src/workspace_index_scheduler.cpp`

职责边界：

- `workspace_index.cpp`
  - 仅保留 facade 与对外 API 转发
- `workspace_index_cache.cpp`
  - 仅负责磁盘 cache、序列化、反序列化、版本兼容清理
- `workspace_index_scan.cpp`
  - 仅负责文件扫描、路径过滤、基础 IO
- `workspace_index_extract.cpp`
  - 仅负责从文本提取 definitions / struct / include 等索引事实
- `workspace_index_reverse_include.cpp`
  - 仅负责 include closure / reverse include closure 相关构建与查询
- `workspace_index_scheduler.cpp`
  - 仅负责后台 worker、latest request 合并、重排和状态流转

禁止项：

- 不要把 `workspace_summary_runtime.*` 的运行时边界并回 index 子模块
- 不要为了拆分而改变“summary-first”的对外查询语义

最小验证：

- `cmake --build .\\server_cpp\\build`
- `npm run test:client:repo`

验收判据：

- `workspace_index.cpp` 已收敛为 facade 与对外 API 转发，不再长期承载 cache / scan / scheduler / rebuild 主流程
- cache、scan、extract、reverse include、scheduler 至少按主责任拆分到稳定子模块
- 对外仍保持 summary-first 语义，没有因为拆分把 `workspace_summary_runtime.*` 的边界并回去
- `workspace_index.hpp` 已说明索引边界、后台职责和非目标范围
- 最小验证已记录；若 indexing / reverse-include / workspace-summary 事实边界变化，`docs/architecture.md` / `docs/testing.md` 已同步

补充验证建议：

- 如果触及 latest-only、调度优先级、文件 watch 回流或 metrics，再补 `npm run test:client:perf`

何时需要同步事实文档：

- 如果 workspace summary / reverse include / indexing 事实边界变化，同步更新 `docs/architecture.md` 和 `docs/testing.md`

### 4.5 执行包 E：client extension 拆解

目标文件：

- `client/src/extension.ts`

保留入口：

- `client/src/extension.ts`

建议新增：

- `client/src/client_runtime_host.ts`
- `client/src/client_config_sync.ts`
- `client/src/client_commands.ts`
- `client/src/client_status_ui.ts`
- `client/src/client_metrics.ts`
- `client/src/client_indexing_status.ts`

职责边界：

- `extension.ts`
  - 仅保留 activate / deactivate 与装配
- `client_runtime_host.ts`
  - 仅负责语言客户端启动、重启、生命周期
- `client_config_sync.ts`
  - 仅负责配置读取与同步
- `client_commands.ts`
  - 仅负责命令注册与命令入口
- `client_status_ui.ts`
  - 仅负责状态栏与用户可见 UI 状态
- `client_metrics.ts`
  - 仅负责 metrics 事件消费与展示辅助
- `client_indexing_status.ts`
  - 仅负责 indexing 状态追踪

禁止项：

- 不要在 client 侧重新定义 server 语义真相
- 不要把 editor shell 能力与 LSP runtime 协议逻辑再次耦合到一个模块

最小验证：

- `npm run compile`
- 如果行为受影响，再补 `npm run test:client:repo`

验收判据：

- `extension.ts` 已收敛为 activate / deactivate 与装配入口，不再长期承载 startup、provider、命令、状态 UI、配置同步等多类主体逻辑
- client runtime host、config sync、commands、status UI、metrics、indexing status 的责任已稳定落到独立模块
- client 侧未趁拆分引入新的 server 语义真相或重新耦合 editor shell 与 LSP runtime 协议逻辑
- 最小验证已记录；如 client/runtime 职责描述已成为维护事实，`README.md` / `docs/architecture.md` 已同步

何时需要同步事实文档：

- 如果 client/server 职责边界或 client 运行时行为描述变化，同步更新 `README.md` 和 `docs/architecture.md`

### 4.6 执行包 F：main.cpp 缩薄

目标文件：

- `server_cpp/src/main.cpp`

处理策略：

- 不把 `main.cpp` 当成新架构中心重写
- 只把明显不属于“进程入口 / 顶层消息编排”的 helper 下沉

建议新增：

- `server_cpp/src/main_did_change_classification.cpp`
- `server_cpp/src/main_occurrence_helpers.cpp`
- `server_cpp/src/main_include_graph_cache.cpp`
- `server_cpp/src/main_background_refresh.cpp`

职责边界：

- `main.cpp`
  - 仅保留进程启动、LSP 消息循环、顶层 wiring、必须位于入口的全局协调
- 新子文件
  - 分别承载 didChange 分类、occurrence 辅助、include graph cache 辅助、后台刷新辅助

禁止项：

- 不要借这个阶段顺手改动 request 调度或 runtime 真相来源
- 不要把更多职责重新并回 `main.cpp`

最小验证：

- `cmake --build .\\server_cpp\\build`
- 视行为影响补 `npm run test:client:repo`

验收判据：

- `main.cpp` 已收敛为进程入口、消息循环、顶层 wiring 和必须位于入口的全局协调
- didChange 分类、occurrence helper、include graph cache、后台刷新等非入口型逻辑已稳定下沉
- 未借拆分机会顺手改变 request 调度边界或 runtime 真相来源
- 最小验证已记录；如 server 启动职责、调度边界或上下文回流路径变化，`docs/architecture.md` 已同步

何时需要同步事实文档：

- 如果 server 启动职责、调度边界、分析上下文回流路径发生变化，同步更新 `docs/architecture.md`

## 5. 第二阶段：目录布局目标

当第一阶段的逻辑边界稳定后，再考虑目录整理。此阶段不是必选项，只有在满足“收益明显大于迁移成本”时才执行。

建议目标布局：

```text
server_cpp/src/
  app/
    main.cpp
    main_background_refresh.cpp
    main_did_change_classification.cpp
  requests/
    server_request_handlers.cpp
    server_request_handler_common.cpp
    server_request_completion.cpp
    server_request_hover.cpp
    server_request_definition.cpp
    server_request_signature_help.cpp
    server_request_references_rename.cpp
    server_request_symbols.cpp
  diagnostics/
    diagnostics.cpp
    diagnostics_preprocessor.cpp
    diagnostics_expression_type.cpp
    diagnostics_symbol_type.cpp
    diagnostics_rules_syntax.cpp
    diagnostics_rules_semantic.cpp
    diagnostics_emit.cpp
  workspace/
    workspace_index.cpp
    workspace_index_cache.cpp
    workspace_index_scan.cpp
    workspace_index_extract.cpp
    workspace_index_reverse_include.cpp
    workspace_index_scheduler.cpp
```

```text
src/test/suite/
  integration/
    runtime-config.ts
    interactive-core.ts
    interactive-signature.ts
    diagnostics.ts
    deferred-doc.ts
    analysis-context.ts
    workspace-summary.ts
    references-rename.ts
```

```text
client/src/
  extension.ts
  client_runtime_host.ts
  client_config_sync.ts
  client_commands.ts
  client_status_ui.ts
  client_metrics.ts
  client_indexing_status.ts
```

第二阶段前置条件：

- 第一阶段拆出的职责边界已稳定
- 没有再出现“新文件刚拆完又马上跨职责回流”的情况
- 团队已确认接受额外的 CMake / import / include 路径调整成本

## 6. 必须停下来确认的触发点

在执行本方案时，出现以下任一情况，必须先停下来确认：

- 需要改变公开行为，而不是单纯做物理拆分
- 需要改变共享模块的单一事实来源
- 需要新增 fallback、compat、adapter、双路径保留旧逻辑
- 发现当前大文件其实承载的是“一个复杂责任”，拆分会迫使新建很多没有清晰边界的小模块
- 需要同时改动 request layer、runtime layer、workspace summary 语义，已经超出“最小拆分”范围
- 需要改资源路径、加载规则、bundle 约定

## 7. 推荐任务粒度

为了让人和 AI 都能稳定推进，建议每次任务只完成一个执行包里的一个子目标。

推荐粒度示例：

- 只拆 `server_request_handlers.cpp` 中的 `hover + definition`
- 只拆 `diagnostics.cpp` 中的 emit / output 组装
- 只拆 `workspace_index.cpp` 中的 cache 序列化部分
- 只拆 `client.integration.groups.ts` 中 analysis-context 相关 suite

不推荐粒度：

- 一次任务同时拆 request handler、workspace index、diagnostics
- 一次任务同时做逻辑拆分和目录迁移
- 一次任务同时做代码拆分、行为调整、测试体系重排

## 8. 验证矩阵

| 执行包 | 最小验证 | 何时补充验证 |
| --- | --- | --- |
| 测试聚合拆解 | `npm run compile` | 测试入口行为变化时补 repo 测试 |
| request handler 拆解 | `cmake --build .\\server_cpp\\build` + `npm run test:client:repo` | 调度/性能路径变化时补 `npm run test:client:perf` |
| diagnostics 拆解 | `cmake --build .\\server_cpp\\build` + `npm run test:client:repo` | full diagnostics / cache / 性能变化时补 perf |
| workspace index 拆解 | `cmake --build .\\server_cpp\\build` + `npm run test:client:repo` | latest-only / watch / reverse include / metrics 变化时补 perf |
| client extension 拆解 | `npm run compile` | 行为变化时补 `npm run test:client:repo` |
| main.cpp 缩薄 | `cmake --build .\\server_cpp\\build` | 行为变化时补 `npm run test:client:repo` |

### 8.1 当前复核状态矩阵（2026-03-28）

| 执行包 | 当前状态 | 主要缺口 / 当前判断依据 | 最近复核验证 |
| --- | --- | --- | --- |
| A 测试聚合文件拆解 | `[已验收完成]` | 薄聚合入口已成立，测试主题已按架构域拆分 | `npm run compile`、`npm run test:client:repo` |
| B request handler 拆解 | `[已验收完成]` | `server_request_handlers.cpp` 已基本只保留 dispatch + 少量 metrics | `cmake --build .\\server_cpp\\build`、`npm run test:client:repo` |
| C diagnostics 拆解 | `[已验收完成]` | facade 入口已成立，diagnostics 分层已写回事实文档 | `cmake --build .\\server_cpp\\build`、`npm run test:client:repo` |
| D workspace index 拆解 | `[已验收完成]` | `workspace_index.cpp` 已收敛为 facade / query / 最小 owner wiring；cache / scan / scheduler / extract / reverse include 已拆出 | `cmake --build .\\server_cpp\\build`、`npm run test:client:repo` |
| E client extension 拆解 | `[已验收完成]` | `extension.ts` 已收敛为 activate / lifecycle wiring / 高层装配；runtime host / active unit / editor feedback / events / status commands 已拆出 | `npm run compile`、`npm run test:client:repo` |
| F main.cpp 缩薄 | `[已验收完成]` | `main.cpp` 已收敛为进程入口 / 消息循环 / 顶层协调；diagnostics background queue / worker runtime 已下沉到 `main_background_refresh.*` | `cmake --build .\\server_cpp\\build`、`npm run test:client:repo` |

## 9. 文档同步规则

本方案本身位于 `docs/human-ai/`，不属于当前事实。

因此：

- 仅新增本提案或调整其阶段安排时，不要求同步修改事实文档
- 一旦某个执行包实际落地，并改变了模块边界、测试策略、命令、公开行为或接口契约，就必须在同一次实现任务里同步更新事实文档

最低检查项：

- `README.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
- 如涉及接口契约或 AI 维护约束，再检查 `AGENTS.md`

## 10. 推荐首批落地顺序

如果要从明天开始实际执行，推荐首批任务按下面顺序开：

1. 拆 `client.integration.groups.ts`
2. 拆 `server_request_handlers.cpp` 的 `hover / definition / signature help`
3. 拆 `diagnostics.cpp` 的 emit 与 syntax rules
4. 拆 `workspace_index.cpp` 的 cache 与 reverse include

原因：

- 这四步的收益最高
- 对现有单一事实来源冲击最小
- 可以尽早降低后续 AI 修改核心路径时的上下文负担

## 11. 预期收益与完成判据

如果本方案执行有效，应该出现以下结果：

- 单次任务涉及的核心文件数量下降，但每个文件的职责更清楚
- AI 在修改核心功能时，读取的无关代码显著减少
- review 更容易围绕“职责是否正确”而不是“有没有碰到其他分支”
- 新增功能更容易沿现有共享模块扩展，而不是继续堆到历史大文件
- 测试维护开始按架构域推进，而不是继续把所有集成断言往一个大文件里塞

阶段性完成判据不是“文件数量变多”，而是：

- 薄入口成立
- 子模块主责任单一
- 头文件契约更清楚
- 验证矩阵更稳定
- 没有引入新的事实漂移或局部规则复制

## 12. 进度快照（2026-03-28，含验收标注）

本节是协作交接快照，不属于当前事实来源，只用于帮助后续线程延续当前拆分工作。

阅读本节时，统一按 `3.2` 与 `3.3` 解释：

- “已拆出 helper / 已发生目录迁移”不自动等于 `[已验收完成]`
- 只有满足当前执行包退出标准时，才能写成 `[已验收完成]`

### 12.1 `[已验收完成]` 执行包 A：测试聚合文件拆解

- `src/test/suite/client.integration.groups.ts`
  - 已从约 `4306` 行收敛到约 `43` 行。
  - 当前已退化成 barrel / re-export 入口。
  - 测试主题实现已拆到：
    - `src/test/suite/integration/analysis-context.ts`
    - `src/test/suite/integration/deferred-doc.ts`
    - `src/test/suite/integration/diagnostics.ts`
    - `src/test/suite/integration/interactive-core.ts`
    - `src/test/suite/integration/interactive-signature.ts`
    - `src/test/suite/integration/interactive-support.ts`
    - `src/test/suite/integration/references-rename.ts`
    - `src/test/suite/integration/runtime-config.ts`
    - `src/test/suite/integration/ui-metadata.ts`
    - `src/test/suite/integration/workspace-summary.ts`
  - 当前判断为 `[已验收完成]` 的原因：
    - 薄聚合入口已成立
    - `*.test.ts` 入口壳保留，测试发现机制未改变
    - `docs/testing.md` 已同步说明 registrar 与 `integration/*.ts` 布局

### 12.2 `[已验收完成]` 执行包 B：request handler 拆解

- `server_cpp/src/requests/server_request_handlers.cpp`
  - 已从约 `3186` 行收敛到约 `200` 行量级。
  - 当前只保留 handler 表、method dispatch 与少量 signature help metrics。
  - 已拆出的 request family：
    - `server_cpp/src/requests/server_request_handler_background.cpp`
    - `server_cpp/src/requests/server_request_handler_completion.cpp`
    - `server_cpp/src/requests/server_request_handler_definition.cpp`
    - `server_cpp/src/requests/server_request_handler_hover.cpp`
    - `server_cpp/src/requests/server_request_handler_references.cpp`
    - `server_cpp/src/requests/server_request_handler_signature.cpp`
    - 共享私有头：`server_cpp/src/server_request_handler_common.hpp`
  - 当前判断为 `[已验收完成]` 的原因：
    - 薄入口已成立
    - 请求层契约已写回 `server_request_handlers.hpp`
    - `docs/architecture.md` 已同步反映 request 目录与职责边界

### 12.3 `[已验收完成]` 执行包 C：diagnostics 拆解

- `server_cpp/src/diagnostics/diagnostics.cpp`
  - 当前约 `261` 行，已收敛成 facade / build orchestration 入口。
  - 已拆出的外围 / 中层模块：
    - `server_cpp/src/diagnostics/diagnostics_emit.cpp`
    - `server_cpp/src/diagnostics/diagnostics_expression_type.cpp`
    - `server_cpp/src/diagnostics/diagnostics_io.cpp`
    - `server_cpp/src/diagnostics/diagnostics_preprocessor.cpp`
    - `server_cpp/src/diagnostics/diagnostics_syntax.cpp`
    - `server_cpp/src/diagnostics/diagnostics_indeterminate.cpp`
    - `server_cpp/src/diagnostics/diagnostics_semantic_common.cpp`
    - `server_cpp/src/diagnostics/diagnostics_semantic.cpp`
    - `server_cpp/src/diagnostics/diagnostics_symbol_type.cpp`
  - 当前判断为 `[已验收完成]` 的原因：
    - facade 入口已成立
    - 事实文档已同步 diagnostics 分层
    - 剩余工作主要是收益递减的进一步细分，不阻塞当前执行包验收
  - 后续细化不影响当前验收：
    - 可继续把 `diagnostics_semantic.cpp` 内剩余 semantic rules 再按 call / undefined-symbol / narrowing 等子责任拆开
    - 可继续评估 diagnostics 内部共享 helper 是否还要收敛成更明确的 internal contract

### 12.4 `[已验收完成]` 执行包 D：workspace index 拆解

- `server_cpp/src/workspace/workspace_index.cpp`
  - 当前约 `2076` 行。
  - 已拆出的模块：
    - `server_cpp/src/workspace/workspace_index_cache.cpp`
    - `server_cpp/src/workspace/workspace_index_scan.cpp`
    - `server_cpp/src/workspace/workspace_index_scheduler.cpp`
    - `server_cpp/src/workspace/workspace_index_extract.cpp`
    - `server_cpp/src/workspace/workspace_index_reverse_include.cpp`
    - `server_cpp/src/workspace/workspace_index_internal.cpp`
  - 当前仍保留：
    - facade / summary query
    - 最小 owner wiring 与 indexing state snapshot/publish
  - 当前判断为 `[已验收完成]` 的原因：
    - cache、scan、scheduler、extract、reverse include 已按主责任拆到稳定子模块
    - `workspace_index.cpp` 已不再长期承载 cache / scan / scheduler / rebuild 主流程
    - `docs/architecture.md` 已同步反映新的模块边界
  - 2026-03-28 进展：
    - `D1` 已完成：cache 路径、磁盘 load/save 与旧索引兼容迁移已下沉到 `workspace_index_cache.cpp`
    - `D2` 已完成：path 归一化、include-closure 扫描与 file-to-meta 解析 helper 已下沉到 `workspace_index_scan.cpp`
    - `D3` 已完成：rebuild / file-watch update / 后台线程与并行索引调度已下沉到 `workspace_index_scheduler.cpp`
    - 执行包 D 到此可按本计划口径记为 `[已验收完成]`

### 12.5 `[已验收完成]` 执行包 E：client extension 拆解

- `client/src/extension.ts`
  - 当前约 `509` 行。
  - 已拆出的 client 辅助模块：
    - `client/src/client_runtime_host.ts`
    - `client/src/client_active_unit.ts`
    - `client/src/client_editor_feedback.ts`
    - `client/src/client_editor_events.ts`
    - `client/src/client_runtime_events.ts`
    - `client/src/client_status_commands.ts`
    - `client/src/client_config_sync.ts`
    - `client/src/client_indexing_status.ts`
    - `client/src/client_internal_commands.ts`
    - `client/src/client_metrics.ts`
    - `client/src/client_status_ui.ts`
    - `client/src/client_user_commands.ts`
    - `client/src/client_watched_files.ts`
  - 当前仍主要保留：
    - activate / deactivate
    - client 状态所有权与高层装配
  - 当前判断为 `[已验收完成]` 的原因：
    - runtime host、active unit、editor feedback、editor/runtime events、status command 实现都已下沉到独立模块
    - `extension.ts` 已不再长期承载 startup / provider / editor event / 命令主体逻辑
    - `docs/architecture.md` 已同步反映新的 client 模块边界
  - 2026-03-28 进展：
    - `E1` 已完成：LanguageClient 启动 / 重启 / `onReady` lifecycle 已下沉到 `client_runtime_host.ts`
    - `E2` 已完成：editor/document 事件注册与 runtime config / watcher / 配置事件注册已下沉到 `client_editor_events.ts`、`client_runtime_events.ts`
    - `E3` 已完成：active unit、editor feedback / inlay provider 与 status command 实现已下沉到 `client_active_unit.ts`、`client_editor_feedback.ts`、`client_status_commands.ts`
    - 执行包 E 到此可按本计划口径记为 `[已验收完成]`

### 12.6 `[已验收完成]` 执行包 F：main.cpp 缩薄

- `server_cpp/src/app/main.cpp`
  - 当前约 `1859` 行。
  - 已拆出的模块：
    - `server_cpp/src/app/main_background_refresh.cpp`
    - `server_cpp/src/app/main_did_change_classification.cpp`
    - `server_cpp/src/app/main_include_graph_cache.cpp`
    - `server_cpp/src/app/main_occurrence_helpers.cpp`
  - 当前仍保留：
    - server 入口与消息循环
    - request enqueue / dispatch 编排
    - 顶层消息协调
  - 当前判断为 `[已验收完成]` 的原因：
    - diagnostics background queue / worker runtime 已从入口文件下沉到 `main_background_refresh.*`
    - `main.cpp` 当前主要剩余进程入口、消息循环与顶层协调逻辑
    - `docs/architecture.md` 已同步反映新的 server 入口边界
  - 2026-03-28 进展：
    - `F1` 已完成：diagnostics background queue / latest-only 调度 / worker runtime 已下沉到 `main_background_refresh.*`
    - 执行包 F 到此可按本计划口径记为 `[已验收完成]`

### 12.7 `[已验收完成（目录迁移范围）]` 第二阶段状态

- 第二阶段目录迁移已完成到当前稳定收益范围。
- 当前已落地的目录布局包括：
  - `server_cpp/src/app/*.cpp`
  - `server_cpp/src/requests/*.cpp`
  - `server_cpp/src/diagnostics/*.cpp`
  - `server_cpp/src/workspace/*.cpp`
  - `src/test/suite/integration/*.ts`
- client 侧当前事实仍保留 `client/src/extension.ts` 与同级 helper 文件布局，因为该部分在本计划里本就没有要求继续下沉到子目录。
- 该标注只代表“目录迁移目标已在当前约定范围内落地”，不代表执行包 D / E / F 已自动转为 `[已验收完成]`

### 12.8 最近验证结果（2026-03-28 复核）

- 以下命令已再次实际运行并通过：
  - `npm run compile`
  - `cmake --build .\\server_cpp\\build`
  - `npm run test:client:repo`

### 12.9 后续可选入口

如果后续仍想继续做更细颗粒度的内部瘦身，推荐顺序：

1. 继续把 `server_cpp/src/diagnostics/diagnostics_semantic.cpp` 细分成更小的 semantic rule 文件
2. 再评估 `server_cpp/src/workspace/workspace_index.cpp` 内剩余 owner / scheduler 逻辑是否值得继续细拆
3. 视收益再决定是否继续缩薄 `client/src/extension.ts` 与 `server_cpp/src/app/main.cpp`
4. 视收益再决定是否移动头文件到与实现目录一致的子目录层级

原因：

- 当前真正 `[已验收完成]` 的是一批高收益执行包，而不是“第一阶段全部完成”
- 后续收益更偏向局部减负与可维护性提升，而不是补齐基础目录结构
- 优先继续处理 diagnostics / workspace index / extension / main 的剩余厚入口，收益高于再次扩大目录迁移范围

## 13. 当前执行方案（2026-03-28）

本节给出从当前状态继续推进的实际执行顺序。目标不是“把所有剩余文件一次性拆完”，而是把当前 `[部分完成]` 的执行包逐个推进到可验收状态。

### 13.1 执行原则

- 一次任务只推进一个执行包里的一个子步骤，不在同一轮里并行改 D / E / F
- 每个子步骤结束后，都先按对应执行包的“验收判据”复核，再决定是否继续下一个子步骤
- 如果某个子步骤已经足以满足该执行包验收，就停止，不为了目录整齐继续扩拆
- 如果拆分过程中发现必须改动 request 调度边界、runtime 真相来源或公开行为，立即回到第 6 节停下来确认

### 13.2 推荐执行顺序

推荐顺序：

1. 第一阶段执行包已完成
2. 后续仅在收益明显时继续做第二阶段目录细化或更小颗粒度瘦身

排序原因：

- D / E / F 当前都已达到本计划的验收口径
- 后续继续拆分已进入收益递减区，应按真实收益再决定是否继续

### 13.3 执行包 D：workspace index 收尾方案

目标：

- 把当前 `[部分完成]` 推进到 `[已验收完成]`
- 让 `workspace_index.cpp` 不再长期承载 cache / scan / scheduler / rebuild 主流程

当前进度：

- `D1` 已完成
- `D2` 已完成
- `D3` 已完成
- 执行包 D 已验收完成；后续继续按 `13.4` 从执行包 E 开始

推荐子步骤：

1. `D1`：提取 cache 责任
   - 目标文件：`server_cpp/src/workspace/workspace_index.cpp`
   - 建议落点：`server_cpp/src/workspace/workspace_index_cache.cpp`
   - 作用域：磁盘 cache 路径、序列化、反序列化、版本兼容清理
   - 停止条件：cache 主流程已不再留在 `workspace_index.cpp`
   - 最小验证：`cmake --build .\\server_cpp\\build`

2. `D2`：提取 scan / IO 责任
   - 目标文件：`server_cpp/src/workspace/workspace_index.cpp`
   - 建议落点：`server_cpp/src/workspace/workspace_index_scan.cpp`
   - 作用域：文件扫描、路径过滤、include closure 相关基础 IO
   - 停止条件：scan / path / IO 主体已从 `workspace_index.cpp` 移出
   - 最小验证：`cmake --build .\\server_cpp\\build`

3. `D3`：提取 scheduler / rebuild 责任
   - 目标文件：`server_cpp/src/workspace/workspace_index.cpp`
   - 建议落点：`server_cpp/src/workspace/workspace_index_scheduler.cpp`
   - 作用域：后台线程、pending rebuild、latest request 合并、状态流转
   - 停止条件：`workspace_index.cpp` 只剩 facade、最小 owner wiring 与 API 转发
   - 最小验证：`cmake --build .\\server_cpp\\build` + `npm run test:client:repo`

执行包 D 验收复核：

- 若 `workspace_index.cpp` 仍然保留 cache / scan / scheduler 主流程，则继续保持 `[部分完成]`
- 若已收敛为 facade + 最小必要入口，并满足 `4.4` 验收判据，则改记为 `[已验收完成]`
- 如果索引边界、reverse include 或 testing 约定已经变化，同步更新 `docs/architecture.md` / `docs/testing.md`

### 13.4 执行包 E：client extension 收尾方案

目标：

- 把 `extension.ts` 从“已经拆出 helper 的大入口”推进到“以装配为主的入口”

推荐子步骤：

1. `E1`：提取 runtime host
   - 目标文件：`client/src/extension.ts`
   - 建议落点：`client/src/client_runtime_host.ts`
   - 作用域：LanguageClient 启动、重启、onReady、生命周期
   - 停止条件：`ensureClientStarted`、client startup / restart 主体已从 `extension.ts` 移出
   - 最小验证：`npm run compile`

2. `E2`：提取 editor / runtime event 注册主体
   - 目标文件：`client/src/extension.ts`
   - 建议落点：
      - 优先复用已拆出的现有 client helper
      - 如现有 helper 无法承载，再新增一个职责明确的 registration 模块，而不是继续堆回 `extension.ts`
   - 作用域：editor/document 事件注册、runtime config / watcher / 配置事件注册，以及与运行时宿主解耦的注册逻辑
   - 停止条件：上述事件注册主体已离开 `extension.ts`
   - 最小验证：`npm run compile`，必要时补 `npm run test:client:repo`

3. `E3`：提取剩余的 inlay provider 与 editor feedback 主体
   - 目标文件：`client/src/extension.ts`
   - 建议落点：
     - 优先新增一个职责明确的 provider / feedback 模块
     - 不要把 provider、状态栏、配置同步再次耦合进同一新文件
   - 作用域：inlay provider、include underline / inlay refresh 调度、剩余 editor feedback 主体
   - 停止条件：`extension.ts` 只剩 activate / deactivate 与高层装配
   - 最小验证：`npm run compile`，必要时补 `npm run test:client:repo`

执行包 E 验收复核：

- 若 `extension.ts` 仍持有 startup / provider / editor event 主体，则继续保持 `[部分完成]`
- 若已收敛为装配入口，并满足 `4.5` 验收判据，则改记为 `[已验收完成]`
- 如果 client 运行时职责描述已变化，同步更新 `README.md` / `docs/architecture.md`

当前进度：

- `E1` 已完成
- `E2` 已完成（editor/document 事件注册与 runtime config / watcher / 配置事件注册已下沉）
- `E3` 已完成（active unit、editor feedback / inlay provider 与 status command 实现已下沉）
- 执行包 E 已验收完成

### 13.5 执行包 F：main.cpp 收尾方案

目标：

- 继续缩薄 `main.cpp`，但不越过“server 顶层入口 / 调度边界”的安全线

推荐子步骤：

1. `F1`：提取 diagnostics background / publish 主体
   - 目标文件：`server_cpp/src/app/main.cpp`
   - 建议落点：`server_cpp/src/app/main_background_refresh.cpp`
   - 作用域：diagnostics queue、publish helper、background refresh 相关实现
   - 停止条件：diagnostics queue / publish 主体不再长期留在 `main.cpp`
   - 最小验证：`cmake --build .\\server_cpp\\build` + `npm run test:client:repo`

2. `F2`：复核是否还需要继续下沉
   - 目标文件：`server_cpp/src/app/main.cpp`
   - 作用域：只做“是否继续”的复核，不默认继续拆
   - 判断标准：
     - 如果 `main.cpp` 已主要剩下进程入口、消息循环、顶层 wiring，则停止
     - 如果仍残留明显不属于入口职责的大块 helper，再开下一条最小子任务

执行包 F 验收复核：

- 若 `main.cpp` 仍然持有大量 diagnostics queue / request 编排主体，则继续保持 `[部分完成]`
- 若已收敛为入口与顶层协调，并满足 `4.6` 验收判据，则改记为 `[已验收完成]`
- 如果拆分触碰 request 调度边界或分析上下文回流路径，先停下来确认，再决定是否继续

当前进度：

- `F1` 已完成（diagnostics background queue / latest-only 调度 / worker runtime 已下沉）
- 执行包 F 已验收完成

### 13.6 每轮任务完成后的固定动作

每完成一轮子步骤，固定执行：

1. 对照对应执行包的“验收判据”判断状态是否变化
2. 更新 `12.x` 进度快照和 `8.1` 状态矩阵
3. 只记录本轮实际跑过的最小验证，不预支未来验证结果
4. 如果模块事实边界已变化，同步更新事实文档；否则只更新本提案
