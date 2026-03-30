half3 MultiContextPartialFog(half3 color, half4 fog_factors)
{
    return color * fog_factors.xxx;
}
