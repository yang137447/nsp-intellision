struct VariantProfileResolvedType
{
    float variant_value;
    float resolved_only;
};

struct VariantProfileFallbackType
{
    float variant_value;
    float fallback_only;
};

#if TARGET_VARIANT_PROFILE == 1
typedef VariantProfileResolvedType VariantProfileType;
#else
typedef VariantProfileFallbackType VariantProfileType;
#endif

VariantProfileType MakeVariantProfileValue()
{
    VariantProfileType value = (VariantProfileType)0;
    value.variant_value = 1.0;
    return value;
}
