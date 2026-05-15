struct PixelMaterialInputs
{
    float4 base_color;
    float opacity;
};

cbuffer GlobalConstants1
{
    float u_frame_time_radian;
};

texture t_distort1: DistortMap
<
    string TextureFile="common\\textures\\white.tga";
>;

float u_include_metadata_scale
<
    string SasUiLabel = "Include metadata scale";
    string SasUiControl = "FloatSlider";
    float SasUiMin = 0.0;
    float SasUiMax = 4.0;
> = 1.0f;

SamplerState s_distort1
{
    MipLODBias = -1;
    AddressU = WRAP;
    AddressV = WRAP;
};
