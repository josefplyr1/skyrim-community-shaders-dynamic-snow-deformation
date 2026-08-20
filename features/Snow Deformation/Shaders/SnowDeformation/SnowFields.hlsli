#ifndef __SNOW_FIELDS_DEPENDENCY_HLSL__
#define __SNOW_FIELDS_DEPENDENCY_HLSL__

// Trench-detail shaping shared verbatim by SnowShell.hlsl and
// SnowStaticsShell.hlsl (ROUTING-ROADMAP M8), so landscape and object snow
// cannot drift apart. Relies on the including shell's ShellCB
// (GridToDeformOffset, DeformInvWorldSize) and BermFieldMap (t14), declared
// before this include. Deliberately NOT shared: the deformation samplers
// (the landscape smooths bicubic, statics stays bilinear by cost) and
// BermFieldTapped, which rides each shell's own sampler.

// World-anchored value noise, shared by the border domain warp, the dune
// undulation and the churn (and any other organic-edge shaping).
float ShapeNoiseHash(float2 cell)
{
	float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float ShapeNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(ShapeNoiseHash(i), ShapeNoiseHash(i + float2(1, 0)), f.x),
		lerp(ShapeNoiseHash(i + float2(0, 1)), ShapeNoiseHash(i + float2(1, 1)), f.x), f.y);
}

// Saturates EARLY (full height once the disc is a third carved), and the old
// high-field cut is gone - both for the same reported reason. The cut's job,
// keeping berms out of carved interiors, is now done exactly by the explicit
// (1 - deformation) mask at every call site, which also lets the strip
// between two adjacent trails pile a proper ridge (the cut used to kill it).
// And a rise that kept climbing to 0.6 meant a rim point's berm grew for as
// long as the trail kept extending within the 40-unit disc - Josef watched
// ground he had already passed "morph" upward, which snow does not do. With
// the early plateau, the berm is at full height by the time the trail REACHES
// a point, and the walker meets a full lip ahead of the leading edge instead
// of raising one behind. Tail still reaches zero with zero slope.
float BermShape(float bermDeform)
{
	return smoothstep(0.02, 0.32, bermDeform);
}

// 17 taps on two staggered 8-point rings. Tap COUNT is the anti-seam: for
// a straight trail edge each tap's projection crosses zero at a different
// distance, so the field climbs in 1/17 steps instead of the 2/9 ledge a
// sparse ring printed as a visible contour line ~34 units out. Bilinear
// taps: the 17-way average supplies the smoothness bicubic would.
static const float2 kBermTaps[16] = {
	float2(18.0, 0.0), float2(12.73, 12.73), float2(0.0, 18.0), float2(-12.73, 12.73),
	float2(-18.0, 0.0), float2(-12.73, -12.73), float2(0.0, -18.0), float2(12.73, -12.73),
	float2(36.96, 15.31), float2(15.31, 36.96), float2(-15.31, 36.96), float2(-36.96, 15.31),
	float2(-36.96, -15.31), float2(-15.31, -36.96), float2(15.31, -36.96), float2(36.96, -15.31)
};

