# AGENTS.md

本文件面向在本仓库中执行任务的 AI 协作者。目标是让代码、测试、资源和文档始终保持一致。

## 先读什么

开始任何任务前，优先阅读这些文件：

1. `README.md`
2. `docs/architecture.md`
3. `docs/resources.md`
4. `docs/testing.md`

如果任务明确涉及某个专题，再补读相关执行方案或历史设计稿：

- `docs/` 下与当前任务直接相关的专题文档
- `docs/client-editor-features.md`
  - 当任务涉及注释切换、自动配对、`wordPattern`、snippets、folding、语言扩展名归属等 client 编辑器壳层能力时优先阅读

默认情况下，`README.md` 和 `docs/{architecture,resources,testing}.md` 才是当前事实文档。

## 仓库地图

- `client/`: VS Code 扩展客户端
- `server_cpp/`: C++ LSP 服务端
- `server_cpp/resources/`: 运行时资源 bundle
- `src/test/`: VS Code 集成测试入口
- `test_files/`: 测试夹具
- `scripts/`: 资源脚本、门禁脚本、验证脚本
- `docs/`: 当前事实文档

## 当前事实规则

- 只支持 bundle 资源布局，不要重新引入 flat 文件命名。
- bundle 统一约定：
  - `base.json`
  - `override.json`
  - `schema.json`
- 资源路径变更后，必须同步更新文档和脚本引用。
- 当前关键共享入口：
  - `server_cpp/src/resource_registry.*`
  - `server_cpp/src/language_registry.*`
  - `server_cpp/src/hlsl_builtin_docs.*`
  - `server_cpp/src/type_model.*`
  - `server_cpp/src/server_request_handlers.cpp`

## 文档更新规则

只要任务改变了以下任一项，就必须在同一次任务里更新对应 Markdown 文档：

- 构建、运行、测试命令
- 目录结构
- 资源 bundle 路径、命名或加载规则
- 模块边界、单一事实来源、关键架构关系
- 影响 hover、completion、signature help、diagnostics、semantic tokens 的公开行为
- AI 协作流程或仓库约束

最低需要检查是否要更新的文档：

- `README.md`
- `AGENTS.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`

如果本次改动不需要改文档，也要在最终说明里明确写出 "No doc updates needed" 或中文等价说明，并给出原因。

## 关闭任务前的检查清单

结束任务前，按顺序检查：

1. 命令是否变化
2. 路径或命名是否变化
3. 架构或单一事实来源是否变化
4. 测试策略是否变化
5. 文档是否已同步

满足其一为 "是" 时，必须更新相应文档后再结束任务。

## 推荐验证顺序

按改动范围选择最小必要验证：

- 资源/schema 变更：`npm run json:validate`
- TypeScript/client 变更：`npm run compile`
- C++ server 变更：`cmake --build .\\server_cpp\\build`
- 默认集成回归：`npm run test:client:repo`
- 发版前全量门禁：`npm run gate:d3`
- 发版打包：`npm run package:vsix`

## Windows 注意事项

- `server_cpp\\build\\nsf_lsp.exe` 可能因为运行中的 VS Code 扩展宿主或测试进程被锁住。
- 如果 C++ 链接失败且提示 `permission denied`，先确认占用进程已退出，再重新构建。
- 对单条集成测试的偶发失败，先重跑一次再判断是否为真实回归；不要直接据此改业务逻辑。
