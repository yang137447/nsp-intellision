# 架构总览

本文档描述当前仓库的实际结构和单一事实来源。它不是规划稿，而是给维护者和 AI 用的当前事实说明。

## 顶层结构

- `client/`: VS Code 扩展客户端，负责启动语言服务、同步配置、消费 LSP 能力
- `server_cpp/`: C++ 语言服务端，负责解析、索引、诊断、悬停、补全、签名帮助、语义高亮
- `server_cpp/resources/`: 服务端运行时资源 bundle
- `src/test/`: VS Code 集成测试入口
- `test_files/`: 仓库模式测试夹具
- `scripts/`: 门禁脚本、资源生成脚本、资源校验脚本

## 请求链路

1. VS Code 激活 `client/out/extension`
2. 客户端优先使用内置 server；如果配置了高级设置 `nsf.serverPath`，则用它覆盖默认路径
3. 客户端用 stdio 启动 C++ server
4. server 在 `server_request_handlers.cpp` 中分发 LSP 请求
5. 具体能力通过共享模块和 registry 提供数据与判定
6. 结果再由客户端呈现给编辑器

## 客户端职责

当前客户端事实位于 `client/src/extension.ts`：

- 启动/重启 C++ server
- 同步配置项，如 include paths、defines、diagnostics、inlay hints、semantic tokens、metrics
- 管理状态栏、trace 输出、诊断和索引状态展示
- 在测试模式下按固定方式启动语言客户端

客户端不是语义真相来源。HLSL 关键字、builtin、对象类型、对象方法和诊断规则都应以 server 侧共享模块为准。

## 服务端职责

当前服务端事实位于 `server_cpp/`：

- `server_request_handlers.cpp`
  - LSP 请求编排层
  - 负责把 completion、hover、signature help、diagnostics、semantic tokens 等请求接到共享模块
- `diagnostics.*`
  - 诊断主入口
- `semantic_tokens.*`
  - 语义高亮主入口
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
- `symbol_query.*`: 符号目标共享查询
- `member_query.*`: 成员访问共享查询
- `declaration_query.*`: 声明位置共享查询
- `hover_rendering.*`: 通用 hover 结构化渲染
- `completion_rendering.*`: 通用 completion item 拼装
- `workspace_scan_plan.*`: 工作区扫描计划

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

## 需要同步更新本文档的场景

出现以下情况时，必须更新本文档：

- client 和 server 的职责边界变化
- 新增共享查询模块并成为事实入口
- builtin/language/type/method 的单一事实来源变化
- 关键请求链路发生改变
