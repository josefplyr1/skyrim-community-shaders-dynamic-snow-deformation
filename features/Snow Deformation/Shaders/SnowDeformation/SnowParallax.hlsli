// Snow parallax and anti-tiling tap machinery, shared by BOTH shells.
//
// Lives here rather than duplicated per shell so the landscape shell and the
// object skins cannot drift apart: identical parallax response either side of
// the SnowSnowFade cross-fade is the entire point of the exercise.
//
// Requires, already declared by the includer: SnowHeightMap (t8),
// SnowSampler (s0), ShellCB's SnowParallax, and under PSHADER also
// ExtendedMaterials/ExtendedMaterials.hlsli (DisplacementParams and the
// AdjustDisplacementNormalized / ParallaxShadowTapCount / Shadow+Quality
// constants this reuses verbatim).

#ifndef SNOW_DEFORMATION_SNOWPARALLAX_HLSLI
#define SNOW_DEFORMATION_SNOWPARALLAX_HLSLI

#if defined(DOMAINSHADER) || defined(PSHADER)
// Cheap 2D cell hash for stochastic tiling offsets.
float2 StochasticHash(float2 cell)
{
	float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return frac(float2((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y));
}

// Anti-tiling snow fetch: blend 3 taps of the texture at random per-cell UV
// offsets over a triangular lattice, so the texture repeat never lines up.
// Weight sharpening keeps the cross-fade zones from reading as ghosted
// double-images. Taps are computed once and applied to every snow map
// (albedo, normal, RMAOS) so all channels agree on the same offsets.
struct SnowTaps
{
	float2 uv0, uv1, uv2;
	float3 weights;
	float2 duvdx, duvdy;
};

// Derivative-free core, so the DOMAIN shader can build the same taps the
// pixel shader will shade that point with. Only SampleGrad needs
// duvdx/duvdy, and only the PS uses SampleGrad; the DS samples at an
// explicit mip. The blended field is continuous across lattice cell
// boundaries (the barycentric weight of a departing tap reaches zero there),
// so vertices and pixels landing in different cells still agree.
SnowTaps ComputeSnowTapsNoGrad(float2 uv, float2 worldXY)
{
	// World-anchored lattice (~427 units per cell): the snow uv rebases by
	// tile multiples as the camera-following grid moves, so a uv-derived
	// lattice jumps with the camera. Hash cell selection tolerates absolute-
	// coordinate float error (unlike height-field finite differences).
	// Deliberately NOT tied to kSnowUVTile: cell size is a world-space feature
	// size, held fixed so the tiling change is the only variable in-game.
	// First dial if the finer tile reads repetitive (2.5 repeats/cell now).
	float2 lattice = mul(float2x2(1.0, -0.57735027, 0.0, 1.15470054), worldXY * (0.6 / 256.0));
	float2 cellBase = floor(lattice);
	float2 f = frac(lattice);

	float2 v0, v1, v2;
	float3 bary;
	if (f.x + f.y < 1.0) {
		v0 = cellBase;
		v1 = cellBase + float2(1, 0);
		v2 = cellBase + float2(0, 1);
		bary = float3(1.0 - f.x - f.y, f.x, f.y);
	} else {
		v0 = cellBase + float2(1, 1);
		v1 = cellBase + float2(0, 1);
		v2 = cellBase + float2(1, 0);
		bary = float3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
	}

	bary = pow(bary, 4.0);
	bary /= dot(bary, 1.0);

	SnowTaps taps;
	taps.uv0 = uv + StochasticHash(v0);
	taps.uv1 = uv + StochasticHash(v1);
	taps.uv2 = uv + StochasticHash(v2);
	taps.weights = bary;
	taps.duvdx = 0.0.xx;
	taps.duvdy = 0.0.xx;
	return taps;
}

// Displacement through the SAME anti-tiling taps as every other map, so the
// domain shader's geometry and the pixel shader's shading describe ONE
// height field. Each tap reads an unrelated patch of the texture, so a
// single un-offset fetch describes a field decorrelated from what is drawn.
// The taps' offsets are per-cell constants, so walking a ray by uvOffset on
// each of them is exact. Explicit mip: the shadow loop's fetches must be
// uniform, and a blurred height field resolves to mush.
float SampleSnowHeight(SnowTaps taps, float2 uvOffset, float mip)
{
	return taps.weights.x * SnowHeightMap.SampleLevel(SnowSampler, taps.uv0 + uvOffset, mip).x +
	       taps.weights.y * SnowHeightMap.SampleLevel(SnowSampler, taps.uv1 + uvOffset, mip).x +
	       taps.weights.z * SnowHeightMap.SampleLevel(SnowSampler, taps.uv2 + uvOffset, mip).x;
}
#endif  // DOMAINSHADER || PSHADER

#ifdef PSHADER

SnowTaps ComputeSnowTaps(float2 uv, float2 worldXY)
{
	SnowTaps taps = ComputeSnowTapsNoGrad(uv, worldXY);
	// All taps share the continuous base uv's derivatives: the per-cell
	// offsets jump at lattice seams, and sampler-derived gradients there make
	// anisotropic filtering fetch the deepest mips (discolored streaks).
	taps.duvdx = ddx(uv);
	taps.duvdy = ddy(uv);
	return taps;
}

float4 SampleSnowMap(Texture2D<float4> tex, SnowTaps taps)
{
	return taps.weights.x * tex.SampleGrad(SnowSampler, taps.uv0, taps.duvdx, taps.duvdy) +
	       taps.weights.y * tex.SampleGrad(SnowSampler, taps.uv1, taps.duvdx, taps.duvdy) +
	       taps.weights.z * tex.SampleGrad(SnowSampler, taps.uv2, taps.duvdx, taps.duvdy);
}

// Mip for the height march, by the same rule as
// ExtendedMaterials::GetMipLevelFromDims' PARALLAX path: MIN of the
// derivatives (standard mipmapping takes max), then floor. Deliberately
// sharper than hardware - a blurred height field resolves to mush. Takes
// derivatives, so call it in uniform flow, not inside the shadow branch.
float SnowHeightMip(float2 uv)
{
	float2 dims;
	SnowHeightMap.GetDimensions(dims.x, dims.y);
	float2 texels = uv * dims;
	float2 dx = ddx(texels);
	float2 dy = ddy(texels);
	return floor(max(0.5 * log2(max(min(dot(dx, dx), dot(dy, dy)), 1e-8)) + SharedData::MipBias, 0.0));
}

// Extended Materials' parallax soft shadow (Tatarchuk 2006) with the four
// fetches routed through the anti-tiling taps. Returns raw OCCLUSION, before
// the saturate, so a caller blending more than one planar projection can mix
// them and clamp once.
// MUST stay in step with the copy in SnowStaticsShell.hlsl.
float SnowParallaxOcclusion(SnowTaps taps, float2 lightUV, float mip, float quality, float noise, DisplacementParams params)
{
	uint tapCount = ExtendedMaterials::ParallaxShadowTapCount(quality);
	float shadowStrength = ExtendedMaterials::ShadowIntensity * (4.0 / tapCount);
	float2 rayDir = lightUV * 0.1 * params.HeightScale;
	float4 multipliers = rcp(float4(1, 2, 3, 4) + noise);

	float sh0 = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, 0.0.xx, mip), params);
	// Unwritten lanes stay at sh0 and contribute zero occlusion.
	float4 sh = sh0.xxxx;
	sh.x = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, rayDir * multipliers.x, mip), params);
	if (quality > 0.25)
		sh.y = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, rayDir * multipliers.y, mip), params);
	if (quality > 0.5)
		sh.z = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, rayDir * multipliers.z, mip), params);
	if (quality > 0.75)
		sh.w = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, rayDir * multipliers.w, mip), params);
	return dot(max(0.0, sh - sh0), shadowStrength);
}

