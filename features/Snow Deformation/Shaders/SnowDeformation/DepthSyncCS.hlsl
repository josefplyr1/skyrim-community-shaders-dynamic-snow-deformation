// Syncs the snow shell's depth writes into Terrain Blending's blended depth
// copies. Screen-space passes that run after the shell (Screen Space GI in
// particular) consume the blended depth, finalized during opaque rendering;
// without this sync they still see buried geometry poking up through the
// snow and paint occlusion halos onto the shell surface.
//
// min() only takes effect where the shell drew closer than the pre-shell
// scene, so Terrain Blending's soft-blend depth values survive everywhere
// the shell is absent. The 16-bit copy is written from the 32-bit one's
// value (typed UAV loads are only guaranteed for R32).

Texture2D<float> MainDepth : register(t0);
RWTexture2D<float> BlendedDepth : register(u0);
RWTexture2D<float> BlendedDepth16 : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float src = MainDepth[dtid.xy];
	float blended = min(BlendedDepth[dtid.xy], src);
	BlendedDepth[dtid.xy] = blended;
	BlendedDepth16[dtid.xy] = blended;
}
