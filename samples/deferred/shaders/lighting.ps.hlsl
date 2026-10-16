Texture2D             tAlbedo   : register(t0);
Texture2D             tNormal   : register(t1);
Texture2D             tEmissive : register(t2);
Texture2D<float>      tDepth    : register(t3);
Texture2D<float4>     tRTShadow : register(t4);  // RT Shadow テクスチャ
SamplerState          gSampler  : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

struct PointLight { float3 pos; float range; float3 color; float intensity; };
cbuffer LightCB : register(b0)
{
	float4      gProjParams;
	float3      _pad0; uint _pad1;
	float4x4    gLightVP;
	float4x4    gInvView;
	float4      gViewPos;
	uint        gLightCount;
	float3      _pad2;
	PointLight  gLights[16];
};

float3 ReconstructViewPos(float2 uv, float depth)
{
	float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	float vz = gProjParams.w / (depth - gProjParams.z);
	return float3(ndc.x / gProjParams.x * vz,
				  ndc.y / gProjParams.y * vz, vz);
}

float SampleShadow(float2 uv)
{
	// RT Shadow テクスチャから直接読み取る
	return tRTShadow.Sample(gSampler, uv).r;
}

struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main(PSIn i) : SV_Target
{
	float depth = tDepth.Sample(gSampler, i.uv).r;
	if (depth >= 0.9999f) return float4(0.05f, 0.05f, 0.05f, 1.0f);

	float4 albedo   = tAlbedo  .Sample(gSampler, i.uv);
	float3 normal   = normalize(tNormal.Sample(gSampler, i.uv).xyz * 2.0f - 1.0f);
	float3 emissive = tEmissive.Sample(gSampler, i.uv).rgb;

	float3 viewPos  = ReconstructViewPos(i.uv, depth);
	float3 worldPos = mul(gInvView, float4(viewPos, 1.0f)).xyz;
	float  shadow   = SampleShadow(i.uv);

	float3 lighting = emissive + albedo.rgb * 0.15f;

	for (uint k = 0; k < gLightCount; ++k)
	{
		float3 L    = gLights[k].pos - worldPos;
		float  dist = length(L);
		if (dist >= gLights[k].range) continue;
		L /= dist;
		float atten = 1.0f - saturate(dist / gLights[k].range);
		float NdotL = max(0.0f, dot(normal, L));
		lighting += albedo.rgb * gLights[k].color
					* gLights[k].intensity * NdotL * atten * atten * shadow;
	}

	return float4(saturate(lighting), albedo.a);
}
