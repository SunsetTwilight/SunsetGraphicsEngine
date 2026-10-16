[shader("closesthit")]
void ClosestHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
	payload.shadowed = 1.0f;
}
