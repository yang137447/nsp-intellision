float4 HelperColor()
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
float4 main_ps() : SV_Target0
{
    return HelperColor();
}
