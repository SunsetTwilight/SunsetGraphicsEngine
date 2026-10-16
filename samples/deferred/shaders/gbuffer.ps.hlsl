Texture2D    gAlbedoTex : register(t0);
SamplerState gSampler   : register(s0);
struct PSIn {
	float4 pos   : SV_Position;
	float3 wNorm : TEXCOORD0;
	float2 uv    : TEXCOORD1;
};
struct MRT {
	float4 albedo   : SV_Target0;
	float4 normal   : SV_Target1;
	float4 emissive : SV_Target2;
};
MRT main(PSIn i)
{
	MRT o;
	o.albedo   = gAlbedoTex.Sample(gSampler, i.uv);
	// ワールド法線を [0,1] に正規化して格納する
	o.normal   = float4(normalize(i.wNorm) * 0.5f + 0.5f, 1.0f);
	o.emissive = float4(0.0f, 0.0f, 0.0f, 1.0f);
	return o;
}
