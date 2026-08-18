// Berm field bake.
//
// The berm field is a 17-tap disc average of the deformation map over a ~40
// unit radius (see BermField in SnowShell.hlsl). It is a pure function of the
// map, yet the shells recomputed it per PIXEL - four times over, for the
// central difference the normal needs, plus once more per vertex. At 68 loads
// a call that dominated the shell's pixel cost.
//
// Baking it once per map texel here turns every one of those calls into a
// single bilinear tap. The kernel, the tap order and the zero-outside-window
// rule below mirror BermField exactly, so the baked field equals what the
// per-pixel path produced at texel centres; between centres the consumers'
// bilinear filtering stands in for the 17-way average, which is already
// smooth at a far coarser scale than one texel.

Texture2D<float> DeformationMap : register(t0);
RWTexture2D<float> OutBermField : register(u0);

// Shares DeformationUpdateCS's PerFrame buffer; only TexelSize is read, but
// the leading layout must match it exactly.
cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	int2 ScrollDelta;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	float StampFalloffStart;
	float StampNoiseAmp;
	float2 WindBias;

	float DeltaTime;
	float MeltCeiling;
	float2 perFramePad;
}

// Must match kBermTaps in SnowShell.hlsl / SnowStaticsShell.hlsl.
static const float2 kBermTaps[16] = {
	float2(18.0, 0.0), float2(12.73, 12.73), float2(0.0, 18.0), float2(-12.73, 12.73),
	float2(-18.0, 0.0), float2(-12.73, -12.73), float2(0.0, -18.0), float2(12.73, -12.73),
	float2(36.96, 15.31), float2(15.31, 36.96), float2(-15.31, 36.96), float2(-36.96, 15.31),
	float2(-36.96, -15.31), float2(-15.31, -36.96), float2(15.31, -36.96), float2(36.96, -15.31)
};

// Bilinear tap in texel coordinates, clamped to the edge - the Load-based
// filtering SampleDeformationBilinear performs in the shells.
float TapBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = DeformationMap.Load(int3(t0.x, t0.y, 0));
	float s10 = DeformationMap.Load(int3(t1.x, t0.y, 0));
	float s01 = DeformationMap.Load(int3(t0.x, t1.y, 0));
	float s11 = DeformationMap.Load(int3(t1.x, t1.y, 0));

	// Saturated: melt writes past 1.0 into the refill headroom, and a berm
	// averaged over raw over-melt values would throw a ridge proportional to
	// invisible depth.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

// A tap whose window UV leaves [0,1] contributes 0, matching
// SampleDeformationFast: the field must decay to nothing at the window border
// rather than smearing the edge texels inward.
float Tap(float2 texel, float2 dims)
{
	float2 uv = (texel + 0.5) / dims;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	return TapBilinear(texel, dims);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (any(float2(dtid.xy) >= dims))
		return;

	// Tap offsets are authored in world units; the window resizes with the
	// Trenches range slider, so convert through the live texel size.
	float invTexel = 1.0 / max(TexelSize, 1e-4);
	float2 centre = float2(dtid.xy);

	float b = Tap(centre, dims);
	[unroll] for (int i = 0; i < 16; i++)
		b += Tap(centre + kBermTaps[i] * invTexel, dims);

	OutBermField[dtid.xy] = saturate(b / 17.0);
}
