cbuffer Transform : register(b0) { float4x4 gVP; };
float4 main(float3 pos : POSITION) : SV_Position
{
	float4 clip = mul(gVP, float4(pos, 1.0f));
	// クリップ空間で Z をわずかに手前にずらして Z-fighting を防止する
	clip.z -= 0.0001f * clip.w;
	return clip;
}
