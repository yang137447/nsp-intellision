cmake_minimum_required(VERSION 3.15)
project(nsf_lsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(nsf_lsp
  src/main.cpp
  src/active_unit.cpp
  src/definition_fallback.cpp
  src/workspace_index.cpp
  src/server_documents.cpp
  src/server_parse.cpp
  src/server_settings.cpp
  src/server_request_handlers.cpp
  src/workspace_scan_plan.cpp
  src/call_query.cpp
  src/symbol_query.cpp
  src/hover_rendering.cpp
  src/member_query.cpp
  src/completion_rendering.cpp
  src/declaration_query.cpp
  src/ast_signature_index.cpp
  src/fast_ast.cpp
  src/full_ast.cpp
  src/include_resolver.cpp
  src/nsf_lexer.cpp
  src/hover_docs.cpp
  src/hover_markdown.cpp
  src/hlsl_builtin_docs.cpp
  src/language_registry.cpp
  src/executable_path.cpp
  src/diagnostics.cpp
  src/semantic_cache.cpp
  src/type_eval.cpp
  src/type_desc.cpp
  src/type_model.cpp
  src/type_relation.cpp
  src/overload_resolver.cpp
  src/visibility_evaluator.cpp
  src/semantic_tokens.cpp
  src/signature_help.cpp
  src/callsite_parser.cpp
  src/json.cpp
  src/resource_registry.cpp
  src/lsp_io.cpp
  src/crash_handler.cpp
  src/lsp_helpers.cpp
  src/uri_utils.cpp
  src/text_utils.cpp
  src/macro_generated_functions.cpp
)

target_compile_features(nsf_lsp PRIVATE cxx_std_17)

add_custom_command(TARGET nsf_lsp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/resources
          $<TARGET_FILE_DIR:nsf_lsp>/resources
)