// One bilinear tap of the baked field, addressed exactly like the deformation
// map it was baked from (BermFieldCS writes texel-for-texel).
float BermFieldBaked(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	BermFieldMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = BermFieldMap.Load(int3(t0.x, t0.y, 0));
	float s10 = BermFieldMap.Load(int3(t1.x, t0.y, 0));
	float s01 = BermFieldMap.Load(int3(t0.x, t1.y, 0));
	float s11 = BermFieldMap.Load(int3(t1.x, t1.y, 0));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Churn lump noise; the caller passes its size knob (ChurnSizeScale /
// ObjChurnSizeScale), the only way the two shells differ here.
float ChurnNoiseScaled(float2 worldXY, float sizeScale)
{
	float s = max(sizeScale, 0.05);
	float n = ShapeNoise(worldXY / (16.0 * s)) * 0.65 + ShapeNoise(worldXY / (7.0 * s)) * 0.35;
	return (n - 0.5) * 2.0;
}

float ChurnWeight(float deformation, float bermDeform)
{
	return max(smoothstep(0.05, 0.5, deformation), BermShape(bermDeform));
}

#if defined(PSHADER)
// Land vertex AO, packed by the CPU window build into the terrain window's
// w channel: baked texels carry maxComponent(vertex color) * 0.499, so the
// LOD-fill provenance codes (>= 0.5) stay distinguishable. Texels without
// vertex data (LOD fill, sentinel) read as 1 = no baked occlusion.
float DecodeTerrainVertexAO(float4 texel)
{
	return (texel.x > -50000.0 && texel.w < 0.5) ? saturate(texel.w * (1.0 / 0.499)) : 1.0;
}

float SampleTerrainVertexAO(float2 gridLocal)
{
	float2 t = clamp((GridToTerrainOffset + gridLocal) / TerrainTexelSize, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim, TerrainDim) - 1);
	float s00 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t0.x, t0.y, 0)));
	float s10 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t1.x, t0.y, 0)));
	float s01 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t0.x, t1.y, 0)));
	float s11 = DecodeTerrainVertexAO(TerrainWindow.Load(int3(t1.x, t1.y, 0)));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}
#endif  // PSHADER

// EM's landscape height blending (ExtendedMaterialsTerrain.hlsli::
// ProcessTerrainHeightWeights) specialized to two surfaces: an edge fade
// contests by height instead of cross-fading through translucency. Same
// log2-space formula, same near/far sharpness ramp, same
// EnableHeightBlending gate; constants mirror EM's HEIGHT_MULT/HEIGHT_POWER.
static const float kHeightBlendMult = 8.0;
static const float kHeightBlendPower = 2.0;

float SnowHeightBlendSharpness(float viewDist)
{
	float nearBlendToFar = smoothstep(1024.0 * 1024.0, 2048.0 * 2048.0, viewDist * viewDist);
	float blendFactor = SharedData::extendedMaterialSettings.EnableHeightBlending ? sqrt(saturate(1.0 - nearBlendToFar)) : 0.0;
	return 1.0 + blendFactor * kHeightBlendPower;
}

// w' = normalize(pow(w * B^(MULT*h), B)) over {w, 1-w}. Preserves 0 and 1,
// so gates and overrides composed around it keep their meaning. EM's skip
// when the heights carry no signal is kept: without it pow() sharpens the
// fade alone and hardens it into a contour.
float SnowHeightBlend(float w, float hSnow, float hOther, float heightBlend)
{
	if (heightBlend <= 1.0 || abs(hSnow - hOther) <= 1e-3)
		return w;
	float logHeightBlend = log2(heightBlend);
	float wSnow = min(100, exp2(heightBlend * (log2(abs(w)) + kHeightBlendMult * hSnow * logHeightBlend)));
	float wOther = min(100, exp2(heightBlend * (log2(abs(1.0 - w)) + kHeightBlendMult * hOther * logHeightBlend)));
	return wSnow * rcp(max(wSnow + wOther, 1e-6));
}

// One-sided form: the neighbor surface the shell cannot sample. NOT a
// constant reference - grain near any fixed value gets no push, and "grain
// near the reference" is a spatially coherent contour, so a constant prints
// a dithered ribbon at one particular height. Instead the bar sweeps with
// the fade itself (1-w): tall grain survives to the outer edge, low grain
// dies early, and the crossing isoline moves with the ramp - no height is
// special, so partial alpha survives only on a thin moving line.
float SnowHeightBlendOneSided(float w, float hSnow, float heightBlend)
{
	return SnowHeightBlend(w, hSnow, 1.0 - w, heightBlend);
}

#endif  //__SNOW_FIELDS_DEPENDENCY_HLSL__
