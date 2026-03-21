# Client 编辑器能力

本文档是当前事实文档，描述 VS Code 客户端独有的编辑器壳层能力，以及这些能力的单一事实来源。

适用场景：

- 修改 `package.json` 里的语言注册、snippets 或设置描述
- 修改 `syntaxes/nsf.language-configuration.json`
- 修改 `snippets/nsf.code-snippets`
- 排查“不是 LSP 语义问题，而是 VS Code 编辑器行为”的问题

不适用场景：

- hover / completion / signature help / diagnostics 的语义正确性
- server 侧资源 bundle、schema 和共享查询模块

## 1. 真相来源

与 client 编辑器壳层能力直接相关的当前事实文件只有这几处：

- `package.json`
  - `contributes.languages`
  - `contributes.grammars`
  - `contributes.snippets`
  - `contributes.configuration`
- `syntaxes/nsf.language-configuration.json`
  - 注释、配对、`wordPattern`、回车规则、folding markers
- `snippets/nsf.code-snippets`
  - 代码片段内容
- `client/src/extension.ts`
  - client 运行时对 `nsf.shaderFileExtensions` 的默认值兜底
- `server_cpp/src/main.cpp`
  - server 启动时的默认 shader 扩展名兜底

结论：

- 如果问题表现为注释切换、自动配对、选词、回车续写、折叠、snippet 不生效，先看这份文档和上述文件。
- 不要把这类问题先归因到 LSP server。

## 2. 当前能力总览

当前 `nsf` 语言在 client 侧已提供这些编辑器能力：

- 语言模式注册：`.nsf/.hlsl/.hlsli/.fx/.usf/.ush`
- grammar 挂接
- 行注释 / 块注释
- 括号与引号自动配对
- 选中后包裹
- 基础缩进
- 保守版 `wordPattern`
- `///` 与 `/** */` 注释续写
- `#region / #endregion` 折叠
- 最小 snippets 集

## 3. 语言注册

真相位置：

- `package.json` 的 `contributes.languages`

当前行为：

- 下列扩展名都归到 `nsf` 语言：
  - `.nsf`
  - `.hlsl`
  - `.hlsli`
  - `.fx`
  - `.usf`
  - `.ush`

最小示例：

```json
{
  "id": "nsf",
  "extensions": [".nsf", ".hlsl", ".hlsli", ".fx", ".usf", ".ush"],
  "configuration": "./syntaxes/nsf.language-configuration.json"
}
```

维护规则：

- 新增或删除语言扩展名时，必须同时检查：
  - `package.json`
  - `client/src/extension.ts`
  - `server_cpp/src/main.cpp`
  - `README.md`

## 4. 语言配置

真相位置：

- `syntaxes/nsf.language-configuration.json`

### 4.1 注释切换

当前支持：

- 行注释：`//`
- 块注释：`/* */`

最小示例：

```json
{
  "comments": {
    "lineComment": "//",
    "blockComment": ["/*", "*/"]
  }
}
```

典型表现：

- `Ctrl+/`
- `Shift+Alt+A`

### 4.2 自动配对与包裹

当前支持：

- `{}` / `[]` / `()`
- `""`

最小示例：

```json
{
  "autoClosingPairs": [
    {"open": "{", "close": "}"},
    {"open": "[", "close": "]"},
    {"open": "(", "close": ")"},
    {"open": "\"", "close": "\"", "notIn": ["string", "comment"]}
  ],
  "surroundingPairs": [
    {"open": "{", "close": "}"},
    {"open": "[", "close": "]"},
    {"open": "(", "close": ")"},
    {"open": "\"", "close": "\""}
  ]
}
```

当前边界：

- 不对 `<` / `>` 做自动配对

原因：

- `Texture2D<float4>` 与比较表达式混用较多，激进配对容易误伤

### 4.3 `wordPattern`

当前策略：

- 保守版规则
- 优先保证 `SV_Position`、宏名、常见标识符和大写开头的泛型对象类型不退化

最小示例：

```json
{
  "wordPattern": "(-?\\b\\d*\\.?\\d+(?:[eE][+-]?\\d+)?[fFuUlL]*\\b)|([A-Z][A-Za-z0-9_]*<[^\\s<>]+>)|([A-Za-z_][A-Za-z0-9_]*)"
}
```

