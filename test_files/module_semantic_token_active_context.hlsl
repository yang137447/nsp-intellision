#ifdef ENABLE_ACTIVE_TOKEN
float4 ActiveContextTint;
#else
float4 StandaloneContextTint;
#endif

float4 ActiveContextFunc(float2 uv) : SV_Target0
{
    float activeValue = ActiveContextTint.x;
    return ActiveContextTint + activeValue;
}
