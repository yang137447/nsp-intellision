export interface Keywords {
  keyword: string;
  description: string;
}

export const keywords: Keywords[] = [
  { keyword: "float", description: "32 位浮点数" },
  { keyword: "float2", description: "2 维浮点向量" },
  { keyword: "float3", description: "3 维浮点向量" },
  { keyword: "float4", description: "4 维浮点向量" },
  { keyword: "int", description: "32 位有符号整型" },
  { keyword: "int2", description: "2 维整型向量" },
  { keyword: "int3", description: "3 维整型向量" },
  { keyword: "int4", description: "4 维整型向量" },
  { keyword: "uint", description: "32 位无符号整型" },
  { keyword: "uint2", description: "2 维无符号整型向量" },
  { keyword: "uint3", description: "3 维无符号整型向量" },
  { keyword: "uint4", description: "4 维无符号整型向量" },
  { keyword: "bool", description: "布尔类型" },
  { keyword: "bool2", description: "2 维布尔向量" },
  { keyword: "bool3", description: "3 维布尔向量" },
  { keyword: "bool4", description: "4 维布尔向量" },
  { keyword: "matrix", description: "矩阵类型" },
  { keyword: "mul", description: "矩阵乘法函数" },
  { keyword: "lerp", description: "线性插值函数" },
  { keyword: "sample", description: "纹理采样函数" },
  { keyword: "sampler", description: "采样器状态" },
  { keyword: "texture", description: "纹理资源" }
];
