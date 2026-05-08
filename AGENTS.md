# AGENTS.md

本文件面向在本仓库中执行任务的 AI 协作者。它只定义协作流程、维护约束和关闭任务要求；代码架构、资源、测试和专题事实分别以对应 `docs/*.md` 为准。

## 开始前阅读

任何任务先读：

1. `README.md`
2. `docs/architecture.md`
3. `docs/resources.md`
4. `docs/testing.md`

按任务补读：

- 涉及 client 编辑器壳层能力：`docs/client-editor-features.md`
- 涉及对象类型、对象方法、array texture 坐标或 `label: Type name` / `label: expr`：`docs/type-method-interface-contract.md`
- 涉及本地开发、调试或打包：`docs/development.md`
- 涉及已有方案或背景：`docs/human-ai/` 下直接相关文档

默认事实来源：

- 当前事实文档：`README.md`、`docs/architecture.md`、`docs/resources.md`、`docs/testing.md`
- 专题事实文档：`docs/client-editor-features.md`、`docs/type-method-interface-contract.md`、`docs/development.md`
- 协作沉淀：`docs/human-ai/` 默认不属于当前事实，除非文档明确声明已升格

## 仓库地图

- `client/`: VS Code 扩展客户端
- `server_cpp/`: C++ LSP 服务端
- `server_cpp/resources/`: 运行时资源 bundle
- `src/test/`: VS Code 集成测试入口
- `test_files/`: 测试夹具
- `scripts/`: 资源、构建、门禁和打包脚本
- `docs/`: 当前事实文档、专题文档和协作沉淀

## 当前硬约束

- 资源只支持 bundle 布局：`base.json`、`override.json`、`schema.json`。
- 资源路径、命名或加载规则变化后，必须同步更新脚本和文档引用。
- 语言知识应通过共享入口维护，不得在 feature 代码里复制：
  - `server_cpp/src/resource_registry.*`
  - `server_cpp/src/language_registry.*`
  - `server_cpp/src/hlsl_builtin_docs.*`
  - `server_cpp/src/type_model.*`
  - `server_cpp/src/requests/server_request_handlers.cpp`

## 默认执行模式

- 默认优先长期架构正确、职责清晰、运行时边界稳定和后续维护成本低。
- 先定位根因，再决定改动层级；如果根因是模块边界、缓存、调度或上下文契约问题，应在共享层或架构层修正。
- 多个症状共享同一设计缺陷时，应合并治理。
- 优先复用现有共享模块、registry 和单一事实来源。
- 不以“最小改动”“先让它过”为理由保留已确认错误的边界、状态所有权或隐式顺序。
- 不把当前任务扩展成与根因无关的命名清理、风格统一或目录整理。

## 连续执行与上下文收敛

- 用户给出明确执行方案且未触发必须确认项时，默认连续推进到完成。
- 不因完成一个子阶段就停下等用户说“继续”。
- 接近上下文窗口 80% 时，先沉淀可续接进度，再继续长链路工作。
- 进度沉淀优先写入当前任务直接相关的既有 Markdown；没有合适文档时，在 `docs/human-ai/` 新增 progress / handoff 文档。
- handoff 至少写清：已完成事项、当前结论、剩余步骤、风险点、待确认事项、续接所需最小上下文。

## AI 可维护性

- 新增或拆分模块 / 类时保持单一职责。
- 触达历史大文件时，不继续把新职责塞回旧入口；优先沿职责边界拆分、下沉或重组共享模块。
- 新增或修改 `*.hpp` 时，把头文件视为局部接口契约，补齐模块 / 类职责、关键 public API、输入输出、调用前提和非目标范围。
- public 接口、所有权边界、缓存语义或调用前提变化时，同步更新头文件说明。
- 补丁式修复与架构治理冲突时，默认优先架构治理。

## 禁止项

