cbuffer ShadowCB : register(b0) { float4x4 gLightMVP; };
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
float4 main(VSIn v) : SV_Position
{
	return mul(gLightMVP, float4(v.pos, 1.0f));
}
