cmake_minimum_required(VERSION 3.15)
project(nsf_lsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(nsf_lsp
  src/app/main.cpp
  src/app/main_background_refresh.cpp
  src/app/main_did_change_classification.cpp
  src/app/main_include_graph_cache.cpp
  src/app/main_occurrence_helpers.cpp
  src/active_unit.cpp
  src/deferred_doc_runtime.cpp
  src/document_owner.cpp
  src/document_runtime.cpp
  src/inlay_hints_runtime.cpp
  src/workspace/workspace_index.cpp
  src/workspace/workspace_index_cache.cpp
  src/workspace/workspace_index_extract.cpp
  src/workspace/workspace_index_internal.cpp
  src/workspace/workspace_index_reverse_include.cpp
  src/workspace/workspace_index_scan.cpp
  src/workspace/workspace_index_scheduler.cpp
  src/server_documents.cpp
  src/server_parse.cpp
  src/server_settings.cpp
  src/requests/server_request_handlers.cpp
  src/requests/server_request_handler_background.cpp
  src/requests/server_request_handler_completion.cpp
  src/requests/server_request_handler_definition.cpp
  src/requests/server_request_handler_hover.cpp
  src/requests/server_request_handler_references.cpp
  src/requests/server_request_handler_signature.cpp
  src/call_query.cpp
  src/immediate_syntax_diagnostics.cpp
  src/interactive_semantic_runtime.cpp
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
  src/conditional_ast.cpp
  src/expanded_source.cpp
  src/hlsl_ast.cpp
  src/semantic_snapshot.cpp
  src/preprocessor_view.cpp
  src/executable_path.cpp
  src/diagnostics/diagnostics.cpp
  src/diagnostics/diagnostics_emit.cpp
  src/diagnostics/diagnostics_expression_type.cpp
  src/diagnostics/diagnostics_indeterminate.cpp
  src/diagnostics/diagnostics_io.cpp
  src/diagnostics/diagnostics_preprocessor.cpp
  src/diagnostics/diagnostics_semantic.cpp
  src/diagnostics/diagnostics_semantic_common.cpp
  src/diagnostics/diagnostics_symbol_type.cpp
  src/diagnostics/diagnostics_syntax.cpp
  src/semantic_cache.cpp
  src/type_eval.cpp
  src/type_desc.cpp
  src/type_model.cpp
  src/type_relation.cpp
  src/overload_resolver.cpp
  src/visibility_evaluator.cpp
  src/semantic_tokens.cpp
  src/callsite_parser.cpp
  src/json.cpp
  src/resource_registry.cpp
  src/lsp_io.cpp
  src/crash_handler.cpp
  src/lsp_helpers.cpp
  src/uri_utils.cpp
  src/text_utils.cpp
  src/macro_generated_functions.cpp
  src/workspace_summary_runtime.cpp
)

target_compile_features(nsf_lsp PRIVATE cxx_std_17)
target_include_directories(nsf_lsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_custom_command(TARGET nsf_lsp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/resources
          $<TARGET_FILE_DIR:nsf_lsp>/resources
)