- 未经用户明确要求，不新增 fallback、compat layer、adapter、shim、feature flag、双写路径或新旧逻辑并存。
- 不新增冗余默认分支、吞错分支、静默降级、重试或猜测性修复。
- 不绕过共享入口直接读取资源、复制语言知识或局部写死规则。
- 不因单个测试失败就修改业务逻辑迁就偶发时序；先复现、重跑并定位根因。
- 不把同一根因拆成多处局部补丁或临时特判。

## 必须停下来确认

出现以下情况，先说明触发点、风险和推荐方案，再等待确认：

- 需要改变 hover、completion、signature help、diagnostics、semantic tokens 等公开行为。
- 需要新增资源 bundle、修改资源路径 / 命名 / 加载规则，或重新引入已否定的旧布局。
- 需要新增跨文件兼容层、迁移层或保留旧行为兜底。
- 存在两个及以上长期架构方向都合理，且取舍影响明显。
- 需要高成本、大范围、长链路架构重写，用户尚未接受成本、风险或节奏。

## 实施与验证

- 复杂、多文件、跨模块或高风险改动前，用 3-5 句话说明根因判断、系统性问题和目标收敛边界。
- 低风险小改动可简短同步，最终说明根因与方案。
- 验证要证明症状消失，也要证明新的架构契约成立、旧补丁路径已移除。
- 验证失败时，先区分真实回归、环境问题和偶发失败。
- 同一问题反复暴露共享状态、上下文传播或模块边界错误时，停止叠补丁，回到架构层收敛。

推荐验证顺序：

- 资源 / schema：`npm run json:validate`
- TypeScript / client：`npm run compile`
- C++ server：`cmake --build .\server_cpp\build`
- 默认集成回归：`npm run test:client:repo`
- 发版前门禁：`npm run gate:d3`
- 发版打包：`npm run package:vsix`

## 测试坑点制度化

- 集成测试里已复现并确认非偶发的坑点，不保留为口头经验。
- 如果坑点来自测试前提、时序或调用方式，收敛为 shared helper、统一写法或写入 `docs/testing.md`。
- 如果坑点来自 AI 协作流程、验证习惯或仓库约束，同步写入 `AGENTS.md`。
- 同类坑点影响 2 条及以上测试时，默认需要制度化。
- 已被 shared helper 吸收的前提，应删除或降级旧文档规则。

## 文档更新规则

只要任务改变以下任一项，必须同步更新对应 Markdown：

- 构建、运行、测试命令
- 目录结构
- 资源 bundle 路径、命名或加载规则
- 模块边界、单一事实来源、关键架构关系
- hover、completion、signature help、diagnostics、semantic tokens 等公开行为
- client 编辑器壳层能力
- AI 协作流程或仓库约束
- 头文件接口契约、模块职责说明或 AI 维护约束

最低检查：

- `README.md`
- `AGENTS.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
- `docs/client-editor-features.md`（涉及 editor 壳层时）
- `docs/type-method-interface-contract.md`（涉及对象类型 / 对象方法时）
- `docs/development.md`（涉及开发、调试或打包时）

如果不需要更新文档，最终说明必须写出 “No doc updates needed” 或中文等价说明，并说明原因。

## 关闭任务前检查

按顺序确认：

1. 命令是否变化
2. 路径或命名是否变化
3. 架构或单一事实来源是否变化
4. 测试策略是否变化
5. 文档是否已同步

任一项为“是”时，先更新对应文档再结束任务。

## 最终汇报

最终说明必须包含：

- 根因
- 实际改动
- 为何符合当前架构
- 实际运行的验证
- 文档是否需要更新

如果做了结构性重构、扩大改动范围或删除旧路径，直接说明这是为了消除系统性设计问题、保持架构整洁和避免补丁化演进。

## Windows 注意事项

- `server_cpp\build\nsf_lsp.exe` 可能被运行中的 VS Code 扩展宿主或测试进程锁住。
- C++ 链接失败并提示 `permission denied` 时，先确认占用进程已退出，再重新构建。
- 单条集成测试偶发失败时，先重跑一次再判断是否真实回归。
- 本机执行 Python 3 脚本时，优先使用 `py -3`，不要假设 `python` 指向可用的 Python 3。
