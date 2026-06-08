# 对象类型与对象方法接口契约

本文档收敛 `server_cpp/resources/types/*`、`server_cpp/resources/methods/object_methods/*`、`type_model.*` 与下游 consumer 之间的共享契约。它是当前事实文档。

除“当前偏差”外，本文条目都是当前仓库要求遵守的契约；实现若不一致，应优先修实现，或在同一次任务里同步改写本文。

## 适用范围

任务涉及以下任一主题时，先读本文：

- `Texture*`、`RWTexture*`、`Buffer*`、`Sampler*` 对象类型知识
- `types/object_types`、`types/object_families`、`types/type_overrides`
- `methods/object_methods`
- `type_model.*`
- 对象方法的 hover、completion、signature help、diagnostics
- array texture 坐标维度
- 参数声明标签、调用参数标签、`label: Type name` / `label: expr` 语法

## 单一事实来源

- 原始对象类型数据：`server_cpp/resources/types/object_types/`
- 原始对象族与兼容关系：`server_cpp/resources/types/object_families/`
- 原始对象方法数据：`server_cpp/resources/methods/object_methods/`
- 资源加载与 schema 校验：`server_cpp/src/resource_registry.*`
- 对象类型语义查询：`server_cpp/src/type_model.*`
- 对象方法资源加载与展示展开：`server_cpp/src/hover_markdown.*`

约束：

- feature 层不得绕过共享入口直接读取 JSON。
- request、diagnostics、hover、completion 层不得各自复制对象类型表、对象族表或方法适配规则。

## 接口分层

| 层次 | 当前载体 | 当前责任 |
| --- | --- | --- |
| 资源/数据接口 | `resources/types/*`、`resources/methods/object_methods/*` | 定义对象类型字段、对象族和对象方法模板 |
| 加载接口 | `resource_registry.*` | bundle 路径解析、JSON 读取和 schema 校验 |
| 共享模块 API | `type_model.*`、`hover_markdown.*` public 查询 | 暴露跨模块可调用入口 |
| consumer-ready 语义接口 | `type_model.*` 返回的 family、texture-like、sampleCoordDim、loadCoordDim | 把底层字段转换成 consumer 可直接使用的共享语义 |
| 请求/渲染接口 | `hover_markdown.*`、`diagnostics.*`、request handler | 使用共享语义生成 hover、signature help 和 diagnostics |
| 事实/验证接口 | 本文、夹具和断言 | 固化契约并提供回归验证 |

`type_model.*` 可以同时提供原始字段查询和 consumer-ready 语义查询；它的主责任仍然是对象类型语义查询。真正需要避免的是 request 层直接解释资源字段，或一个模块同时承担资源加载、语义归一化和请求编排三种主责任。

## 共享术语

- `baseType`: 规范化基础类型名，例如 `Texture2DArray`、`RWTexture2D`、`SamplerState`
- `family`: 对象族，例如 `Texture`、`RWTexture`、`Buffer`
- `coordDim`: 空间坐标维度，不包含 array slice、mip 或 sample index
- `isArray`: 对象是否带 array slice 维度
- `sampleCoordDim`: 采样类方法坐标维度，最低要求为 `coordDim + (isArray ? 1 : 0)`
- `loadCoordDim`: `Load` 类方法整数坐标维度，最低要求为 `sampleCoordDim + 1`
- `parameter label`: 对外展示或调用时匹配的标签，例如 `uv`
- `binding name`: 函数体内可被局部解析、hover、diagnostics 读取的形参变量名，例如 `uvz`

## 资源层契约

`types/object_types` 每个对象类型条目至少定义：

- `name`
- `family`
- `coordDim`
- `isRw`
- `isArray`

约束：

- `coordDim` 只表达空间维度。
- `isArray` 必须能被下游共享查询消费，不能只停留在资源层或加载层。
- `types/object_families` 负责对象族归属与兼容关系，不负责方法参数维度或坐标形状。
- `methods/object_methods` 的 `targetFamilies` 只表达 family 级目标范围。
- 方法签名模板中的占位符必须由共享层按规范化对象语义展开。
- 方法可用性如果依赖 family 之外的条件，该条件必须进入共享契约，不能散落在调用方。

## 共享查询层契约

`type_model.*` 的职责：

