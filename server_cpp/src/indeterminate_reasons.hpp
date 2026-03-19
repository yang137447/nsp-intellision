#pragma once

namespace IndeterminateReason {
inline constexpr const char *DiagnosticsRhsTypeEmpty = "rhs_type_empty";
inline constexpr const char *DiagnosticsBudgetTimeout = "budget_timeout";
inline constexpr const char *DiagnosticsHeavyRulesSkipped =
    "heavy_rules_skipped";
inline constexpr const char *DiagnosticsBuiltinUnmodeled = "builtin_unmodeled";
inline constexpr const char *DiagnosticsBuiltinArgTypeUnknown =
    "builtin_arg_type_unknown";
inline constexpr const char *DiagnosticsBuiltinMethodUnmodeled =
    "builtin_method_unmodeled";
inline constexpr const char *DiagnosticsBuiltinMethodArgTypeUnknown =
    "builtin_method_arg_type_unknown";
inline constexpr const char *DiagnosticsBuiltinRegistryUnavailable =
    "builtin_registry_unavailable";
inline constexpr const char *DiagnosticsFunctionSignatureLowConfidence =
    "function_signature_low_confidence";

inline constexpr const char *SignatureHelpCallTargetUnknown =
    "call_target_unknown";
inline constexpr const char *SignatureHelpDefinitionTextUnavailable =
    "definition_text_unavailable";
inline constexpr const char *SignatureHelpSignatureExtractFailed =
    "signature_extract_failed";
inline constexpr const char *SignatureHelpBuiltinRegistryUnavailable =
    "builtin_registry_unavailable";
inline constexpr const char *SignatureHelpBuiltinUnmodeled = "builtin_unmodeled";
inline constexpr const char *SignatureHelpOther = "other";
} // namespace IndeterminateReason
