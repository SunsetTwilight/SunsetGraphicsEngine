cbuffer Transform : register(b0) { float4x4 gMVP; float4x4 gWorld; };
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut {
	float4 pos    : SV_Position;
	float3 wNorm  : TEXCOORD0;
	float2 uv     : TEXCOORD1;
};
VSOut main(VSIn v)
{
	VSOut o;
	o.pos   = mul(gMVP,   float4(v.pos, 1.0f));
	o.wNorm = mul((float3x3)gWorld, v.nrm);
	o.uv    = v.uv;
	return o;
}
