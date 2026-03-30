# 对象类型与对象方法接口契约

本文档用于收敛 `server_cpp/resources/types/*`、`server_cpp/resources/methods/object_methods/*`、`type_model.*` 与其下游消费层之间的共享接口契约。

本文档是该链路的当前事实文档。

约定：

- 除“当前偏差”小节外，其余条目都表示当前仓库要求遵守的共享契约。
- “当前偏差”记录的是已确认存在的实现缺口；它们本身也是当前事实的一部分，但不覆盖本文其余契约。
- 如果实现与本文不一致，且不属于“当前偏差”明确列出的例外，应优先修实现，或在同一次任务里同步改写本文。

## 适用范围

当任务涉及以下任一主题时，应先阅读本文：

- `Texture*` / `RWTexture*` / `Buffer*` / `Sampler*` 对象类型知识
- `types/object_types`、`types/object_families`、`types/type_overrides`
- `methods/object_methods`
- `type_model.*`
- 对象方法的 hover / completion / signature help / diagnostics
- array texture 坐标维度
- 参数声明标签、调用参数标签、`label: expr` / `label: Type name` 一类语法

## 单一事实来源

- 原始对象类型数据：
  - `server_cpp/resources/types/object_types/`
- 原始对象族与兼容关系：
  - `server_cpp/resources/types/object_families/`
- 原始对象方法数据：
  - `server_cpp/resources/methods/object_methods/`
- 资源加载与 schema 校验：
  - `server_cpp/src/resource_registry.*`
- 资源到共享查询的归一化入口：
  - `server_cpp/src/type_model.*`
  - `server_cpp/src/hover_markdown.*`

约束：

- feature 层不得绕过上述共享入口直接读取 JSON。
- request / diagnostics / hover / completion 层不得各自复制一份对象类型表、对象族表或方法适配规则。

## 本链路接口分层映射

这条链路里，同一模块可以同时处在多个“接口层次”里，但主责任仍应保持单一。

| 层次 | 当前载体 | 当前责任 | 调用方不该做什么 |
| --- | --- | --- | --- |
| 资源/数据接口 | `resources/types/*`、`resources/methods/object_methods/*` | 定义对象类型字段、对象族、对象方法模板 | 不要在 consumer 里重新发明一份资源字段 |
| 加载接口 | `resource_registry.*` | bundle 路径解析、JSON 读取、schema 校验 | 不要在 feature 层直接读 JSON |
| 共享模块 API 接口 | `type_model.*`、`hover_markdown.*` 暴露的 public 查询 | 对外提供跨模块可调用入口 | 不要绕过 `*.hpp` 只靠 `*.cpp` 行为猜接口 |
| consumer-ready 语义接口 | `type_model.*` 返回的 family / texture-like / sampleCoordDim / loadCoordDim | 把底层字段转换成 consumer 可直接使用的共享语义 | 不要把 `coordDim`、`isArray` 留给 request 层自己拼装 |
| 请求/渲染接口 | `hover_markdown.*`、`diagnostics.*`、`server_request_handlers.*` | 使用共享语义生成 hover、signature help、diagnostics | 不要本地复制一份对象方法参数规则 |
| 事实/验证接口 | 本文、相关测试夹具与断言 | 记录当前契约，并把共享行为锁进回归 | 不要让契约只存在口头经验里 |

因此：

- `type_model.*` 可以同时有原始字段查询和 consumer-ready 语义查询。
- 这不表示它有多个主责任；它的主责任仍然是“对象类型语义查询”。
- 真正需要避免的是：request 层直接解释资源字段，或一个模块同时承担资源加载、语义归一化、请求编排三种主责任。

## 共享术语

- `baseType`
  - 对象的规范化基础类型名，例如 `Texture2DArray`、`RWTexture2D`、`SamplerState`
- `family`
  - 对象族，例如 `Texture`、`RWTexture`、`Buffer`
- `coordDim`
  - 对象的空间坐标维度，不等于所有方法最终使用的参数向量维度
  - 例如 `Texture2DArray` 的 `coordDim` 仍然是 `2`
- `isArray`
  - 对象是否带 array slice 维度
- `sampleCoordDim`
  - 采样类方法应使用的坐标维度
  - 最低要求：`coordDim + (isArray ? 1 : 0)`
- `loadCoordDim`
  - `Load` 类方法应使用的整数坐标维度
  - 最低要求：`sampleCoordDim + 1`
- `parameter label`
  - 对外展示或调用时匹配的标签，例如 `uv`
- `binding name`
  - 在函数体内可被局部解析、hover、diagnostics 读取的形参变量名，例如 `uvz`

## 资源层契约

### `types/object_types`

每个对象类型条目至少定义：

- `name`
- `family`
- `coordDim`
- `isRw`
- `isArray`

约束：

- `coordDim` 只表达空间维度，不表达 array slice、mip、sample index 等附加维度。
- `isArray` 不是展示字段；它必须能被下游共享查询消费，不能只停留在资源层或加载层。

