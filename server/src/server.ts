/* --------------------------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See License.txt in the project root for license information.
 * ------------------------------------------------------------------------------------------ */
import {
  createConnection,
  TextDocuments,
  Diagnostic,
  DiagnosticSeverity,
  ProposedFeatures,
  InitializeParams,
  CompletionItem,
  CompletionItemKind,
  TextDocumentPositionParams,
  TextDocumentSyncKind,
  InitializeResult,
  HoverParams,
  Hover,
  SignatureHelpParams,
  SignatureHelp,
  DocumentFormattingParams,
  TextEdit,
  DocumentHighlightParams,
  DocumentHighlight,
  DocumentHighlightKind,
  SemanticTokens,
} from "vscode-languageserver/node";

import { TextDocument } from "vscode-languageserver-textdocument";
import { SemanticTokensParams } from "vscode-languageserver/node";
//导入 hlsl 内置的关键字
import { keywords, Keywords } from "./keywords";

// 关键点1： 初始化 LSP 连接对象
const connection = createConnection(ProposedFeatures.all);

// 关键点2： 创建文档集合对象，用于映射到实际文档
const documents: TextDocuments<TextDocument> = new TextDocuments(TextDocument);

// 定义语义令牌图例， 目前只支持关键字高亮
const tokenTypes = ['keyword'];
const tokenModifiers: string[] = [];
const semanticTokensLegend = {
  tokenTypes,
  tokenModifiers,
};
// 构建一个Map, 用于根据关键字快速查找描述
const keywordsDescriptionMap = new Map<string, string>();
keywords.forEach((item: Keywords) => {
  keywordsDescriptionMap.set(item.keyword, item.description);
});

connection.onInitialize((params: InitializeParams) => {
  // 明确声明插件支持的语言特性
  const result: InitializeResult = {
    capabilities: {
      // 增量处理
      textDocumentSync: TextDocumentSyncKind.Incremental,
      // 代码补全
      completionProvider: {
        resolveProvider: true,
      },
      // hover 提示
      hoverProvider: true,
      // 签名提示
      signatureHelpProvider: {
        triggerCharacters: ["("],
      },
      // 格式化
      documentFormattingProvider: true,
      // 语言高亮
      documentHighlightProvider: true,
      // 语义令牌能力， 用于动态语法高亮
      semanticTokensProvider: {
        legend: semanticTokensLegend,
        full: true
      }
    },
  };
  return result;
});

// Make the text document manager listen on the connection
// for open, change and close text document events
documents.listen(connection);

// Listen on the connection
connection.listen();

// 增量错误诊断
documents.onDidChangeContent((change) => {
  const textDocument = change.document;

  // The validator creates diagnostics for all uppercase words length 2 and more
  const text = textDocument.getText();
  const pattern = /\b[A-Z]{2,}\b/g;
  let m: RegExpExecArray | null;

  let problems = 0;
  const diagnostics: Diagnostic[] = [];
  while ((m = pattern.exec(text))) {
    problems++;
    const diagnostic: Diagnostic = {
      severity: DiagnosticSeverity.Warning,
      range: {
        start: textDocument.positionAt(m.index),
        end: textDocument.positionAt(m.index + m[0].length),
      },
      message: `${m[0]} is all uppercase.`,
      source: "Diagnostics Demo",
    };
    diagnostics.push(diagnostic);
  }

  // Send the computed diagnostics to VSCode.
  connection.sendDiagnostics({ uri: textDocument.uri, diagnostics });
});

// 辅助函数：根据行文本和字符位置获取当前单词及其范围
function getWordAtPosition(line: string, character: number): { word: string; start: number; end: number } | null {
  const regex = /\b\w+\b/g;
  let match: RegExpExecArray | null;
  while ((match = regex.exec(line)) !== null) {
    const start = match.index;
    const end = start + match[0].length;
    if (character >= start && character <= end) {
      return { word: match[0], start, end };
    }
  }
  return null;
}
connection.onHover((params: TextDocumentPositionParams): Hover | null => {
  const document = documents.get(params.textDocument.uri);
  if (!document) {
    return null;
  }
  const position = params.position;
  // 获取当前行的全部文本
  const lineText = document.getText({
    start: { line: position.line, character: 0 },
    end: { line: position.line, character: Number.MAX_SAFE_INTEGER }
  });
  const wordInfo = getWordAtPosition(lineText, position.character);
  if (!wordInfo) {
    return null;
  }
  const hoveredWord = wordInfo.word;
  if (keywordsDescriptionMap.has(hoveredWord)) {
    const description = keywordsDescriptionMap.get(hoveredWord);
    return {
      contents: {
        kind: 'markdown',
        value: `**${hoveredWord}**: ${description}`
      }
    };
  }
  return null;
});

