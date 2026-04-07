float4 VisibleSharedGlobalColor = float4(0.25, 0.5, 0.75, 1.0);

float4 VisibleIncludeHelper(float3 normal, float amount)
{
    return float4(normal * amount, 1.0);
}
