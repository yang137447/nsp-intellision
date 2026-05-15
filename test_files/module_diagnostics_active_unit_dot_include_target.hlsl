float4 DotIncludeTarget(float2 uv : TEXCOORD0) : SV_Target0
{
#if DOT_INCLUDE_MACRO
    return float4(uv, 0.0, 1.0);
#else
    float2 bad = float4(0.0, 0.0, 0.0, 0.0);
    return float4(uv, bad.x, 1.0);
#endif
}