connection.onDocumentFormatting(
  (params: DocumentFormattingParams): Promise<TextEdit[]> => {
    const { textDocument } = params;
    const doc = documents.get(textDocument.uri)!;
    const text = doc.getText();
    const pattern = /\b[A-Z]{3,}\b/g;
    let match;
    const res = [];
    while ((match = pattern.exec(text))) {
      res.push({
        range: {
          start: doc.positionAt(match.index),
          end: doc.positionAt(match.index + match[0].length),
        },
        newText: match[0].replace(/(?<=[A-Z])[A-Z]+/, (r) => r.toLowerCase()),
      });
    }

    return Promise.resolve(res);
  }
);

connection.onDocumentHighlight(
  (params: DocumentHighlightParams): Promise<DocumentHighlight[]> => {
    const { textDocument } = params;
    const doc = documents.get(textDocument.uri)!;
    const text = doc.getText();
    const pattern = /\btecvan\b/i;
    const res: DocumentHighlight[] = [];
    let match;
    while ((match = pattern.exec(text))) {
      res.push({
        range: {
          start: doc.positionAt(match.index),
          end: doc.positionAt(match.index + match[0].length),
        },
        kind: DocumentHighlightKind.Write,
      });
    }
    return Promise.resolve(res);
  }
);

connection.onSignatureHelp(
  (params: SignatureHelpParams): Promise<SignatureHelp> => {
    return Promise.resolve({
      signatures: [
        {
          label: "Signature Demo",
          documentation: "human readable content",
          parameters: [
            {
              label: "@p1 first param",
              documentation: "content for first param",
            },
          ],
        },
      ],
      activeSignature: 0,
      activeParameter: 0,
    });
  }
);

// This handler provides the initial list of the completion items.
connection.onCompletion(
  (_textDocumentPosition: TextDocumentPositionParams): CompletionItem[] => {
    return keywords.map((keyword: any) =>({
      label: keyword,
      kind: CompletionItemKind.Keyword,
      data: keyword
    }));
});

// This handler resolves additional information for the item selected in
// the completion list.
connection.onCompletionResolve((item: CompletionItem): CompletionItem => {
  item.detail = '${item.lable}';
  item.documentation = 'This is a build-in HLSL key word';
  return item;
});

// 处理语义令牌请求：使用正则表达式匹配关键字，避免局部匹配问题
// connection.languages.semanticTokens.on((params: SemanticTokensParams): SemanticTokens => {
//   const document = documents.get(params.textDocument.uri);
//   if (!document) {
//     return { data: [] };
//   }
//   const text = document.getText();
//   const lines = text.split(/\r?\n/);
  
//   // 为防止短关键字覆盖长关键字，先降序排序
//   const sortedKeywords = [...keywords].sort((a, b) => b.keyword.length - a.keyword.length);
//   // 构造正则表达式，确保匹配单词边界
//   const regex = new RegExp(`\\b(${sortedKeywords.map(item => item.keyword).join('|')})\\b`, 'g');
  
//   const tokens: { line: number; start: number; length: number }[] = [];
//   for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
//     const line = lines[lineIndex];
//     regex.lastIndex = 0; // 每行开始前重置状态
//     let match: RegExpExecArray | null;
//     while ((match = regex.exec(line)) !== null) {
//       tokens.push({
//         line: lineIndex,
//         start: match.index,
//         length: match[0].length
//       });
//     }
//   }
  
//   // 按文档中顺序排序，然后进行 delta 编码
//   tokens.sort((a, b) => {
//     if (a.line === b.line) {
//       return a.start - b.start;
//     }
//     return a.line - b.line;
//   });
  
//   const data: number[] = [];
//   let prevLine = 0;
//   let prevChar = 0;
//   for (const token of tokens) {
//     const deltaLine = token.line - prevLine;
//     const deltaChar = deltaLine === 0 ? token.start - prevChar : token.start;
//     data.push(deltaLine, deltaChar, token.length, 0, 0);
//     prevLine = token.line;
//     prevChar = token.start;
//   }
  
//   return { data };
// });