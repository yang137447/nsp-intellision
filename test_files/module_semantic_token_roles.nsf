float4 GlobalTint;

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
    material.base_color = value.xxx;
    return SuiteType().color + GlobalTint + CBufferTint;
}
