struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VSOut main(uint id : SV_VertexID)
{
	// 3頂点の大きな三角形で全画面をカバーする
	// id=0: (-1,-1) id=1: (3,-1) id=2: (-1,3)
	VSOut o;
	o.uv  = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);
	o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
	return o;
}
