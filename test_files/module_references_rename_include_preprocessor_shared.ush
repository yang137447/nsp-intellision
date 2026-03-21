#define USE_INCLUDE_BRANCH_A 0

#if USE_INCLUDE_BRANCH_A
float4 IncludeBranchColor(float2 uv)
{
    return float4(uv.x, 0.0, 0.0, 1.0);
}

float4 IncludeBranchConsumerInactive(float2 uv)
{
    return IncludeBranchColor(uv);
}
#else
float4 IncludeBranchColor(float2 uv)
{
    return float4(uv.y, 0.0, 0.0, 1.0);
}

float4 IncludeBranchConsumerActive(float2 uv)
{
    return IncludeBranchColor(uv);
}
#endif
