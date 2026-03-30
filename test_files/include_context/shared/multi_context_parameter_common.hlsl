half4 MultiContextParameterTone(half3 color, half4 fog_factors)
{
    return fog_factors + half4(color, 0.0);
}

half3 MultiContextParameterEntry(half3 color, half4 fog_factors)
{
    half4 final_fog = MultiContextParameterTone(color, fog_factors);
    return final_fog.rgb;
}
