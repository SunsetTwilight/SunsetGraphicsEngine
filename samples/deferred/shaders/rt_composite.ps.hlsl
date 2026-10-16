Texture2D<float4> rtTex : register(t0);
SamplerState      smp   : register(s0);
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD; };
float4 main(PSIn i) : SV_Target
{
	return rtTex.Sample(smp, i.uv);
}
