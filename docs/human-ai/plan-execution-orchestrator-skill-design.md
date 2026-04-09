# Plan Execution Orchestrator Skill Design

重要说明：

- 本文档是人类与 AI 协作设计稿，不是当前事实文档。
- 本文档位于 `docs/human-ai/`，用于沉淀一个新的执行型 skill 设计。
- 当前事实仍以 `README.md`、`AGENTS.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md` 为准。
- 本文档描述的是“在已有计划存在时，如何通过多 subagent 执行、审查、验证并收敛”的设计，不等于当前仓库已经具备该能力。

## 1. 背景与目标

当前仓库已经有较强的仓库级约束与执行 guardrails：

- `AGENTS.md` 约束最小改动、单一事实来源、必须确认边界、验证梯度、文档同步规则
- `docs/human-ai/skills/nsf-repo-execution/` 提供 repo-specific evidence-first 执行约束
- 外部已有 `subagent-driven-development`、`requesting-code-review`、`verification-before-completion` 等 skill 可复用

但在“用户已经明确给出 plan，并要求按方案执行”这一类任务中，仍缺少一个总控型 skill 来统一完成：

1. 读取计划并拆分执行单元
2. 对可并行任务派发多个 implementer subagent
3. 在实现后并行做架构审查与 QA/验证
4. 对代码级问题自动进入下一轮修复
5. 对高风险架构/公开行为问题自动停止并升级给用户
6. 在长链路执行中自动沉淀 progress/handoff
7. 在结束时给出 evidence-backed 的最终收敛汇报

本设计的目标不是“最大化自动化”，而是“最大化有效收敛”。该 skill 必须避免陷入没有新增证据、没有风险下降、没有价值产出的无休止自转。

## 2. 设计结论摘要

本设计推荐新增一个混合式 orchestrator skill，而不是重新发明整套开发流程，也不是简单串联几个已有 skill。

推荐定位：

- 这是一个总控型执行 skill
- 只在用户明确要求“按这个方案执行”时触发
- 只消费明确的 plan/spec Markdown 文件
- 自己负责状态机、派发策略、work-value gate 和 progress/handoff
- 尽量复用现有 skill、现有 repo 约束和现有验证规则，不平行发明第二套事实来源

建议 skill 名称：

- `plan-execution-orchestrator`

该名称强调“计划执行总控”而不是泛化为所有 coding 任务都适用的万能 skill。

## 3. 触发条件与非触发条件

### 3.1 触发条件

仅当以下条件同时满足时触发：

1. 用户明确表达“按这个方案执行”或同等语义
2. 已存在明确的 plan/spec Markdown 文件
3. 该 plan 可以被拆分为至少一个可执行任务
4. 当前工作允许在本会话内推进，而不是仍处于 brainstorming 阶段

### 3.2 非触发条件

以下场景不触发：

- 用户还在讨论想法、方案或边界
- 没有 plan/spec Markdown 文件
- plan 仍明显缺失关键决策，无法安全落地
- 当前任务一开始就命中 `AGENTS.md` 中的必须确认边界，且没有更小的可执行子集
- 唯一可行方案需要立即扩大范围、改关键共享入口、改公开行为，或引入 fallback/compat/adapter

### 3.3 入口约束

该 skill 的入口必须做一个轻量 gate：

- 读取 plan
- 对照 `AGENTS.md` 和 repo 当前事实文档检查明显的 stop conditions
- 判断计划是否存在可安全开始的执行子集
- 如入口即发现高风险边界变化，先汇总最小方案给用户，而不是启动 agent 循环

## 4. 为什么不直接复用单一现有 skill

### 4.1 不采用重型独立总控 skill

如果把实现、审查、验证、进度沉淀、最终收敛都完全重写，会与现有 skill 大量重叠：

- `subagent-driven-development`
- `requesting-code-review`
- `verification-before-completion`
- `docs/human-ai/skills/nsf-repo-execution`

这会带来两套流程并存、维护成本高、仓库规则重复漂移的问题。

### 4.2 不采用纯包装 skill

如果只做“读 plan 然后顺序调用已有 skill”，则无法满足本设计最关键的两个要求：

- work-value gate
- progress/handoff 自动沉淀与收敛

这种方案容易退化成“串流程”，而不是“总控”。

### 4.3 推荐混合式 orchestrator

推荐方案是：

- 总控 skill 自己维护状态机与停止规则
- 实际开发、review、验证动作尽量借用已有 skill 的思路和约束
- repo-specific 判断仍以 `AGENTS.md` 与当前事实文档为最高约束

## 5. 执行状态机

推荐状态机如下：

1. `Entry Gate`
2. `Plan Intake`
3. `Execution Round`
4. `Parallel Review Round`
5. `Work-Value Gate`
6. `Escalate Or Continue`
7. `Progress/Handoff Sink`
8. `Completion Gate`

### 5.1 Entry Gate

职责：

- 验证用户明确要求“按这个方案执行”
- 验证 plan/spec 路径存在
- 识别是否一开始就命中必须确认项
- 识别是否可以安全拆出第一批执行任务

产物：

