struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID)
{
	// フルスクリーントライアングル (頂点バッファ不要)
	float2 uv  = float2((vid << 1) & 2, vid & 2);
	VSOut o;
	o.pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
	o.uv  = uv;
	return o;
}
