texture t_fresnel_normalMap: NormalMap
<
    string TextureFile="common\\textures\\white.tga";
>;

SamplerState s_fresnel_normalmap
{
    MipLODBias = -1;
    AddressU = WRAP;
    AddressV = WRAP;
};
