#include "frame.hlsli"

cbuffer Object : register(b0) { float4x4 gWorld; float4 gColor; };

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; };

VSOut main(VSIn v)
{
	VSOut o;
	const float4 world = mul(gWorld, float4(v.pos, 1.0f));
	o.pos = mul(gViewProj, world);
	o.nrm = mul((float3x3)gWorld, v.nrm);
	return o;
}
