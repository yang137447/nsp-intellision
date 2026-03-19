// SampleColorTexture doc line 1
// SampleColorTexture doc line 2
half4 SampleColorTexture(Texture2D tex, sampler sam, float2 uv) {
  return tex.Sample(sam, uv);
}
