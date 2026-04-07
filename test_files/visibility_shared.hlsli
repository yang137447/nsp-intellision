float4 VisibleSharedGlobalColor;

float4 VisibleIncludeHelper(float3 normal, float amount)
{
    return float4(normal * amount, 1.0);
}
