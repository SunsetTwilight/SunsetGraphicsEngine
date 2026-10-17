// 合成: HDR の色を露出、トーンマップ、ガンマでバックバッファに書く
#include "frame.hlsli"

Texture2D gScene : register(t0);

struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_Target
{
	float3 c = gScene.Load(int3(i.pos.xy, 0)).rgb * gExposure;
	c = c / (1.0f + c);                              // Reinhard
	const float2 d = i.uv - 0.5f;
	c *= 1.0f - 0.35f * dot(d, d) * 2.0f;            // 周辺を少し暗くする
	return float4(pow(saturate(c), 1.0f / 2.2f), 1.0f);
}
