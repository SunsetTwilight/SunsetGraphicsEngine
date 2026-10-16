[shader("miss")]
void Miss(inout ShadowPayload payload)
{
	payload.shadowed = 0.0f;
}
