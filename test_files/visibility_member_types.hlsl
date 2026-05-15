struct SharedVisibleStruct {
    float4 SharedVisibleField;
};

float4 SharedVisibleHelper(float4 baseColor, float amount)
{
    return baseColor * amount;
}