### `types/object_families`

约束：

- family 负责表达对象族归属与兼容关系。
- family 不是方法参数维度、坐标形状或语法兼容性的真相来源。

### `methods/object_methods`

约束：

- `targetFamilies` 只表达 family 级别的目标范围。
- 方法签名模板中的占位符必须由共享层按规范化对象语义展开，不能在 request 层各自猜。
- 如果方法可用性依赖 family 之外的附加条件，条件必须进入共享契约；不能在调用方各自写黑名单或额外分支。

## 共享查询层契约

### `type_model.*`

职责：

- 把资源层的对象类型、对象族、覆盖层合并成统一查询接口。
- 对外提供 consumer 直接可用的共享语义，而不是只暴露原始字段。

约束：

- consumer 如果需要“方法实际使用的坐标维度”，必须通过 `type_model.*` 的共享查询获得，不能自己把 `coordDim` 当最终答案。
- consumer 不得在本地用 `family == Texture`、`coordDim == 2` 之类规则反推 `Texture2DArray`、`TextureCubeArray` 的最终参数形状。
- 如果公开查询语义发生变化，必须在同一次任务里同步更新 `type_model.hpp` 的职责与 public API 注释。

最低共享语义要求：

- 对象族查询
- sampler-like / texture-like 查询
- spatial `coordDim` 查询
- sample-like 坐标维度查询
- load-like 坐标维度查询

### `hover_markdown.*`

职责：

- 加载对象方法资源
- 根据共享对象语义展开占位符、生成 hover / signature help 文案

约束：

- 占位符展开必须依赖共享查询层给出的规范化对象语义。
- 不允许在 hover / signature help 内部对未知 texture 默认为 `2D`，除非该默认是共享契约明确允许的行为。

## 消费层契约

以下层都属于 consumer：

- hover
- signature help
- member completion
- inlay hints
- diagnostics

约束：

- 所有对象方法相关 consumer 必须共享同一套对象方法可用性与参数形状判定。
- 某层一旦需要新增对象方法语义，不得只在该层单独补一份规则；应先下沉到共享层。
- 如果 consumer 只需要“参数形状”而不是“资源字段”，它必须依赖共享查询接口而不是直接解释 `coordDim` / `isArray`。

## 参数标签与调用标签契约

### 声明侧

如果语法支持 `label: Type name`，共享语义层必须能区分至少三件事：

- parameter label
- semantic type
- binding name

约束：

- hover / diagnostics / signature help / inlay hints 不得把这三者混成同一个 token 含义。
- 解析层不能因为看到 `:` 就过早终止，导致只保留 label 而丢失真正的 type / binding name。

### 调用侧

如果语法支持 `label: expr`，consumer 必须遵守：

- 用 `expr` 做类型推断与实参兼容校验
- 用 `label` 做展示、参数位次对齐或编辑器提示

约束：

- 调用参数拆分后，必须先剥离共享层认可的 label 前缀，再进入表达式类型推断。
- 不允许某一层把 `label: expr` 当普通二元表达式，而另一层把它当命名参数。

## 当前偏差

已确认的当前偏差如下：

1. 参数声明与调用参数对 `:` 的解释尚未统一收敛到共享契约。
   - 现有实现中，部分解析逻辑会在 `:` 处提前停止，导致 declaration-side label / binding name / semantic type 的边界不稳定。
   - 调用侧也尚未统一规定 `label: expr` 应在何处被剥离、何处被保留。

这些偏差在修复完成前，不应再继续扩散到更多 request 层本地分支里。

## 当前支持边界

对于 `label: Type name` / `label: expr` 一类标签语法，当前事实如下：

- 仓库当前还没有形成一套已落地且各 consumer 统一消费的共享建模。
- 在共享解析与共享语义层补齐之前，这类写法不属于“可新增依赖的共享支持边界”。
- 现有局部路径如果出现“部分接受、部分忽略、部分误判”，应视为实现偏差，不视为受支持契约。

因此当前约束是：

- 新增功能、规则或测试断言不得继续依赖这类写法的局部成功行为。
- 如果后续要正式支持，必须先在共享层完整建模 declaration-side label / type / binding name，以及 call-site label / expr 的拆分语义，再同步更新本文。
- 如果后续决定不支持，也必须在语法/语义链路上给出一致拒绝；不允许长期保留灰区状态。

## 修改此链路时必须同步做什么

当任务修改以下任一项时，必须同步检查并更新本文：

- `types/*` 资源字段语义
- `methods/object_methods` 的占位符含义
- `type_model.*` 的 public 查询语义
- 对象方法的 hover / completion / signature help / diagnostics 判定规则
- 参数标签 / 调用标签的共享解释方式

推荐同步验证：

- 资源结构变更：`npm run json:validate`
- C++ 实现变更：`cmake --build .\\server_cpp\\build`
- 对外行为变更：`npm run test:client:repo`
