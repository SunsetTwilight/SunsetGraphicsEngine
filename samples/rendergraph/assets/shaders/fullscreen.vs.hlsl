// 全画面の三角形 (頂点バッファなし)
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut main(uint id : SV_VertexID)
{
	VSOut o;
	o.uv  = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return o;
}
