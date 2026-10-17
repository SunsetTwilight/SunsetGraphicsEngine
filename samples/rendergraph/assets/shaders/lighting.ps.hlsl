// ディファードのライティング: G-Buffer と深度から、HDR の色を求める
#include "frame.hlsli"

Texture2D        gAlbedo : register(t0);
Texture2D        gNormal : register(t1);
Texture2D<float> gDepth  : register(t2);

struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_Target
{
	const int3  p     = int3(i.pos.xy, 0);
	const float depth = gDepth.Load(p);
	if (depth >= 1.0f)
	{
		// 空
		return float4(lerp(float3(0.30f, 0.34f, 0.40f), float3(0.05f, 0.08f, 0.16f), i.uv.y), 1.0f);
	}

	const float3 albedo = gAlbedo.Load(p).rgb;
	const float3 n      = normalize(gNormal.Load(p).xyz * 2.0f - 1.0f);

	// 深度からワールド座標を戻す
	const float2 ndc   = float2(i.uv.x * 2.0f - 1.0f, 1.0f - i.uv.y * 2.0f);
	float4       world = mul(gInvViewProj, float4(ndc, depth, 1.0f));
	world.xyz /= world.w;

	float3 color = albedo * gAmbient.rgb;
	color += albedo * gSunColor.rgb * saturate(dot(n, -gSunDir.xyz));
	for (uint k = 0; k < gLightCount; ++k)
	{
		float3      l    = gLights[k].pos - world.xyz;
		const float dist = length(l);
		if (dist >= gLights[k].range)
			continue;
		l /= dist;
		const float atten = 1.0f - saturate(dist / gLights[k].range);
		color += albedo * gLights[k].color * gLights[k].intensity * saturate(dot(n, l)) * atten * atten;
	}
	return float4(color, 1.0f);
}