- 本次执行是否允许开始
- 如果不能开始，输出最小阻塞总结

### 5.2 Plan Intake

职责：

- 读取 plan/spec Markdown
- 读取 `README.md`、`AGENTS.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`
- 按需补读主题文档
- 提取任务、文件范围、验证要求、文档更新要求
- 给任务打标签：
  - 可并行实现
  - 只能串行实现
  - 高风险审查项
  - 预期验证层级

产物：

- 任务拆分表
- 写集/责任归属表
- 最小验证矩阵

### 5.3 Execution Round

职责：

- 对写集不重叠的任务并行派发 implementer
- 对强耦合任务保持串行
- 每个 implementer 都拿到明确 ownership、验证要求与禁止越界规则

产物：

- 代码改动
- 子任务级验证结果
- implementer concerns

### 5.4 Parallel Review Round

职责：

- 在实现轮结束后，并行启动：
  - `architecture-reviewer`
  - `qa-verifier`

审查轮不直接改代码，只提供证据与判定。

### 5.5 Work-Value Gate

职责：

- 判断是否值得继续下一轮
- 避免没有新增证据的无效自转

### 5.6 Escalate Or Continue

职责：

- 普通代码级问题：允许进入修复轮
- 架构边界、公开行为、兼容层/fallback：立即停止并汇总给用户
- 价值不足：停止循环并沉淀 progress/handoff

### 5.7 Progress/Handoff Sink

职责：

- 在每轮结束时记录进展
- 在长链路、暂停、上下文接近上限时留下可续接文档

### 5.8 Completion Gate

职责：

- 检查是否具备“可以宣称执行完成”的 fresh evidence
- 未通过则不得输出完成态

## 6. Subagent 角色与职责边界

推荐仅保留 3 类 subagent，controller 由主会话承担。

### 6.1 Controller

controller 不是 subagent，而是主控。

职责：

- 读取 plan 和 repo 事实文档
- 进行任务拆分与并发决策
- 派发 implementer / architecture-reviewer / qa-verifier
- 汇总结论
- 执行 work-value gate
- 决定继续、升级或停止
- 写 progress/handoff
- 负责最终汇报

约束：

- controller 是唯一允许决定“是否进入下一轮”的角色
- controller 不能因为存在问题就自动继续，必须先过 work-value gate

### 6.2 Implementer

职责：

- 按 ownership 编写代码
- 补测试或更新验证
- 运行最小必要验证
- 报告风险与阻塞

输入 contract：

- plan 文件路径
- 当前任务全文
- 允许修改的文件范围
- 不允许触碰的共享边界
- 最小验证要求
- “你不独占代码库，不要回滚他人改动”

输出 contract：

- `DONE`
- `DONE_WITH_CONCERNS`
- `NEEDS_CONTEXT`
- `BLOCKED`

并附：

- 修改文件列表
- 跑过的验证
- 遗留风险

### 6.3 Architecture Reviewer

职责：

- 只读审查，不改代码
- 检查是否触发 `AGENTS.md` 的必须确认项
- 检查是否越过单一事实来源
- 检查是否引入 fallback、compat、adapter、双写路径
- 检查是否改变模块边界、共享入口或公开行为

输出 contract：

- `PASS`
- `PASS_WITH_RISKS`
- `NEEDS_USER_CONFIRMATION`
- `FAIL`

并附：

- 证据
- 风险级别
- 最小备选方案

### 6.4 QA Verifier

职责：

- 只负责验证与结论，不发明新需求
- 选择与改动面匹配的最小验证
- 区分真实回归、环境问题、偶发失败
- 检查是否触发文档更新
- 检查是否满足 `verification-before-completion`

输出 contract：

- `PASS`
- `FAIL_CODE`
- `FAIL_ENV`
- `FAIL_FLAKE`
- `DOC_UPDATE_REQUIRED`

并附：

- 运行命令
- 结果摘要
- 建议下一步

## 7. Work-Value Gate

这是该 skill 的核心控制器，而不是附属优化项。

原则：

`自动循环只服务于高价值收敛，不服务于无证据的重复劳动。`

### 7.1 硬停止条件

命中任一条时，立即停止自动循环并升级给用户：

- 触发 `AGENTS.md` 中的必须确认项
- 需要修改模块边界、单一事实来源或关键共享入口
- 需要改变公开行为
- 唯一可行路径需要 fallback、compat、adapter、双写路径
- implementer / reviewer / verifier 的结论互相矛盾且当前证据不足以裁决
- 连续两轮没有形成新的可验证假设

### 7.2 价值评分维度

未命中硬停止条件时，对下一轮是否继续做三维评分，每项 `0-2` 分，总分 `0-6`。

#### A. 新增证据

- `0`: 没有新增证据，只是在重复先前判断
- `1`: 有新增日志/失败信息，但尚不足以明确指向
- `2`: 有明确新证据，已形成具体修复假设

#### B. 风险下降

- `0`: 风险未下降或扩大
- `1`: 风险略降，但核心不确定性仍在
- `2`: 关键风险明显下降，问题范围已被有效收缩

#### C. 收益/成本比

