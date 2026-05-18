struct VariantProfileCsvResolvedType
{
    float variant_value;
    float resolved_only;
};

struct VariantProfileCsvFallbackType
{
    float variant_value;
    float fallback_only;
};

#if CSV_STABLE_PROFILE == 3
typedef VariantProfileCsvResolvedType VariantProfileCsvType;
#else
typedef VariantProfileCsvFallbackType VariantProfileCsvType;
#endif

VariantProfileCsvType MakeVariantProfileCsvValue()
{
    VariantProfileCsvType value = (VariantProfileCsvType)0;
    value.variant_value = 3.0;
    return value;
}
