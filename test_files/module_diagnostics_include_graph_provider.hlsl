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

SamplerState s_distort1
{
    MipLODBias = -1;
    AddressU = WRAP;
    AddressV = WRAP;
};