// The shells' shared DisplacementParams: HeightScale is the PBR JSON
// displacementScale verbatim, because kSnowUVTile equals the landscape
// tiling and EM's UV-space slab depth therefore lands on the same world
// depth the ground beside us gets.
DisplacementParams SnowDisplacementParams()
{
	DisplacementParams params;
	params.DisplacementScale = 1.0;
	params.DisplacementOffset = 0.0;
	params.HeightScale = SnowParallax.x;
	params.FlattenAmount = 0.0;
	return params;
}

// Near/far tap budget, Extended Materials' own thresholds.
float SnowParallaxQuality(float viewDist)
{
	return viewDist < ExtendedMaterials::ParallaxCheapDistance ?
	           ExtendedMaterials::ParallaxNearShadowQuality :
	           ExtendedMaterials::ParallaxFarShadowQuality;
}

// Shift a tap set's uvs without rebuilding the lattice: the per-cell hash
// offsets and barycentric weights belong to the WORLD position and must not
SnowTaps OffsetSnowTaps(SnowTaps taps, float2 uvOffset)
{
	taps.uv0 += uvOffset;
	taps.uv1 += uvOffset;
	taps.uv2 += uvOffset;
	return taps;
}

// Parallax occlusion march: Extended Materials' GetParallaxCoords, with the
// fetches routed through the anti-tiling taps so the depth it resolves is the
// depth of the grain that actually gets drawn. Structure, the grazing-angle
// limiter, the contact refinement and the secant solve are EM's; the loop is
// scalar rather than quad-vectorized because each of our fetches is already a
// 3-tap blend, so the cost lives in the taps and not in the lane count.
//
// Returns a uv OFFSET (zero when disabled), applied to the base uv and to
// every tap. Marching the blended field rather than the dominant tap is
// deliberate: the dominant tap flips at Voronoi boundaries, and a flip means
float2 SnowParallaxOffset(SnowTaps taps, float3 viewTS, float mip, float distFade, uint maxSteps, DisplacementParams params)
{
	// EM's grazing limiter, NOT a true 1/z: unbounded shear at grazing angles
	// breaks the sampling derivatives into marbling. That is the failure that
	// killed the 2026-08-14 attempt (7ce548ba), and the reason this divisor
	// looks arbitrary.
	viewTS.xy /= viewTS.z * 0.7 + 0.3 + params.FlattenAmount;

	float maxHeight = 0.1 * params.HeightScale;
	float minHeight = maxHeight * 0.5;

	uint numSteps = (uint)max(4.0, round(maxSteps * (1.0 - distFade)));
	float stepSize = rcp((float)numSteps);
	float2 perStep = viewTS.xy * maxHeight * stepSize;

	// Ray enters half a slab above the polygon plane: displacement 0.5 IS the
	// plane (EM centres its height convention), so relief runs both ways.
	float2 prevOffset = viewTS.xy * minHeight;
	float prevBound = 1.0;
	float prevHeight = 1.0;

	float2 pt1 = 0.0.xx;
	float2 pt2 = 0.0.xx;

	uint stepsLeft = numSteps;
	bool refined = false;
	[loop] while (stepsLeft > 0)
	{
		float2 offs = prevOffset - perStep;
		float bound = prevBound - stepSize;
		float h = ExtendedMaterials::AdjustDisplacementNormalized(SampleSnowHeight(taps, offs, mip), params);
		[branch] if (h >= bound)
		{
			pt1 = float2(bound, h);
			pt2 = float2(prevBound, prevHeight);
			if (refined)
				break;
			// Contact refinement: re-march the straddling interval at the full
			// budget, so N steps resolve like N*N. prev* still holds the empty
			// end of the interval, which is where the finer march restarts.
			refined = true;
			stepsLeft = numSteps;
			stepSize /= (float)numSteps;
			perStep /= (float)numSteps;
			continue;
		}
		prevOffset = offs;
		prevBound = bound;
		prevHeight = h;
		stepsLeft--;
	}

	// Line-line intersection of the ray against the height segment.
	float d2 = pt2.x - pt2.y;
	float d1 = pt1.x - pt1.y;
	float denom = d2 - d1;
	float parallaxAmount = denom == 0.0 ? 0.0 : (pt1.x * d2 - pt2.x * d1) / denom;

	float offset = (1.0 - parallaxAmount) * -maxHeight + minHeight;
	return viewTS.xy * offset * (1.0 - distFade);
}
#endif  // PSHADER

#endif  // SNOW_DEFORMATION_SNOWPARALLAX_HLSLI
