cbuffer Object : register(b0) { float4x4 gWorld; float4 gColor; };

struct PSIn { float4 pos : SV_Position; float3 nrm : NORMAL; };
struct MRT
{
	float4 albedo : SV_Target0;
	float4 normal : SV_Target1;   // ワールドの法線を [0, 1] にしたもの
};

MRT main(PSIn i)
{
	MRT o;
	o.albedo = gColor;
	o.normal = float4(normalize(i.nrm) * 0.5f + 0.5f, 1.0f);
	return o;
}
