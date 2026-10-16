RaytracingAccelerationStructure Scene    : register(t0);
Texture2D<float>                tDepth   : register(t1);
RWTexture2D<float4>             Output   : register(u0);
SamplerState                    gLinearSampler : register(s0);

cbuffer RTShadowCB : register(b0)
{
	float4x4 gInvViewProj;
	float3   gLightPos;
	float    gLightRange;
};

struct ShadowPayload { float shadowed; };

[shader("raygeneration")]
void RayGen()
{
	uint2  idx  = DispatchRaysIndex().xy;
	uint2  dim  = DispatchRaysDimensions().xy;
	float2 uv   = (float2(idx) + 0.5f) / float2(dim);

	float depth = tDepth.SampleLevel(gLinearSampler, uv, 0).r;
	if (depth >= 0.9999f)
	{
		Output[idx] = float4(1, 1, 1, 1);
		return;
	}

	// NDC → ワールド座標
	float4 ndcPos  = float4(uv * float2(2,-2) + float2(-1,1), depth, 1);
	float4 worldH  = mul(gInvViewProj, ndcPos);
	float3 worldPos = worldH.xyz / worldH.w;

	// シャドウレイを発射する
	float3 toLight = gLightPos - worldPos;
	float  dist    = length(toLight);
	float3 dir     = toLight / dist;

	RayDesc ray;
	ray.Origin    = worldPos + dir * 0.01f;
	ray.Direction = dir;
	ray.TMin      = 0.001f;
	ray.TMax      = dist - 0.01f;

	ShadowPayload payload;
	payload.shadowed = 0.0f;
	TraceRay(Scene,
		RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
		0xFF, 0, 1, 0, ray, payload);

	float shadow = 1.0f - payload.shadowed;
	Output[idx] = float4(shadow, shadow, shadow, 1);
}
