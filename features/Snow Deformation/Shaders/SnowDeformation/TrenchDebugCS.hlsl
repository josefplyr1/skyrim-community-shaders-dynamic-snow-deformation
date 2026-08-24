// Copies the deformation map's DEPTH channel into a single-channel texture so
// the menu can show it honestly.
//
// The map is RGBA16F and its .w was claimed by the bow wave's deposit field.
// ImGui blends by the texture's alpha, so drawing the map directly renders it
// through deposit: transparent wherever nothing has been pushed, and blank
// entirely right after a load, whatever the depth channel holds. Three rounds
// of a bug hunt were read backwards from that image.
//
// R8_UNORM samples as (depth, 0, 0, 1) - alpha is 1 by construction - so the
// copy cannot lie about what is there.

Texture2D<float4> DeformationMap : register(t0);
RWTexture2D<float> DebugDepth : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	// Displaced depth, matching what the store keeps and what the shells carve
	// from: melted ground leaves no spoil and is not a trench.
	const float4 texel = DeformationMap[dtid.xy];
	DebugDepth[dtid.xy] = saturate(texel.x - max(texel.y, 0.0));
}
