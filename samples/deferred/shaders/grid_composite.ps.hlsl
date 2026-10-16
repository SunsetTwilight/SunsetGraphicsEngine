Texture2D<float4> gGrid : register(t0);
SamplerState      gSmp  : register(s0);
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; };
float4 main(PSIn i) : SV_Target { return gGrid.Sample(gSmp, i.uv); }