- 合并对象类型、对象族和覆盖层资源。
- 对外提供 consumer 直接可用的共享语义。

最低共享语义：

- 对象族查询
- sampler-like / texture-like 查询
- spatial `coordDim` 查询
- sample-like 坐标维度查询
- load-like 坐标维度查询

约束：

- consumer 需要方法实际坐标维度时，必须通过 `type_model.*` 查询。
- consumer 不得用 `family == Texture`、`coordDim == 2` 等本地规则反推 array texture 参数形状。
- 公开查询语义变化时，必须同步更新 `type_model.hpp` 的职责与 public API 注释。

`hover_markdown.*` 的职责：

- 加载对象方法资源。
- 根据共享对象语义展开占位符，生成 hover / signature help 文案。
- 对 diagnostics 暴露同一份已展开参数声明，使对象方法参数兼容检查消费资源模板和 `type_model.*` 结果，而不是重新解释 `coordDim` / `isArray`。

约束：

- 占位符展开必须依赖共享查询层给出的规范化对象语义。
- 不允许在 hover / signature help 内部把未知 texture 静默默认为 `2D`，除非该默认是共享契约明确允许的行为。
- diagnostics 消费对象方法参数形状时，只能使用 `hover_markdown.*` 展开的参数声明和 `type_relation.*` 兼容结果；不得本地维护 sampler-like、floatCoord、intCoord 或 array slice 规则。

## Consumer 契约

consumer 包括 hover、signature help、member completion、inlay hints 和 diagnostics。

约束：

- 对象方法可用性与参数形状判定必须共享同一套规则。
- 新增对象方法语义时，先下沉到共享层，再由各 consumer 消费。
- consumer 只需要参数形状时，不得直接解释 `coordDim` / `isArray`。
- member access base 如果来自 `T values[N]` 的 indexed 表达式（包括 parenthesized / macro-wrapped 形态），hover、member completion、signature help 和 diagnostics 应按元素类型 `T` 查询对象方法；未 indexed 的 `T[]` declarator array 仍应保持数组类型，不能为了补全方法而静默降级。
- 合法但有风险的对象方法参数隐式转换应作为 `type_relation.*` warning 发布；找不到合法转换序列时才发布对象方法 type mismatch。

## 参数标签契约

声明侧 `label: Type name` 必须能区分：

- parameter label
- semantic type
- binding name

调用侧 `label: expr` 必须遵守：

- 用 `expr` 做类型推断与实参兼容校验。
- 用 `label` 做展示、参数位次对齐或编辑器提示。
- 调用参数拆分后，先剥离共享层认可的 label 前缀，再进入表达式类型推断。

禁止：

- hover、diagnostics、signature help、inlay hints 把 label、type、binding name 混成同一个 token 含义。
- 某层把 `label: expr` 当普通二元表达式，另一层把它当命名参数。

## 当前偏差

已确认偏差：

1. 参数声明与调用参数对 `:` 的解释尚未统一收敛到共享契约。
2. 部分解析逻辑会在 `:` 处提前停止，导致 declaration-side label / binding name / semantic type 边界不稳定。
3. 调用侧尚未统一规定 `label: expr` 应在何处剥离、何处保留。

修复完成前，不应继续把这些偏差扩散到更多 request 层本地分支。

## 当前支持边界

对于 `label: Type name` / `label: expr`：

- 当前还没有形成一套各 consumer 统一消费的共享建模。
- 在共享解析与共享语义层补齐之前，这类写法不属于可新增依赖的共享支持边界。
- 现有局部路径如果出现部分接受、部分忽略或部分误判，应视为实现偏差，不视为受支持契约。

后续若正式支持，必须先在共享层完整建模 declaration-side label / type / binding name，以及 call-site label / expr 的拆分语义，再同步更新本文。若决定不支持，也必须在语法 / 语义链路上给出一致拒绝。

## 修改流程

修改以下任一项时，必须同步检查并更新本文：

- `types/*` 资源字段语义
- `methods/object_methods` 占位符含义
- `type_model.*` public 查询语义
- 对象方法 hover、completion、signature help、diagnostics 判定规则
- 参数标签 / 调用标签共享解释方式

推荐验证：

- 资源结构变更：`npm run json:validate`
- C++ 实现变更：`cmake --build .\server_cpp\build`
- 对外行为变更：`npm run test:client:repo`
