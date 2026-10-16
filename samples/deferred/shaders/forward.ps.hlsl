Texture2D    gTex     : register(t0);
SamplerState gSampler : register(s0);
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_Target
{
	return gTex.Sample(gSampler, i.uv);
}