- `0`: 下一轮大概率继续试错，成本高于预期收益
- `1`: 可以尝试一轮，但收益有限
- `2`: 下一轮目标清晰，且预期能显著推进收敛

### 7.3 决策规则

- 总分 `5-6`：允许自动进入下一轮
- 总分 `3-4`：仅允许在纯代码级修复场景进入一轮有限修复
- 总分 `0-2`：停止循环，转为 progress/handoff

### 7.4 轮次护栏

即使评分通过，也应加上护栏：

- 普通代码修复最多连续自动 2 轮
- 同一失败模式最多追 2 次
- 第二次仍是同类失败时，默认停止继续试错并沉淀结论

## 8. 自动继续与必须暂停的分界

### 8.1 可以自动继续的情况

- implementer 已形成明确代码级修复方案
- architecture-reviewer 未触发必须确认项
- qa-verifier 给出明确的代码问题或验证缺口
- work-value gate 判断继续有价值

### 8.2 必须暂停给用户的情况

- 触发 `AGENTS.md` 的必须确认边界
- architecture-reviewer 输出 `NEEDS_USER_CONFIRMATION`
- 修改将影响公开行为或关键共享入口
- 唯一修复路径会扩大任务范围
- 继续投入的价值评分过低

### 8.3 停止时必须输出的内容

停止并不等于“什么都不做”，而应至少输出：

- 当前轮次
- 已有证据
- 为什么不值得继续自动循环
- 最小可选方案
- 用户下一步只需要做的决策

## 9. Progress/Handoff 设计

progress/handoff 不是补救措施，而是内建能力。

### 9.1 触发时机

满足任一条件时写入：

- 一轮执行结束且需要判断是否进入下一轮
- 命中硬停止条件
- work-value gate 判定停止
- 上下文接近上限
- 用户中断或准备换线程续接

### 9.2 写入位置

优先级如下：

1. 追加到当前任务已有的 plan/spec Markdown 中的 `Execution Progress` 段
2. 如不适合直接改原文，则在 `docs/human-ai/` 下新增 task-specific progress/handoff 文档

这类文档默认属于协作沉淀，不自动升级为当前事实文档。

### 9.3 建议结构

- `Context`
- `Completed`
- `Findings`
- `Work-Value Gate`
- `Remaining`
- `Risks / Needs Confirmation`
- `Resume Entry`

其中 `Work-Value Gate` 必须明确写出：

- 新增证据
- 风险变化
- 收益/成本判断
- 继续或停止的原因

## 10. 最终收敛汇报格式

该 skill 完成时，最终说明应固定覆盖以下内容：

- `Root Cause / Outcome`
- `Actual Changes`
- `Architecture Fit`
- `Verification`
- `Doc Update Status`
- `If Stopped`

其中：

- `Verification` 必须基于 fresh evidence
- `Doc Update Status` 若未改文档，也必须明确写 `No doc updates needed`
- `If Stopped` 应说明为何停止自动循环，以及当前最有价值的下一步

## 11. 与现有 skill 的关系

该设计不应替代现有 skill，而应建立在它们之上。

推荐关系：

- repo-specific guardrails：复用 `docs/human-ai/skills/nsf-repo-execution/`
- 实现阶段的多 agent 拆分思路：参考 `subagent-driven-development`
- review 思路：参考 `requesting-code-review`
- 完成态验证：参考 `verification-before-completion`

该 orchestrator skill 的新增价值在于：

- 计划驱动的入口 gate
- 面向轮次的并发调度
- work-value gate
- progress/handoff 自动沉淀
- 自动继续与必须暂停的硬边界

## 12. 推荐实现路线

建议分两阶段实现，避免第一版过重。

### Phase 1

先实现最小可用 orchestrator：

- 明确触发条件
- 读取 plan
- 派发 implementer
- 实现后并行 architecture-reviewer 与 qa-verifier
- 落地 work-value gate
- 落地 progress/handoff 模板

### Phase 2

在第一版稳定后再补强：

- 更细粒度的任务切分 heuristics
- 更严格的 output contract 模板
- 针对 repo 常见任务的 prompt 模板库
- 对 plan 文档的自动追加进度

## 13. 明确非目标

本设计明确不追求：

- 把所有 coding 任务都自动变成多 agent 编排
- 绕过用户对高风险边界的确认
- 用更多 agent 掩盖 plan 本身不清楚的问题
- 用“自动多轮”代替 evidence-based 判断
- 用无限重试代替价值判断

## 14. Open Questions

当前仍有两个实现落地层面的开放项，但不影响设计成立：

1. 该 skill 最终放在仓库共享目录 `docs/human-ai/skills/`，还是安装到 `$CODEX_HOME/skills/`
2. 第一版是否需要额外定义 reviewer/verifier prompt 模板文件，还是先把 contract 内联在 SKILL.md 中

推荐默认：

- 先在仓库内以共享设计稿和共享 skill 形式演进
- 稳定后再视需要推广到个人或团队全局 skill 目录

## 15. 一句话准则

`该 skill 的目标不是最大化 agent 活动量，而是最大化有效收敛。`