实际例子：

- `SV_Position`
- `MY_DEFINE`
- `Texture2D<float4>`
- `myValue`

当前边界：

- swizzle 和更复杂模板样式只保证“不明显退化”，不承诺覆盖所有特殊写法

### 4.4 缩进与回车规则

当前支持：

- `{` 后回车缩进
- 同行存在 `}` 时使用 `indentOutdent`
- `///` 文档注释续写
- `/** */` 多行注释续写

最小示例：

```json
{
  "indentationRules": {
    "increaseIndentPattern": "^((?!\\/\\/).)*(\\{[^}\"'`]*)$",
    "decreaseIndentPattern": "^\\s*\\}"
  },
  "onEnterRules": [
    {
      "beforeText": "^\\s*///.*$",
      "action": {
        "indent": "none",
        "appendText": "/// "
      }
    }
  ]
}
```

实际例子：

```hlsl
/// explain this function
float4 Shade(...)
```

按回车后应续出：

```hlsl
/// explain this function
/// 
```

### 4.5 Folding

当前支持：

- `#region / #endregion`

最小示例：

```json
{
  "folding": {
    "markers": {
      "start": "^\\s*#\\s*region\\b",
      "end": "^\\s*#\\s*endregion\\b"
    }
  }
}
```

当前边界：

- 还没有“固定注释块折叠”的正式规则

## 5. Snippets

真相位置：

- `package.json` 的 `contributes.snippets`
- `snippets/nsf.code-snippets`

当前最小片段集：

- `VS_INPUT`
- `PS_INPUT`
- `cbuffer`
- `vs_main`
- `ps_main`
- `technique/pass`
- `#if / #else / #endif`

最小示例：

```json
{
  "NSF: PS_INPUT struct": {
    "prefix": ["nsf-ps-input", "psinput"],
    "body": [
      "struct PS_INPUT",
      "{",
      "\t${1:float4} ${2:position} : SV_Position;",
      "};"
    ]
  }
}
```

维护规则：

- snippet 文件新增后，必须在 `package.json` 注册
- prefix 应保持统一风格，避免 completion 面板混乱
- 首版优先可直接改写的骨架，不要引入过多占位符

## 6. `.hlsli` 一致性

`.hlsli` 是当前最容易因为多点维护而失配的扩展名。

需要保持一致的文件：

- `package.json`
- `client/src/extension.ts`
- `server_cpp/src/main.cpp`
- `README.md`

当前事实：

- editor 语言注册已包含 `.hlsli`
- `nsf.shaderFileExtensions` 默认值已包含 `.hlsli`
- client 运行时兜底默认值已包含 `.hlsli`
- server 启动默认值已包含 `.hlsli`

## 7. AI 协作建议

如果 AI 任务涉及 client 编辑器壳层能力，推荐阅读顺序：

1. 本文档
2. `README.md`
3. `docs/architecture.md`
4. `docs/testing.md`
5. 具体实现文件：
   - `package.json`
   - `syntaxes/nsf.language-configuration.json`
   - `snippets/nsf.code-snippets`
   - `client/src/extension.ts`

判断规则：

- 如果问题是“功能完全不触发”，优先检查 manifest 挂接和文件路径。
- 如果问题是“触发了但行为不理想”，优先检查 `language-configuration.json`。
- 如果问题是“扩展名表现不一致”，优先检查 `.hlsli` 这类多处默认值同步点。

## 8. 更新规则

以下变化发生时，必须同步更新本文档：

- `package.json` 中的语言注册、snippets 注册或 editor 相关设置描述变化
- `syntaxes/nsf.language-configuration.json` 规则变化
- `snippets/nsf.code-snippets` 的片段集合或命名策略变化
- `client/src/extension.ts` 或 `server_cpp/src/main.cpp` 中默认 shader 扩展名变化
- editor 壳层能力的公开行为变化

## 9. 验收入口

这类改动的验证说明以 `docs/testing.md` 为准。

最小手工 smoke 应至少覆盖：

- `.nsf`
- `.hlsl`
- `.hlsli`

重点检查：

- 语言模式归属
- 注释切换
- 自动配对与包裹
- 基础缩进
- `wordPattern`
- 注释续写
- `#region / #endregion`
- snippets
