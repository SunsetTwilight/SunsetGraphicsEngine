cbuffer Params : register(b0) { uint gValue; uint gMode; };
RWByteAddressBuffer gOut : register(u0);
Texture2D<float4>   gTex : register(t0);

[numthreads(4, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (gMode == 0)
	{
		// dispatchIndirect の引数 (2, 1, 1) を書く
		if (id.x == 0)
			gOut.Store3(0, uint3(2, 1, 1));
	}
	else
	{
		float4 c = gTex.Load(int3(id.x, 0, 0));
		gOut.Store(id.x * 4, gValue + (uint)(c.r * 255.0f));
	}
}
