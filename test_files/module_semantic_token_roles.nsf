float4 GlobalTint;
Texture2D GlobalTexture;
SamplerState GlobalSampler;
groupshared float4 SharedGroupColor;

cbuffer SuiteCBuffer {
    float4 CBufferTint;
};

struct SuiteType {
    float4 color;
};

float4 SuiteFunc(float2 uv, inout PixelMaterialInputs material) : SV_Target0
{
    float value = 1.0f;
    value += uv.x;
    if (value > 0.0f) {
        value += GlobalTexture.Sample(GlobalSampler, uv).x;
    }
    material.base_color = value.xxx;
    return SuiteType().color + GlobalTint + CBufferTint;
}
