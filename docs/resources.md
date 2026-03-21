# 资源规范

本文档描述 `server_cpp/resources/` 的当前事实布局、加载规则和维护流程。

## 总原则

所有运行时资源都使用 bundle 布局，不允许重新引入 flat 文件命名。

每个 bundle 目录统一包含：

```text
<bundle>/
  base.json
  override.json
  schema.json
```

## 当前 bundle 列表

```text
server_cpp/resources/
  builtins/
    intrinsics/
  language/
    keywords/
    directives/
    semantics/
  methods/
    object_methods/
  types/
    object_types/
    object_families/
    type_overrides/
```

## 各 bundle 的用途

- `builtins/intrinsics`
  - HLSL builtin 函数名、签名、文档
- `language/keywords`
  - 关键字名和说明
- `language/directives`
  - 预处理指令名、说明和语法
- `language/semantics`
  - 系统语义名、说明和典型类型
- `methods/object_methods`
  - 对象方法签名、适用对象族、文档
- `types/object_types`
  - 对象类型定义
- `types/object_families`
  - 对象族与兼容关系
- `types/type_overrides`
  - 类型和对象族的覆盖层

## 运行时加载规则

当前加载入口是 `server_cpp/src/resource_registry.*`：

- 通过 bundle key 解析路径
- 读取 `schema.json`
- 读取 `base.json`
- 读取 `override.json`
- 对 `base.json` 和 `override.json` 分别执行 schema 校验

当前 bundle key 示例：

- `builtins/intrinsics`
- `language/keywords`
- `language/directives`
- `language/semantics`
- `methods/object_methods`
- `types/object_types`
- `types/object_families`
- `types/type_overrides`

## 合并语义

当前项目遵循双层模型：

- `base`
- `override`

优先级：

- `override > base`

约定：

- 同名条目可以在 `override.json` 中覆盖 `base.json`
- `disabled: true` 表示显式禁用条目
- 即使当前没有覆盖项，也保留有效的空 `override.json`

## schema 规则

每个 bundle 的 `schema.json` 约束对应 bundle 的 JSON 结构。

当前项目已经在两处使用 schema：

- `scripts/json/validate_resources.js`
- `server_cpp/src/resource_registry.cpp`

这意味着：

- 提交前可以用脚本提前发现结构问题
- 运行时也不会静默接受错误资源

## 修改资源时必须做什么

当你修改 `server_cpp/resources/` 下任意 bundle 时，至少完成以下步骤：

1. 修改对应的 `base.json` 或 `override.json`
2. 如结构变化，同步修改 `schema.json`
3. 运行 `npm run json:validate`
4. 如果改动影响 server 行为，运行 `cmake --build .\\server_cpp\\build`
5. 如影响 completion/hover/diagnostics/semantic tokens，运行 `npm run test:client:repo`
6. 如命名、路径、规则有变化，同步更新 `README.md` 和本文件

## 生成脚本

当前资源相关脚本：

- `scripts/builtins/update_hlsl_intrinsics_manifest.js`
  - 生成 `server_cpp/resources/builtins/intrinsics/base.json`
- `scripts/json/validate_resources.js`
  - 校验所有资源 bundle

要求：

- 生成脚本输出必须直接对准当前 bundle 路径
- 不要再生成任何旧 flat 路径文件

## 构建拷贝规则

`server_cpp/CMakeLists.txt` 在构建 `nsf_lsp` 后会把整个 `server_cpp/resources/` 拷贝到可执行文件旁边的 `resources/` 目录。

因此：

- 修改资源后，只改源码目录不够
- 如果需要让本地构建产物拿到最新资源，必须重新执行一次 server 构建

## 不要做什么

- 不要新增 `manifest.json`、`*_overrides.json`、`resources/schemas/*.schema.json` 这类旧布局
- 不要让 feature 代码绕过 registry 直接读取任意 JSON 文件
- 不要把临时或实验性资源放进正式 bundle 路径而不配 `schema.json`
