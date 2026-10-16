cbuffer Transform : register(b0) { float4x4 gMVP; };
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(VSIn v)
{
	VSOut o;
	o.pos = mul(gMVP, float4(v.pos, 1.0f));
	o.uv  = v.uv;
	return o;
}
