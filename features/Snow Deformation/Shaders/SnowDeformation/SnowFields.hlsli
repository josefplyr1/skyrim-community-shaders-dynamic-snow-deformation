// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#ifndef __SNOW_FIELDS_DEPENDENCY_HLSL__
#define __SNOW_FIELDS_DEPENDENCY_HLSL__

// Trench-detail shaping, spell-mark readers and shared field surfaces,
// verbatim-identical in SnowShell.hlsl and SnowStaticsShell.hlsl
// (ROUTING-ROADMAP M8), so landscape and
// object snow cannot drift apart. Relies on the including shell's ShellCB
// (GridToDeformOffset, DeformInvWorldSize, ExclusionFieldWindow,
// UndulationAmp/Scale, UndulationFieldWindow, TrampleStyle,
// GridOrigin, DeformMapOrigin, WarpedHalfSpan, SeamBounds/SeamRampInv), DeformationMap
// (t1) with the shell's DeformTexel torus helper, BermFieldMap
// (t14), UntouchedTiles (t57), ExclusionFieldMap (t15), the frost patterns (t16/t17),
// UndulationFieldMap (t29), SnowSampler
// (PS) and the TerrainVariation include - all declared before this include.
// Deliberately NOT shared: the deformation .x samplers (the landscape
// smooths bicubic, statics stays bilinear by cost).

// ShapeNoiseHash / ShapeNoise / UndulationNorm live in SnowNoise.hlsli so
// the undulation bake CS shares the exact functions without the ShellCB
// context. Callers here are unchanged.
#include "SnowDeformation/SnowNoise.hlsli"

// The rim teeth's value noise. Pixel passes that bind the hash table (t109,
// DrawShell) read it in one fetch; every other stage evaluates the same torus
// noise from the hash.
#if defined(SNOW_NOISE_TABLE)
Texture2D<float> SnowNoiseHash : register(t109);
// Trench detail tiles (tools/churn-tiles), slices powder / firm / wet: N = normal
// xy (DirectX), H = (height about the tile mean, cavity).
Texture2DArray<float2> SnowTrenchDetailN : register(t112);
Texture2DArray<float2> SnowTrenchDetailH : register(t113);

float SnowDetailNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	return SnowNoiseHash.SampleLevel(SnowSampler, (i + f * f * (3.0 - 2.0 * f) + 0.5) / kNoiseTorusCells, 0.0);
}
#else
float SnowDetailNoise(float2 p)
{
	return ShapeNoiseTorus(p, kNoiseTorusCells);
}
#endif

// Lattice cells the stochastic hash repeats after: it multiplies the cell
// index by ~3300 and needs it small to keep a fraction. CS's sampler wraps
// the hash alone, so the grid itself has no seam.
static const float kTrenchDetailLatticePeriod = 64.0;

// Field map sizes. The landscape shell (SNOW_FIELD_DIMS) takes them from
// ShellCB.FieldDims, GetDimensions of what DrawShell binds (0 where unbound);
// elsewhere the macros expand to the query itself, token for token.
#ifdef SNOW_FIELD_DIMS
#	define SNOW_DEFORM_DIMS(d) float2 d = FieldDims.xx
#	define SNOW_BERM_DIMS(d) float2 d = FieldDims.yy
#	define SNOW_EXCLUSION_DIMS(d) float2 d = FieldDims.ww
#else
#	define SNOW_DEFORM_DIMS(d) 		float2 d;               		DeformationMap.GetDimensions(d.x, d.y)
#	define SNOW_BERM_DIMS(d) 		float2 d;             		BermFieldMap.GetDimensions(d.x, d.y)
#	define SNOW_EXCLUSION_DIMS(d) 		float2 d;                  		ExclusionFieldMap.GetDimensions(d.x, d.y)
#endif

// Two-octave domain warp for a boundary, a_noise world units strong: a capped
// 37-unit coarse wander plus a fine 8-unit octave for raggedness. Moves WHERE
// a border falls without touching what is sampled there, so it stops following
// the data lattice and reads as an organic edge instead of a staircase. The
// trench patch's silhouette.
float2 BorderJitter(float2 worldXY, float a_noise)
{
	float coarseAmp = min(a_noise, 16.0);
	return float2(
			   ShapeNoise(worldXY / 37.0) - 0.5,
			   ShapeNoise(worldXY / 37.0 + 111.7) - 0.5) *
	           (2.0 * coarseAmp) +
	       float2(
			   ShapeNoise(worldXY / 8.0) - 0.5,
			   ShapeNoise(worldXY / 8.0 + 57.3) - 0.5) *
	           (1.2 * a_noise);
}

// The ground snow's edge fade: 1 inside its span, 0 at the loaded-cell seam
// (SeamBounds, where the game swaps full terrain for LOD meshes) across
// SeamRampInv's band, and at the grid's own physical edge.
float ShellEdgeFade(float2 gridLocal)
{
	float2 delta = abs(gridLocal - WarpedHalfSpan);
	float fade = saturate((WarpedHalfSpan - max(delta.x, delta.y)) / 2048.0);
	[flatten] if (SeamRampInv > 0.0)
	{
		float2 worldXY = GridOrigin + gridLocal;
		float inside = min(min(worldXY.x - SeamBounds.x, worldXY.y - SeamBounds.y),
			min(SeamBounds.z - worldXY.x, SeamBounds.w - worldXY.y));
		fade = min(fade, saturate(inside * SeamRampInv));
	}
	return fade;
}

// Across the band the snow's depth runs linearly down to its ground (0 at the
// seam); bare ground's negative depth is left as it is.
float ShellEdgeDepth(float a_depth, float a_fade)
{
	return min(a_depth, a_depth * a_fade);
}

// Saturates EARLY (full height once the disc is a third carved), and the old
// high-field cut is gone - both for the same reported reason. The cut's job,
// keeping berms out of carved interiors, is now done exactly by the explicit
// (1 - deformation) mask at every call site, which also lets the strip
// between two adjacent trails pile a proper ridge (the cut used to kill it).
// And a rise that kept climbing to 0.6 meant a rim point's berm grew for as
// long as the trail kept extending within the 40-unit disc, so ground already
// passed appeared to morph upward, which snow does not do. With
// the early plateau, the berm is at full height by the time the trail REACHES
// a point, and the walker meets a full lip ahead of the leading edge instead
// of raising one behind. Tail still reaches zero with zero slope.
float BermShape(float bermDeform)
{
	return smoothstep(0.02, 0.32, bermDeform);
}

// Berm Steepness scales the bake's rings (BermFieldCS kBermTaps) by
// BermReach. The shading step follows it, floored at half a map texel (below
// that the bilinear bake shades in facets); exactly 4 at the default reach.
float BermGradStep()
{
	SNOW_DEFORM_DIMS(dims);
	return min(4.0, max(4.0 * BermReach, 0.5 / (DeformInvWorldSize * dims.x)));
}

// The carve's short blur (BermFieldCS .y): the centre and a ring of eight
// this far out, world units. CarveProfile lays walls back toward it by the
// slough (CompactLook.w). Mirror in BermFieldCS.hlsl.
static const float kWallBlurReach = 12.0;

// One bilinear tap of the baked field, addressed exactly like the deformation
// map it was baked from (BermFieldCS writes texel-for-texel, same toroidal
// layout and MapOrigin - so partial rebuilds survive scrolls). x = berm,
// y = the carve's short blur, z = the carve's date, w = the berm's date
// (TrackAgeOf).
float4 BermFieldBaked(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_BERM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
#ifdef SNOW_DEFORM_SAMPLER
	float4 v = BermFieldMap.SampleLevel(SnowSampler, (t + 0.5 + float2(DeformMapOrigin)) / dims, 0.0);
#else
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = BermFieldMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = BermFieldMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = BermFieldMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = BermFieldMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));

	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
#endif
	// The dates are baked times their weights (BermFieldCS).
	v.z = v.y > 1e-5 ? v.z / v.y : 0.0;
	v.w = v.x > 1e-5 ? v.w / v.x : 0.0;
	return v;
}

// ---- Track age (TRACK-VARIATION-RESEARCH.md 3.4) ----
// A baked date's age on the ageing clock (TrackAge.x now): 0 just cut, 1
// aged (TrackAge.y per clock unit; 0 = tracks never age). No date (0) reads
// new: nothing is cut there for an age to shape, and an aged reading would
// fetch the walls' blur for nothing. Snowfall runs the clock fastest, so an
// hour of it rounds a print the way a day of calm cold does.
float TrackAgeOf(float a_date)
{
	return a_date > 0.0 ? saturate((TrackAge.x - a_date) * TrackAge.y) : 0.0;
}

// Age's say in a carve's shape (CarveProfile), worked out once per point:
// x how far the walls round toward the carve's short blur, over the whole
// age; y the rim teeth's strength (RimStyle.y), gone first; z the lip's
// (RimStyle.x). Linear ramps: the age moves slowly, and the profile runs
// several times a pixel. TrackCarveLook(0) is the fresh cut, the look before
// ageing.
float3 TrackCarveLook(float a_age)
{
	return float3(saturate(a_age * 1.1111 - 0.1111), RimStyle.y * saturate(1.0 - a_age * 2.857), RimStyle.x * saturate(1.1111 - a_age * 2.2222));
}

// What age leaves of a berm: snowfall buries it with everything else, and
// settling rounds what stands proud first.
float TrackBermScale(float a_age)
{
	return 1.0 - 0.6 * saturate(a_age * 1.25 - 0.25);
}

// What age leaves of the trench detail: new snow settles over the clods, so
// an old track wears the powder set at a third of its relief. Never none:
// disturbed snow reads crisp in every condition.
float TrackDetailReliefScale(float a_age)
{
	return 1.0 - 0.65 * saturate(a_age * 1.6667);
}

// An old hit's clods are under new snow: the impact set fades with age.
float TrackDetailImpactScale(float a_age)
{
	return saturate(1.0 - a_age * 2.5);
}

// The sun heightfield march's tap distances, both shells. Inside the trail
// map the first three give way to kSunNearTaps and the sun envelope.
static const float kSunMarchDist[5] = { 12.0, 32.0, 90.0, 300.0, 1000.0 };
// Inside the trail map (MarchNearHorizon): exact taps at the receiver, in map
// texels, then the sun envelope read kSunEnvelopeShift texels sunward, where
// every texel its bilinear blends lies past the receiver.
static const float kSunNearTaps[4] = { 0.45, 0.9, 1.45, 2.05 };
static const float kSunEnvelopeShift = 1.5;
// The envelope bake's run (SnowShell.hlsl MarchSunCS), in map texels: half
// texels to 10, whole texels to 28.
static const uint kSunEnvelopeSteps = 38;

float SunEnvelopeRun(uint a_k)
{
	return a_k <= 20u ? 0.5 * a_k : 10.0 + (float)(a_k - 20u);
}

#if defined(PSHADER) || defined(SNOW_MARCH_SUN_CS)
// The sun marches' occluder heights, baked (MarchCache.cpp): per deformation
// texel in the map's own torus, the ground's tap height (MarchHeightCS) and
// the road's where the statics march takes a road tap, sentinel elsewhere
// (RoadMarchHeightCS); past the trail map, the ground's per land-layer
// vertex over the FineWindow (MarchHeightFarCS).
Texture2D<float> MarchHeightGround : register(t106);
Texture2D<float> MarchHeightRoad : register(t107);
Texture2D<float> MarchHeightFar : register(t108);
// The sun envelope over the ground's heights (SnowShell.hlsl MarchSunCS), the
// same torus: per texel, the highest occluder toward the sun less its run
// times the sun's tangent.
Texture2D<float> MarchSunEnvelope : register(t100);

// A cache's four texels around a grid-local point (s00, s10, s01, s11), one
// gather on s0 (it wraps like DeformTexel); a_f = the bilinear weights.
float4 MarchCacheTexels(Texture2D<float> a_map, float2 a_gridLocal, out float2 a_f)
{
	SNOW_DEFORM_DIMS(dims);
	const float2 t = (GridToDeformOffset + a_gridLocal) * DeformInvWorldSize * dims - 0.5;
	const float2 t0 = floor(t);
	a_f = t - t0;
	return a_map.GatherRed(SnowSampler, (t0 + float2(DeformMapOrigin) + 1.0) / dims).wzxy;
}

float MarchCacheBilinear(float4 a_s, float2 a_f)
{
	return lerp(lerp(a_s.x, a_s.y, a_f.x), lerp(a_s.z, a_s.w, a_f.x), a_f.y);
}

// Every tap of a march from a_gridLocal along a_stepDir, out to a_reach, lies
// at least a texel inside the map's window.
bool MarchCacheCovers(float2 a_gridLocal, float2 a_stepDir, float a_reach)
{
	SNOW_DEFORM_DIMS(dims);
	const float2 margin = 1.5 / dims;
	const float2 uv0 = (GridToDeformOffset + a_gridLocal) * DeformInvWorldSize;
	const float2 uv1 = uv0 + a_stepDir * (a_reach * DeformInvWorldSize);
	return all(min(uv0, uv1) >= margin) && all(max(uv0, uv1) <= 1.0 - margin);
}

// The same for the far cache, whose texel i lies on land-layer vertex i.
bool MarchFarCovers(float2 a_gridLocal, float2 a_stepDir, float a_reach)
{
	const float2 t0 = (FineWindow.xy + a_gridLocal) / FineWindow.w;
	const float2 t1 = t0 + a_stepDir * (a_reach / FineWindow.w);
	return FineWindow.z > 0.5 && all(min(t0, t1) >= 1.0) && all(max(t0, t1) <= FineWindow.z - 2.0);
}

float MarchHeightNear(float2 a_gridLocal)
{
	float2 f;
	return MarchCacheBilinear(MarchCacheTexels(MarchHeightGround, a_gridLocal, f), f);
}

float MarchHeightFarAt(float2 a_gridLocal)
{
	const float2 t = (FineWindow.xy + a_gridLocal) / FineWindow.w;
	const float2 t0 = floor(t);
	return MarchCacheBilinear(MarchHeightFar.GatherRed(SnowSampler, (t0 + 1.0) / FineWindow.z).wzxy, t - t0);
}

// The receiver's height and the five taps', all from one cache, so the six
// gathers issue together.
void MarchCachedHeights(float2 a_gridLocal, float2 a_stepDir, bool a_near, out float a_h[6])
{
	[branch] if (a_near)
	{
		a_h[0] = MarchHeightNear(a_gridLocal);
		[unroll] for (uint i = 0; i < 5; i++)
			a_h[i + 1] = MarchHeightNear(a_gridLocal + a_stepDir * kSunMarchDist[i]);
	}
	else
	{
		a_h[0] = MarchHeightFarAt(a_gridLocal);
		[unroll] for (uint i = 0; i < 5; i++)
			a_h[i + 1] = MarchHeightFarAt(a_gridLocal + a_stepDir * kSunMarchDist[i]);
	}
}

// The march inside the trail map, against the receiver a_ref: x = the horizon
// over the exact near taps and the two far taps (the sun's tangent), y = how
// far the sun envelope stands above a_ref + 1 (units).
float2 MarchNearHorizon(float2 a_gridLocal, float2 a_stepDir, float a_sunTan, float a_ref)
{
	SNOW_DEFORM_DIMS(dims);
	const float texel = 1.0 / (DeformInvWorldSize * dims.x);
	float horizonTan = -10.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		const float d = kSunNearTaps[i] * texel;
		horizonTan = max(horizonTan, (MarchHeightNear(a_gridLocal + a_stepDir * d) - a_ref - 1.0) / d);
	}
	[unroll] for (uint j = 3; j < 5; j++)
		horizonTan = max(horizonTan, (MarchHeightNear(a_gridLocal + a_stepDir * kSunMarchDist[j]) - a_ref - 1.0) / kSunMarchDist[j]);
	const float shift = kSunEnvelopeShift * texel;
	float2 f;
	const float envelope = MarchCacheBilinear(MarchCacheTexels(MarchSunEnvelope, a_gridLocal + a_stepDir * shift, f), f) - shift * a_sunTan;
	return float2(horizonTan, envelope - a_ref - 1.0);
}

// MarchNearHorizon's light: the taps soft by a_soft in the sun's tangent, the
// envelope by a_softUnits.
float MarchNearLight(float2 a_horizon, float a_sunTan, float a_soft, float a_softUnits)
{
	return min(smoothstep(-1.5 * a_soft, 1.5 * a_soft, a_sunTan - a_horizon.x), smoothstep(-a_softUnits, a_softUnits, -a_horizon.y));
}
#endif

// Every map and berm read within 8 units of here returns exactly zero, so the
// carve, berm and surface-state terms equal their zero-input results. Just
// outside the window a tap can still reach in, so only far outside skips.
bool ShellUntouched(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	float2 pad = 8.0 * DeformInvWorldSize;
	bool farOutside = any(uv < -pad) || any(uv > 1.0 + pad);
	SNOW_DEFORM_DIMS(dims);
	int2 logical = clamp(int2(uv * dims), 0, int2(dims) - 1);
	bool zeroTile = UntouchedTiles.Load(int3(((logical + DeformMapOrigin) & (int2(dims) - 1)) >> 3, 0)) != 0;
	bool inside = all(uv >= 0.0) && all(uv <= 1.0);
	return BermBakeActive > 0.5 && (farOutside || (inside && zeroTile));
}

// Trench detail tile: 64 units on 1024 texels, its height stored as 0.5 + h / 6
// about the tile mean (the powder set reaches furthest, +2.38), one stochastic
// offset per tile cell (CS's sampler, one lattice cell per tile). Slice 0 is
// the powder set, worn by all disturbed snow in every weather; 1 the impact
// set (the tile generator's "wet"), worn where something hit the snow hard or
// fire melted it. An impact's energy (the map's .w) also raises the relief by
// up to kImpactReliefBoost.
static const float kTrenchDetailTile = 64.0;
static const float kTrenchDetailSpan = 6.0;
static const float kTrenchDetailReach = 2.75;
static const float kTrenchDetailPowder = 0.0;
static const float kTrenchDetailImpact = 1.0;
static const float kImpactReliefBoost = 0.5;

// The detail's two stochastic taps at a point: CS's sampler picks the two
// lattice cells and the dominant one's weight, and each cell turns its tile by
// an angle of its own, read from the offset CS hashed for it, about the cell's
// own corner. Normals turn back with the tile.
struct TrenchDetailTaps
{
	float4 uv;    // xy the dominant cell's tile uv, zw the second's
	float4 turn;  // (cos, sin) of each cell's turn
	float w;      // the dominant tap's weight (CS's tap1Weight)
};

float2 TrenchDetailTurn(float2 a_offset)
{
	const float a = 6.2831853 * frac(dot(a_offset, float2(17.13, 31.71)));
	return float2(cos(a), sin(a));
}

float2 TrenchDetailRotate(float2 v, float2 cs)
{
	return float2(cs.x * v.x - cs.y * v.y, cs.y * v.x + cs.x * v.y);
}

TrenchDetailTaps TrenchDetailTapsAt(float2 worldXY)
{
	const float2 uv = worldXY / kTrenchDetailTile;
	const StochasticOffsets o = ComputeStochasticOffsetsScaledPeriodic(uv, MESH_SCALE, kTrenchDetailLatticePeriod);
	TrenchDetailTaps t;
	t.turn = float4(TrenchDetailTurn(o.offset1), TrenchDetailTurn(o.offset2));
	t.uv = float4(TrenchDetailRotate(uv - StochasticCellUV(o.cell1, MESH_SCALE), t.turn.xy) + o.offset1,
		TrenchDetailRotate(uv - StochasticCellUV(o.cell2, MESH_SCALE), t.turn.zw) + o.offset2);
	t.w = o.tap1Weight;
	return t;
}

// Both taps' uv change for a world step a_d.
float4 TrenchDetailStep(TrenchDetailTaps a_t, float2 a_d)
{
	return float4(TrenchDetailRotate(a_d, a_t.turn.xy), TrenchDetailRotate(a_d, a_t.turn.zw)) / kTrenchDetailTile;
}

// ComputeTerrainStochasticLodBase's rule for world-unit pixel derivatives.
float TrenchDetailLod(float2 a_dx, float2 a_dy)
{
	return max(0.5 * log2(max(min(dot(a_dx, a_dx), dot(a_dy, a_dy)), 1e-12)) + log2(1024.0 / kTrenchDetailTile) + SharedData::MipBias, 0.0);
}

// Where the detail lies across a trench. The measure holds 0.85 on the floor,
// the carve's short blur across the rim and the berm field over the apron
// (a_bermPair: x berm, y short blur), so it falls to 0 with no plateau. The
// cut moves from the floor (a_coverage 0) to just past the rim (1) and out
// over the berm (2); its band and the tail gate keep the fade soft wherever
// it lands. Untouched snow is 0.
float TrenchDetailMask(float a_carve, float2 a_bermPair, float a_coverage)
{
	const float f = max(0.85 * a_carve, max(a_bermPair.y, 0.8 * a_bermPair.x));
	const float cut = 0.85 - 0.325 * a_coverage;
	return smoothstep(cut - 0.4, cut, f) * smoothstep(0.0, 0.15, f);
}

// Speed Spoil: the trench detail over snow a fast foot threw past the trench,
// where its impact energy lies. Undated there, so it reads fresh. Past the
// run's own body ring (a sprint's 0.2), which belongs in the trench.
float TrenchSpoilMask(float a_energy)
{
	return smoothstep(0.3, 0.45, a_energy);
}

// One tap's stored height (0.5 = the tile mean): the powder set, mixed toward
// the impact set by a_impact (something hit the snow, or fire melted it).
float TrenchDetailHeight01(float2 a_uv, float a_lod, float a_impact)
{
#if defined(SNOW_NOISE_TABLE)
	float h = SnowTrenchDetailH.SampleLevel(SnowSampler, float3(a_uv, kTrenchDetailPowder), a_lod).x;
	[branch] if (a_impact > 0.001)
		h = lerp(h, SnowTrenchDetailH.SampleLevel(SnowSampler, float3(a_uv, kTrenchDetailImpact), a_lod).x, a_impact);
	return h;
#else
	return 0.5;
#endif
}

// The detail's height in units at both taps' uvs, the taps blended as CS
// blends its stochastic taps (height-biased).
float TrenchDetailHeight(float4 a_uv, float a_w, float a_lod, float a_impact)
{
	const float h1 = TrenchDetailHeight01(a_uv.xy, a_lod, a_impact);
	const float h2 = TrenchDetailHeight01(a_uv.zw, a_lod, a_impact);
	return (StochasticBlendTwoSamples(h1.xxxx, h2.xxxx, a_w, h1, h2).x - 0.5) * kTrenchDetailSpan;
}

// One tap's height gradient in its tile's frame, from its normals, the sets
// mixed as TrenchDetailHeight01 mixes them.
float2 TrenchDetailGrad01(float2 a_uv, float a_lod, float a_impact)
{
#if defined(SNOW_NOISE_TABLE)
	float2 n = SnowTrenchDetailN.SampleLevel(SnowSampler, float3(a_uv, kTrenchDetailPowder), a_lod) * 2.0 - 1.0;
	float2 g = -n / max(sqrt(saturate(1.0 - dot(n, n))), 0.25);
	[branch] if (a_impact > 0.001)
	{
		n = SnowTrenchDetailN.SampleLevel(SnowSampler, float3(a_uv, kTrenchDetailImpact), a_lod) * 2.0 - 1.0;
		g = lerp(g, -n / max(sqrt(saturate(1.0 - dot(n, n))), 0.25), a_impact);
	}
	return g;
#else
	return 0.0;
#endif
}

// The detail's height gradient in world space: each tap's turned back out of
// its tile's frame, the two blended with their heights' weight.
float2 TrenchDetailGrad(TrenchDetailTaps a_t, float a_lod, float a_impact)
{
	const float h1 = TrenchDetailHeight01(a_t.uv.xy, a_lod, a_impact);
	const float h2 = TrenchDetailHeight01(a_t.uv.zw, a_lod, a_impact);
	const float2 g1 = TrenchDetailRotate(TrenchDetailGrad01(a_t.uv.xy, a_lod, a_impact), a_t.turn.xy * float2(1.0, -1.0));
	const float2 g2 = TrenchDetailRotate(TrenchDetailGrad01(a_t.uv.zw, a_lod, a_impact), a_t.turn.zw * float2(1.0, -1.0));
	return StochasticBlendTwoSamples(float4(g1, 0.0, 0.0), float4(g2, 0.0, 0.0), a_t.w, h1, h2).xy;
}

// Distance along the unit view ray (dir) from the sheet point to where it first
// meets the detail heights laid over the pixel's tangent plane (slope g): a
// linear search from +reach to -reach around the plane, its start dithered by
// a_noise, refined by secant. a_t = the taps at the sheet point, a_duv their
// uvs per unit t; a_detail = (relief, mip, horizontal step in units); a_impact
// as TrenchDetailHeight01's.
float TrenchDetailParallaxT(float3 dir, float2 g, TrenchDetailTaps a_t, float4 a_duv, float3 a_detail, float a_impact, float a_noise)
{
	const float rise = dir.z - dot(dir.xy, g);
	const float hMax = a_detail.x * kTrenchDetailReach;
	// Single exit: a return inside [branch] trips X4000.
	float t = 0.0;
	[branch] if (rise <= -0.02)
	{
		const float t0 = hMax / rise;
		const float span = -2.0 * hMax / rise;
		const float steps = clamp(ceil(span * length(dir.xy) / a_detail.z), 6.0, 40.0);
		const float dt = span / steps;
		float prevT = t0;
		float prevGap = hMax - a_detail.x * TrenchDetailHeight(a_t.uv + a_duv * t0, a_t.w, a_detail.y, a_impact);
		t = t0 + span;
		[loop] for (float i = 0.0; i < steps; i += 1.0)
		{
			const float ti = t0 + dt * (i + a_noise);
			const float gap = rise * ti - a_detail.x * TrenchDetailHeight(a_t.uv + a_duv * ti, a_t.w, a_detail.y, a_impact);
			[branch] if (gap <= 0.0)
			{
				t = lerp(prevT, ti, prevGap / max(prevGap - gap, 1e-6));
				break;
			}
			prevT = ti;
			prevGap = gap;
		}
	}
	return t;
}

// Sun visibility through the detail heights from a point on them (taps a_t),
// a_h over the tangent plane of slope g; a_detail = (relief, mip, strength),
// a_impact as TrenchDetailHeight01's. Ten taps along the ray toward the sun
// until it clears the tallest detail (32 units at most), each tap's overshoot
// weighed against its distance for a soft edge, the worst scaled by strength
// as the snow texture's parallax shadow is (Parallax Shadow). 1 = lit.
float TrenchDetailSunShadow(TrenchDetailTaps a_t, float a_h, float3 L, float2 g, float3 a_detail, float a_impact, float a_noise)
{
	const float hMax = a_detail.x * kTrenchDetailReach;
	const float climb = L.z - dot(L.xy, g);
	float lit = 1.0;
	[branch] if (climb > 0.01 && a_h < hMax)
	{
		const float reach = min((hMax - a_h) / climb, 32.0);
		const float4 duv = TrenchDetailStep(a_t, L.xy);
		float occlusion = 0.0;
		[loop] for (float i = 0.0; i < 10.0; i += 1.0)
		{
			const float d = reach * (i + 0.1 + 0.9 * a_noise) / 10.0;
			const float over = a_detail.x * TrenchDetailHeight(a_t.uv + duv * d, a_t.w, a_detail.y, a_impact) - (a_h + climb * d);
			occlusion = max(occlusion, over / (0.2 * d + 0.05));
		}
		lit = 1.0 - saturate(occlusion * a_detail.z);
	}
	return lit;
}

// ---- Deformation surface-state readers (spell marks). One bilinear tap;
// channel meanings in DeformationUpdateCS.hlsl. ----

// Melted depth at a point: the positive half of the surface-state channel.
// The shadow caster reads it to keep melt pits from casting.
float SampleMelted(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	return saturate(v.y);
}

// The trench detail's marks at a point, one bilinear tap: x = melted, y = the
// impact energy (.w); both wear the impact set.
float2 SampleDetailMarks(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	return float2(saturate(v.y), saturate(v.w));
}

// Scorch at a point: burnt snow left by a shock discharge, the negative half
// of the same channel.
float SampleScorch(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	return saturate(-v.y);
}

// A fire or shock mark's weight (either half of .y). Magnitudes per tap, so a
// melt beside a scorch cannot cancel.
static const float kSpellMarkStart = 0.02;
float SampleSpellMark(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float m00 = abs(DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).y);
	float m10 = abs(DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).y);
	float m01 = abs(DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).y);
	float m11 = abs(DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).y);
	return smoothstep(kSpellMarkStart, 0.15, lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y));
}

// The deepest a body has reached into the layer at each map texel (StampCS),
// 0 where none has since the ground arrived. Physical layout, like the map.
Texture2D<float> BodyReachMax : register(t56);

// Trampled ground: snow walked deeper than one pass is packed, and the
// detail's relief on its floor scales toward TrampleStyle.y (1 = off) by how
// far. One pass leaves TrampleStyle.z of the deepest reach bodies have had at
// a texel, which holds for prints narrower than a texel; ground with none on
// record measures against the whole layer. Shallow reach is the trench's
// edge, and the berm outside it keeps its own. Melted depth is not packing.
float TrampleDetailScale(float2 worldXY, float deformation)
{
	float scale = 1.0;
	const float2 uv = (GridToDeformOffset + worldXY - GridOrigin) * DeformInvWorldSize;
	[branch] if (TrampleStyle.y < 0.999 && deformation > 0.001 && all(uv >= 0.0) && all(uv <= 1.0))
	{
		SNOW_DEFORM_DIMS(dims);
		const float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
		const int2 t0 = (int2)t;
		const float2 f = t - t0;
		const int2 t1 = min(t0 + 1, int2(dims) - 1);
		float4 packed;
		[unroll] for (int i = 0; i < 4; i++)
		{
			const int3 texel = DeformTexel(int2((i & 1) ? t1.x : t0.x, (i & 2) ? t1.y : t0.y), int2(dims));
			const float2 carve = saturate(DeformationMap.Load(texel).xy);
			const float reach = BodyReachMax.Load(texel);
			const float layer = reach > 0.0 ? reach : 1.0;
			const float pressed = max(carve.x - carve.y, 0.0) / layer;
			packed[i] = saturate((pressed - TrampleStyle.z) / max(1.0 - TrampleStyle.z, 1e-3)) * smoothstep(0.05, 0.5, layer);
		}
		scale = lerp(1.0, saturate(TrampleStyle.y), lerp(lerp(packed.x, packed.y, f.x), lerp(packed.z, packed.w, f.x), f.y));
	}
	return scale;
}

// ---- Snow Climate per pixel (TRACK-VARIATION-RESEARCH.md 3.3) ----
// The CPU's climate rows lerped by the point's temperature offset from the
// player's (height x lapse, clamped at the outer rows' 2 C). Fixed indices:
// a per-pixel index into the constant buffer serialises the warp's reads.
float4 SnowClimateRow(float worldZ)
{
	const float dT = clamp(SnowClimate.y * (SnowClimate.z - worldZ), -2.0, 2.0);
	return lerp(SnowClimateLUT[1], dT < 0.0 ? SnowClimateLUT[0] : SnowClimateLUT[2], abs(dT) * 0.5);
}

// SnowClimateRow's lerp of one value per row (xyz).
float SnowClimateRowOf(float3 a_rows, float worldZ)
{
	const float dT = clamp(SnowClimate.y * (SnowClimate.z - worldZ), -2.0, 2.0);
	return lerp(a_rows.y, dT < 0.0 ? a_rows.x : a_rows.z, abs(dT) * 0.5);
}

#ifdef PSHADER
// Thin-snow print: a boot through a dusting (thin classes) has no wall to
// light, so it reads by material - pressed wet, darker and smoother. Gone by
// ~6 units of cover, where relief takes over. Not on melted floors: a walked
// fire ring went dark as a whole. Wet snow (Snow Climate) presses darker and
// glossier at any depth; the warmest row says whether any snow in view is
// wet. Under Snow Climate the thin print is the wet look too, so it follows
// the wetness. An old print is under new snow and lighter: a_dated reads its
// age at a_gridLocal (the baked carve date), inside the branch, so the date
// is read only where a print darkens.
// Wet Snow and Ice (the rows' y and x): wet snow darkens by Community
// Shaders' wetness rule (Lighting.hlsl, snow fully porous) and stops
// sparkling; pressed snow glazes by the meltwater that froze in it after it
// was cut (SnowIceSince), and the rest of the surface takes kIceSurfaceGlaze
// of the rows' ice (the sun crust a thaw and a freeze leave). Mirror kIceTint,
// kIceRoughCut and kIceSurfaceGlaze in SnowLighting.hlsli. Darkens a_albedo;
// returns x the roughness cut, y the share of glints left.
static const float3 kIceTint = float3(0.93, 0.955, 0.985);
static const float kIceRoughCut = 0.8;
static const float kIceSurfaceGlaze = 0.1;
// Glazed snow's Parallax Depth and Parallax Shadow, as a share of the sliders.
static const float kIceParallax = 0.5;

float SnowPressed(float a_carve, float a_melt)
{
	return smoothstep(0.1, 0.5, a_carve) * (1.0 - smoothstep(0.1, 0.4, a_melt));
}

// Any row iced: the CPU puts the most ice of the rows in every z.
bool SnowIceInView()
{
	return SnowClimateLUT[0].z > 0.001;
}

// The ice that froze in a track after it was cut, of a_rowIce (the point's
// row's x): the date is the carve's (BermFieldBaked), the ice then
// SnowIce::AtCut's. A cut in frozen snow breaks the crust and holds no water
// to glaze until the next thaw and freeze. No date (a print on an object, no
// bake) reads as just cut.
float SnowIceSince(float a_rowIce, float2 a_gridLocal, bool a_dated, float a_worldZ)
{
	float since = 0.0;
	[branch] if (a_dated && BermBakeActive > 0.5)
		since = max(a_rowIce - SnowClimateRowOf(SnowIce::AtCut(BermFieldBaked(a_gridLocal).z), a_worldZ), 0.0);
	return since;
}

// The pressed snow's glaze, which softens the parallax.
float SnowIceGlaze(float a_carve, float a_melt, float a_worldZ, float2 a_gridLocal, bool a_dated)
{
	const float pressed = SnowPressed(a_carve, a_melt);
	float glaze = 0.0;
	[branch] if (pressed > 0.001)
		glaze = SnowIceSince(SnowClimateRow(a_worldZ).x, a_gridLocal, a_dated, a_worldZ) * pressed;
	return glaze;
}

// The glaze SnowWetPrint draws: the pressed snow's, and the surface crust.
float SnowIceLook(float a_carve, float a_melt, float a_worldZ, float2 a_gridLocal, bool a_dated)
{
	return max(SnowIceGlaze(a_carve, a_melt, a_worldZ, a_gridLocal, a_dated), kIceSurfaceGlaze * SnowClimateRow(a_worldZ).x);
}

// The sliders' share on glazed snow: 1 off the ice, kIceParallax on it.
float SnowIceParallax(float a_carve, float a_melt, float a_worldZ, float2 a_gridLocal, bool a_dated)
{
	float share = 1.0;
	[branch] if (SnowIceInView())
		share = lerp(1.0, kIceParallax, SnowIceGlaze(a_carve, a_melt, a_worldZ, a_gridLocal, a_dated));
	return share;
}

float2 SnowWetPrint(inout float3 a_albedo, float a_carve, float a_melt, float a_depth, float a_worldZ, float2 a_gridLocal, bool a_dated)
{
	float2 look = float2(0.0, 1.0);
	const float wetPress = SnowPressed(a_carve, a_melt);
	const bool wetIce = SnowClimateLUT[2].y > 0.001 || SnowIceInView();
	[branch] if (wetIce || (wetPress > 0.001 && (a_depth < 6.0 || SnowClimateLUT[2].w > 0.001)))
	{
		const float4 row = SnowClimateRow(a_worldZ);
		a_albedo = lerp(a_albedo, pow(abs(a_albedo), 1.0 + row.y), 0.5);
		float pressGlaze = 0.0;
		[branch] if (wetPress > 0.001 && row.x > 0.001)
			pressGlaze = SnowIceSince(row.x, a_gridLocal, a_dated, a_worldZ) * wetPress;
		const float glaze = max(pressGlaze, kIceSurfaceGlaze * row.x);
		look.y = (1.0 - row.y) * (1.0 - glaze);
		look.x = glaze * kIceRoughCut;
		a_albedo *= lerp(1.0, kIceTint, glaze);
		[branch] if (wetPress > 0.001)
		{
			const float climateWet = row.w;
			const float thinPrint = (1.0 - smoothstep(1.5, 6.0, a_depth)) * saturate(SnowClimate2.w + climateWet);
			float wetPrint = wetPress * max(thinPrint, 0.8 * climateWet);
			[branch] if (a_dated && TrackAge.y > 0.0 && BermBakeActive > 0.5)
				wetPrint *= saturate(1.0 - 2.0 * TrackAgeOf(BermFieldBaked(a_gridLocal).z));
			a_albedo = lerp(a_albedo, a_albedo * float3(0.76, 0.77, 0.80) * (1.0 - 0.25 * climateWet), wetPrint);
			look.x = max(wetPrint * (0.24 + 0.26 * climateWet), look.x);
		}
	}
	return look;
}
#endif

#if defined(SNOW_CONDITION_VIEW) || defined(SNOW_CONDITION_VIEW_RUNTIME)
// Debugging Options > Snow Condition. Mirrors SnowDeformation::GetSnowCondition
// (Accumulation.cpp): the player's values shifted by height. xyzw =
// temperature (C), wetness, stickiness, firmness.
float4 SnowConditionAt(float a_worldZ)
{
	const float playerT = SnowClimate.x;
	const float t = playerT - SnowClimate.y * (a_worldZ - SnowClimate.z);
	const float fresh = SnowClimate.w;
	const float wind = SnowClimate2.x;
	const float wet = SnowClimateOverride.x >= 0.0 ? saturate(SnowClimateOverride.x) :
	                                                 saturate(SnowClimate2.y + smoothstep(-1.5, 1.5, t) - smoothstep(-1.5, 1.5, playerT));
	const float freshDensity = t <= 0.0 ? 67.9 + 51.3 * exp(t / 2.6) : 125.0 + 40.0 * min(t, 3.0);
	const float density = clamp(lerp(freshDensity, 290.0, 1.0 - fresh) + 110.0 * wet + 150.0 * wind, 50.0, 560.0);
	const float sticky = SnowClimateOverride.y >= 0.0 ? saturate(SnowClimateOverride.y) :
	                                                    saturate(0.1 + 0.62 * smoothstep(-20.0, -1.5, t) * (0.3 + 0.7 * (1.0 - fresh)) +
																 0.45 * wet * (1.0 - smoothstep(0.75, 1.0, wet)) + 0.25 * wind);
	const float firm = SnowClimateOverride.z >= 0.0 ? saturate(SnowClimateOverride.z) : smoothstep(150.0, 460.0, density);
	return float4(t, wet, sticky, firm);
}

// 1 temperature (blue below freezing, white at it, orange above, a line
// every 2 C), 2 wetness, 3 stickiness, 4 firmness, 5 all three as RGB, 6
// track age (a_track: x the age, y how much trench is there; green just cut,
// red aged, untouched snow white), 7 ice (a_track.x: the glaze, cyan).
float3 SnowConditionColor(float4 a_condition, float a_view, float2 a_track)
{
	const float t = a_condition.x;
	const float band = t / 2.0;
	const float bandWidth = fwidth(band);
	float3 color;
	[branch] if (a_view < 1.5)
	{
		const float3 cold = lerp(float3(1.0, 1.0, 1.0), float3(0.05, 0.25, 1.0), saturate(-t / 20.0));
		const float3 warm = lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.25, 0.0), saturate(t / 4.0));
		const float isotherm = 1.0 - smoothstep(0.0, 1.5 * bandWidth, abs(frac(band + 0.5) - 0.5));
		color = (t < 0.0 ? cold : warm) * (1.0 - 0.6 * isotherm);
	}
	else if (a_view < 2.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(0.1, 0.35, 1.0), a_condition.y);
	else if (a_view < 3.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(1.0, 0.45, 0.05), a_condition.z);
	else if (a_view < 4.5)
		color = lerp(float3(0.95, 0.95, 0.95), float3(0.55, 0.1, 0.85), a_condition.w);
	else if (a_view < 5.5)
		color = a_condition.yzw;
	else if (a_view < 6.5)
		color = lerp(float3(0.95, 0.95, 0.95), lerp(float3(0.15, 0.85, 0.2), float3(0.75, 0.2, 0.05), a_track.x), a_track.y);
	else
		color = lerp(float3(0.95, 0.95, 0.95), float3(0.05, 0.75, 1.0), a_track.x);
	return color;
}
#endif

// Blood at a point (BloodMap t30 / BloodClock t31, the deformation map's
// torus): rgb = pigment (linear), a = concentration after burial. Each texel
// is dated by the snowfall clock and the game hour it was deposited; the
// fade is computed per tap and the faded taps are interpolated, so a never-
// written neighbour (clock 0) reads as nothing rather than as ancient blood.
// fresh: 1 just spilled, 0 once BloodLook2.x hours have passed.
float4 SampleBlood(float2 gridLocal, out float fresh)
{
	// No early return inside a [branch]: fxc reads it as an uninitialised
	// out (X4000), the trap CLAUDE.md records.
	fresh = 0.0;
	float4 acc = 0.0;
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	const bool live = BloodLook2.z > 0.5 && all(uv >= 0.0) && all(uv <= 1.0);
	[branch] if (live)
	{
		float2 dims;
		BloodMap.GetDimensions(dims.x, dims.y);
		float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
		int2 t0 = (int2)t;
		float2 f = t - t0;
		int2 t1 = min(t0 + 1, int2(dims) - 1);

		const int2 taps[4] = { int2(t0.x, t0.y), int2(t1.x, t0.y), int2(t0.x, t1.y), int2(t1.x, t1.y) };
		const float weights[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
		float freshAcc = 0.0;
		[unroll] for (int i = 0; i < 4; i++)
		{
			float4 b = BloodMap.Load(DeformTexel(taps[i], int2(dims)));
			[branch] if (b.a > 0.002)
			{
				float2 clock = BloodClock.Load(DeformTexel(taps[i], int2(dims)));
				float buried = saturate((BloodLook.y - clock.x) / BloodLook.w);
				float age = saturate((BloodLook.z - clock.y) / BloodLook2.x);
				float conc = b.a * (1.0 - buried) * weights[i];
				acc += float4(b.rgb * conc, conc);
				freshAcc += (1.0 - age) * conc;
			}
		}
		[branch] if (acc.a > 1e-4)
		{
			acc.rgb /= acc.a;
			fresh = freshAcc / acc.a;
		}
	}
	return acc;
}

// Wide exclusion field, bilinear: x = door suppression, y = melt. Returns 0
// outside the window (nothing claimed where nothing was baked). The
// landscape unions this with its near object-bottoms mask; the skin reads
// it alone.
float2 SampleExclusionField(float2 worldXY)
{
	float2 result = 0.0;
	[branch] if (ExclusionFieldWindow.w > 0.5)
	{
		float2 local = (worldXY - ExclusionFieldWindow.xy) * ExclusionFieldWindow.z;
		[branch] if (all(abs(local) < 0.995))
		{
			SNOW_EXCLUSION_DIMS(dims);
			float2 uv = local * 0.5 + 0.5;
			float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
			int2 t0 = (int2)t;
			float2 f = t - t0;
			int2 t1 = min(t0 + 1, int2(dims) - 1);

			float2 s00 = ExclusionFieldMap.Load(int3(t0.x, t0.y, 0));
			float2 s10 = ExclusionFieldMap.Load(int3(t1.x, t0.y, 0));
			float2 s01 = ExclusionFieldMap.Load(int3(t0.x, t1.y, 0));
			float2 s11 = ExclusionFieldMap.Load(int3(t1.x, t1.y, 0));

			result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
		}
	}
	return result;
}

// ---- Shared field surface pieces: the carve profile, the dune undulation
// and the melt floor, seen identically by geometry, shading and both
// shells' self-shadow marches. ----

// Melted fire basins keep this much snow above the terrain: the floor stays
// shell snow, never bare ground, never below the terrain mesh.
static const float kFireMeltFloor = 1.0;

float Undulation(float2 worldXY)
{
	return UndulationNorm(worldXY, UndulationScale) * UndulationAmp;
}

// ---- Baked undulation (UndulationFieldCS) ----
// Amp-free height (x) and its +-kUndulationGradStep gradient (yz), both
// multiplied by UndulationAmp on read so the Strength slider needs no
// rebake, in a camera-snapped world window: UndulationFieldWindow = (centre XY,
// 1/half-extent, bake live > 0.5). Same manual-bilinear convention as
// BermFieldBaked, so every stage reads it without a sampler. The live
// Undulation() path stays compiled underneath: the fallback for taps
// outside the window (distant statics), and the debug A/B.

float3 UndulationBakedHG(float2 worldXY)
{
	float2 uv = (worldXY - UndulationFieldWindow.xy) * UndulationFieldWindow.z * 0.5 + 0.5;
	float2 dims;
	UndulationFieldMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	float3 s00 = UndulationFieldMap.Load(int3(t0, 0)).xyz;
	float3 s10 = UndulationFieldMap.Load(int3(t0 + int2(1, 0), 0)).xyz;
	float3 s01 = UndulationFieldMap.Load(int3(t0 + int2(0, 1), 0)).xyz;
	float3 s11 = UndulationFieldMap.Load(int3(t0 + int2(1, 1), 0)).xyz;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

bool UndulationBakedCovers(float2 worldXY)
{
	return UndulationFieldWindow.w > 0.5 &&
	       max(abs(worldXY.x - UndulationFieldWindow.x), abs(worldXY.y - UndulationFieldWindow.y)) * UndulationFieldWindow.z < 0.999;
}

// Drop-in for Undulation(): one bilinear read instead of two octaves.
// Single-exit like BermField - a return inside [branch] trips X4000.
float UndulationSampled(float2 worldXY)
{
	float result;
	[branch] if (UndulationBakedCovers(worldXY))
		result = UndulationBakedHG(worldXY).x * UndulationAmp;
	else
		result = Undulation(worldXY);
	return result;
}

// Drop-in for the shading blocks' four-tap central difference (kUndulationGradStep,
// the operator the bake stored).
float2 UndulationGradSampled(float2 worldXY)
{
	float2 result;
	[branch] if (UndulationBakedCovers(worldXY))
		result = UndulationBakedHG(worldXY).yz * UndulationAmp;
	else {
		const float uStep = kUndulationGradStep;
		result = float2(
					 Undulation(worldXY + float2(uStep, 0.0)) - Undulation(worldXY - float2(uStep, 0.0)),
					 Undulation(worldXY + float2(0.0, uStep)) - Undulation(worldXY - float2(0.0, uStep))) /
		         (2.0 * uStep);
	}
	return result;
}

// Deformation carves the layer to the ground.
//
// P7 (trench plan Stage 3): DEPTH PICKS THE PROFILE. The map's carve
// gradient is depth-blind, so a bootprint in 5 units of cover wore the same
// normalized cliff walls as a knee-deep trench, miniaturized. The
// smootherstep remap flattens the response at rim and floor - the rim rolls
// over, the floor entry rounds, and a shallow print reads as a soft dimple -
// while deep snow keeps the map-authored cut untouched. Living HERE rather
// than in the stamp falloff (where the plan first pointed) because the map
// update has no depth data and every consumer of the shape - geometry,
// finite-difference normals, both shells, the self-shadow march - already
// routes through this one function.
// P5 (trench plan Stage 3) rides in here too, for the same reason P7 does:
// one function, every consumer agrees. Teeth first, then the depth remap,
// then the lip on the notched value so the teeth break the lip into blocks
// (RDR2 O1: "irregular teeth and broken blocks" at the rim).
// Slough (Snow Climate) comes before all of them: loose snow falls back into
// the print, toward the carve's short blur (deformBlur, the berm field's .y)
// but never past the print, so the hole keeps its outline and its floor
// rounds and narrows.
// Age (a_look, TrackCarveLook of the berm field's .z date) then rounds it
// toward the blur itself, shallower and wider, as snowfall and settling round
// a print, and takes the teeth and the lip off the rim first.
float CarveSlough(float deformation, float deformBlur, float a_round)
{
	float d = saturate(lerp(deformation, min(deformation, deformBlur), CompactLook.w));
	return lerp(d, deformBlur, a_round);
}

float CarveProfile(float deformation, float deformBlur, float uncarvedDepth, float2 worldXY, float3 a_look)
{
	float depthT = smoothstep(6.0, 18.0, uncarvedDepth);
	float d = CarveSlough(deformation, deformBlur, a_look.x);

	// P5 teeth: the border work's own two-octave recipe (37-unit wander +
	// 8-unit raggedness, HEIGHT-BLEND-PLAN - reused, not reinvented),
	// applied to the carve value inside the rim band only.
	// The contour breaks into teeth; floors (d high) and open snow (d = 0)
	// sit outside the band and never move. Faded by depthT: teeth are
	// cut-wall vocabulary, dimples stay smooth.
	[branch] if (RimStyle.y > 0.001)
	{
		float rimBand = smoothstep(0.02, 0.12, d) * (1.0 - smoothstep(0.30, 0.55, d));
		// Outside the band the notch adds a signed zero.
		[branch] if (rimBand > 0.0 && depthT > 0.0)
		{
			float notch = (SnowDetailNoise(worldXY / 37.0) - 0.5) * 0.8 +
			              (SnowDetailNoise(worldXY / 8.0) - 0.5) * 1.2;
			d = saturate(d + notch * a_look.y * 0.35 * rimBand * depthT);
		}
	}

	float soft = d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
	d = lerp(soft, d, depthT);
	float profile = uncarvedDepth * (1.0 - d);

	// P5 lip: the rim rolls UP before it drops - a small cornice bulge on
	// the carve skirt, peaking at d ~ 0.1 and gone by mid-wall. Berm
	// guardrail (the plan's double-ridge warning): at the default 0.10 the
	// lip is ~1/4 berm height and sits on the berm's INNER flank, so it
	// reads as the rim rolling into the berm rather than a second ridge -
	// RimStyle.x is the dial if it ever stacks. Faded by depthT with the
	// rest of the cut-wall vocabulary.
	[branch] if (RimStyle.x > 0.001)
	{
		float hump = smoothstep(0.02, 0.10, d) * (1.0 - smoothstep(0.10, 0.35, d));
		profile += a_look.z * uncarvedDepth * hump * depthT;
	}
	return profile;
}

// CarveProfile without the rim teeth, for the sun march's occluders. The tail
// mirrors CarveProfile's: keep the two in step.
float CarveProfileNoTeeth(float deformation, float deformBlur, float uncarvedDepth, float3 a_look)
{
	float depthT = smoothstep(6.0, 18.0, uncarvedDepth);
	float d = CarveSlough(deformation, deformBlur, a_look.x);
	float soft = d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
	d = lerp(soft, d, depthT);
	float profile = uncarvedDepth * (1.0 - d);
	[branch] if (RimStyle.x > 0.001)
	{
		float hump = smoothstep(0.02, 0.10, d) * (1.0 - smoothstep(0.10, 0.35, d));
		profile += a_look.z * uncarvedDepth * hump * depthT;
	}
	return profile;
}

// P7's berm half: spoil needs material. Below ~3 units of cover there is
// nothing to throw and the ridge vanishes; full berms only from ~12 up.
// Multiply this in wherever BermShape scales by a local depth - geometry,
// the shading gradient, the shadow-march occluder and the statics ridge
// must all agree or shape, light and shadow drift apart.
float BermDepthGate(float depth)
{
	return smoothstep(3.0, 12.0, depth);
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
// log2-space formula and near/far sharpness ramp; constants mirror EM's
// HEIGHT_MULT/HEIGHT_POWER. EM-checkbox-INDEPENDENT (the Snow
// Borders dials are the only owners of the snow border; the checkbox
// consult here silently disabled the statics rim/ground shaping and the
// DS skirt descent).
static const float kHeightBlendMult = 8.0;
static const float kHeightBlendPower = 2.0;

float SnowHeightBlendSharpness(float viewDist)
{
	float nearBlendToFar = smoothstep(1024.0 * 1024.0, 2048.0 * 2048.0, viewDist * viewDist);
	float blendFactor = sqrt(saturate(1.0 - nearBlendToFar));
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

#if defined(PSHADER) && defined(SNOW_BLOOD_TILES)
// Blood detail tiles (BloodTiles.cpp), landscape shell only. Where a tile
// lies it replaces the blood map's reading: the pigment (premultiplied, own
// mip chain), dated per clock block. The mark comes back as PAINT - the
// decal's colour over the snow by its alpha, as a rune's glyph lies on it.
// The cell and the texel come from BloodLook3.zw (the
// detail level sets them); the rest mirrors SnowDeformation.h.
Texture2D<uint> BloodTileIndex : register(t66);
Texture2DArray<float4> BloodTilePigment : register(t67);  // a slice per row of 8 tiles
Texture2D<float4> BloodTileClock : register(t69);

static const int kBloodTileDim = 512;
// A clock block is 8 texels, 4 units.
static const int kBloodTileBlockShift = 3;

// worldDx / worldDy: screen derivatives of worldXY, taken outside any branch.
void SampleBloodTiles(float2 worldXY, float2 worldDx, float2 worldDy, inout float4 blood, inout float fresh, out float4 paint)
{
	paint = 0.0;
	[branch] if (BloodLook2.w > 0.5)
	{
		const float kBloodTileCell = BloodLook3.z;
		const float kBloodTileTexel = BloodLook3.w;
		const float kBloodTileApron = 0.5 * (float(kBloodTileDim) * kBloodTileTexel - kBloodTileCell);
		const uint kBloodTilesAcross = 8u;
		const int2 cell = (int2)floor(worldXY / kBloodTileCell);
		const uint slot1 = BloodTileIndex.Load(int3(cell & 127, 0));
		[branch] if (slot1 != 0u)
		{
			const uint slot = slot1 - 1u;
			const int2 tileTexel = int2(slot % kBloodTilesAcross, slot / kBloodTilesAcross) * kBloodTileDim;
			const float2 local = (worldXY - (float2(cell) * kBloodTileCell - kBloodTileApron)) / kBloodTileTexel;
			const float atlasDim = float(kBloodTileDim) * float(kBloodTilesAcross);
			const float uvPerUnit = 1.0 / (kBloodTileTexel * atlasDim);
			// A row a slice: v and its gradient span one tile, so the texel-space
			// gradient (and the mip) is the square atlas's.
			const float2 uvScale = float2(uvPerUnit, uvPerUnit * float(kBloodTilesAcross));
			const float3 uvw = float3((float(tileTexel.x) + local.x) / atlasDim, local.y / float(kBloodTileDim), float(slot / kBloodTilesAcross));
			float4 own = BloodTilePigment.SampleGrad(ShellLinearSampler, uvw, worldDx * uvScale, worldDy * uvScale);

			const int2 texel = clamp((int2)local, 0, kBloodTileDim - 1);
			const int2 blockOrigin = tileTexel >> kBloodTileBlockShift;
			float4 clock = BloodTileClock.Load(int3(blockOrigin + (texel >> kBloodTileBlockShift), 0));

			const float ownKeep = step(1e-4, clock.y) * (1.0 - saturate((BloodLook.y - clock.x) / BloodLook.w));
			const float ownConc = own.a * ownKeep;
			blood = 0.0;
			paint = float4(own.rgb / max(own.a, 1e-3), saturate(ownConc));
			fresh = 1.0 - saturate((BloodLook.z - clock.y) / BloodLook2.x);
		}
	}
}
#endif

#if defined(PSHADER) && defined(SNOW_RUNE_GLYPHS)
// Rune glyphs and frost marks (Blood.cpp, RenderRuneCapture), for the shells
// that define SNOW_RUNE_GLYPHS: the landscape shell, the road patch and the
// object coat. Runes: a 2x2 atlas, one tile per live rune, redrawn every frame
// from the rune's own ground decal as a uv field (rg = uv, b = decal id, a =
// material alpha). Frost marks: every live one as a uv plane over the ground,
// listed per cell of a window round the player. Either way the mark itself
// comes from the decal's own textures through its uv, and is the game's
// vanilla decal laid over the snow: Lighting's albedo, glow and specular, from
// its material's own constants.
Texture2D<float4> RuneAtlas : register(t90);
Texture2D<float4> RuneDiffuse[4] : register(t80);
Texture2D<float4> RuneNormal[4] : register(t84);
// Per mark: [3i] = uv at the window's origin, texture slot + 4 x its quarter
// of the 2x2 decal atlas (4 = the whole texture), material alpha; [3i + 1] =
// uv per world unit along x, then along y; [3i + 2] = x = the lowest, y = the
// highest point of the surfaces the decal covers.
StructuredBuffer<float4> FrostPlanes : register(t117);
Texture2D<float4> FrostDiffuse[4] : register(t118);
Texture2D<float4> FrostNormal[4] : register(t122);
// Per cell of the 64 x 64 window: its first entry in FrostCellList and the count.
StructuredBuffer<uint2> FrostCells : register(t126);
StructuredBuffer<uint> FrostCellList : register(t127);
// Glow maps, per rune tile and per frost texture slot: single slots, no four
// free ones lie in a row.
Texture2D<float4> RuneGlow0 : register(t88);
Texture2D<float4> RuneGlow1 : register(t89);
Texture2D<float4> RuneGlow2 : register(t95);
Texture2D<float4> RuneGlow3 : register(t96);
Texture2D<float4> FrostGlow0 : register(t97);
Texture2D<float4> FrostGlow1 : register(t99);
Texture2D<float4> FrostGlow2 : register(t111);
Texture2D<float4> FrostGlow3 : register(t64);
cbuffer RuneCB : register(b7)
{
	// x = live tiles
	float4 RuneParams;
	// xy = tile's world min, z = 1 / world size, w = 1 where its material has
	// a glow map; tile i at (i & 1, i >> 1)
	float4 RuneRects[4];
	// rgb = the decal material's EmitColor, w = 1 with a normal map
	float4 RuneTints[4];
	// The frost window: xy = world min, z = 1 / cell size, w = 1 while it holds marks
	float4 FrostRect;
	// 1 where frost texture slot i has a normal map
	float4 FrostNormals;
	// The material's SpecularColor (0 without the Specular flag), w = its specular power
	float4 RuneSpecular[4];
	// x = the lowest, y = the highest point of the surfaces the rune's decal
	// covers, z = the material's emissive multiplier
	float4 RuneReach[4];
	// Per frost texture slot: SpecularColor and specular power as RuneSpecular,
	float4 FrostSpecular[4];
	// EmitColor, w = 1 where the material has a glow map,
	float4 FrostTints[4];
	// and the emissive multiplier.
	float4 FrostEmissiveMults;
}

struct RuneGlyph
{
	float4 colour;     // rgb = the decal's albedo, a = its alpha here
	float3 normalTS;   // its normal map
	float3 tangent;    // world direction of +u
	float3 bitangent;  // world direction of +v
	float3 emission;   // its glow, alpha included
	float4 specular;   // rgb = SpecularColor x the normal map's alpha, w = specular power
};

// A mark reaches only snow lying on a surface its decal covers: the base
// surface's absolute z against the decal's own span. kMarkAnyBase: no test.
static const float kMarkAnyBase = -1.0e7;
static const float kMarkSpanSlack = 4.0;
bool MarkReaches(float base, float2 span)
{
	return base <= kMarkAnyBase || (base >= span.x - kMarkSpanSlack && base <= span.y + kMarkSpanSlack);
}

// Lighting's glow over the material's EmitColor (Lighting.hlsl, the vanilla branch).
float3 MarkGlow(float3 emit, float3 glow)
{
	return emit * ColorManagement::TextureToWorking(glow) * SharedData::linearLightingSettings.glowmapMult;
}

float3 SampleRuneGlow(uint i, float2 uv, float2 gradX, float2 gradY)
{
	float3 glow;
	[branch] switch (i)
	{
	case 0u:
		glow = RuneGlow0.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
		break;
	case 1u:
		glow = RuneGlow1.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
		break;
	case 2u:
		glow = RuneGlow2.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
		break;
	default:
		glow = RuneGlow3.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
		break;
	}
	return glow;
}

// Every frost mark over a point, through its own uv plane, laid over one
// another as the game stacks decals, the newest on top. Each is its own
// quarter of the decal atlas, whole wherever the game clipped the decal, and
// fades into the snow over the outer part of its round. Stops once the stack
// is opaque.
RuneGlyph SampleFrostMarks(float2 worldXY, float2 worldDx, float2 worldDy, float base)
{
	RuneGlyph g;
	g.colour = 0.0;
	g.normalTS = float3(0.0, 0.0, 1.0);
	g.tangent = float3(1.0, 0.0, 0.0);
	g.bitangent = float3(0.0, 1.0, 0.0);
	g.emission = 0.0;
	g.specular = 0.0;
	[branch] if (FrostRect.w > 0.5)
	{
		const float2 rel = worldXY - FrostRect.xy;
		const int2 cell = int2(floor(rel * FrostRect.z));
		[branch] if (all(cell >= 0) && all(cell < 64))
		{
			const uint2 span = FrostCells[cell.y * 64 + cell.x];
			float4 colour = 0.0;
			float3 normalW = 0.0;
			float3 emission = 0.0;
			float4 specular = 0.0;
			[loop] for (uint j = 0; j < span.y && colour.a < 0.995; j++)
			{
				const uint mark = FrostCellList[span.x + span.y - 1u - j];
				const float4 head = FrostPlanes[mark * 3u];
				const float4 slope = FrostPlanes[mark * 3u + 1u];
				const float2 uv = head.xy + slope.xy * rel.x + slope.zw * rel.y;
				const uint quarter = (uint)head.z >> 2;
				const float2 centre = quarter < 4u ? float2(quarter & 1u, quarter >> 1u) * 0.5 + 0.25 : 0.5;
				const float r = length(uv - centre) * (quarter < 4u ? 4.0 : 2.0);
				[branch] if (head.w > 0.0 && r < 1.0 && MarkReaches(base, FrostPlanes[mark * 3u + 2u].xy))
				{
					const float2 gradX = slope.xy * worldDx.x + slope.zw * worldDx.y;
					const float2 gradY = slope.xy * worldDy.x + slope.zw * worldDy.y;
					const uint slot = (uint)head.z & 3u;
					float4 d = 0.0;
					float4 n = float4(0.5, 0.5, 1.0, 0.0);
					[branch] switch (slot)
					{
					case 0u:
						d = FrostDiffuse[0].SampleGrad(SnowSampler, uv, gradX, gradY);
						n = FrostNormal[0].SampleGrad(SnowSampler, uv, gradX, gradY);
						break;
					case 1u:
						d = FrostDiffuse[1].SampleGrad(SnowSampler, uv, gradX, gradY);
						n = FrostNormal[1].SampleGrad(SnowSampler, uv, gradX, gradY);
						break;
					case 2u:
						d = FrostDiffuse[2].SampleGrad(SnowSampler, uv, gradX, gradY);
						n = FrostNormal[2].SampleGrad(SnowSampler, uv, gradX, gradY);
						break;
					default:
						d = FrostDiffuse[3].SampleGrad(SnowSampler, uv, gradX, gradY);
						n = FrostNormal[3].SampleGrad(SnowSampler, uv, gradX, gradY);
						break;
					}
					const float a = d.a * head.w * (1.0 - smoothstep(0.6, 1.0, r)) * (1.0 - colour.a);
					float3 nw = float3(0.0, 0.0, 1.0);
					const float det = slope.x * slope.w - slope.z * slope.y;
					[branch] if (FrostNormals[slot] > 0.5 && abs(det) > 1e-12)
					{
						const float3 nts = normalize(n.xyz * 2.0 - 1.0);
						// Inverse of the plane's slopes: world xy per unit u, per unit v.
						const float3 tangent = normalize(float3(slope.w, -slope.y, 0.0) / det);
						const float3 bitangent = normalize(float3(-slope.z, slope.x, 0.0) / det);
						nw = normalize(tangent * nts.x + bitangent * nts.y + float3(0.0, 0.0, nts.z));
					}
					float3 emit = FrostTints[slot].rgb;
					[branch] if (FrostTints[slot].w > 0.5)
					{
						float3 glow;
						[branch] switch (slot)
						{
						case 0u:
							glow = FrostGlow0.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
							break;
						case 1u:
							glow = FrostGlow1.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
							break;
						case 2u:
							glow = FrostGlow2.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
							break;
						default:
							glow = FrostGlow3.SampleGrad(SnowSampler, uv, gradX, gradY).rgb;
							break;
						}
						emit = MarkGlow(emit, glow);
					}
					// Lighting's own diffuse conversion, without the
					// PBRLightingScale the shells' write tail applies; the glow
					// lights the texture's colour, as Lighting adds it to the light.
					const float3 albedo = Color::Albedo(ColorManagement::DiffuseToWorking(d.rgb));
					colour += float4(albedo / Color::PBRLightingScale, 1.0) * a;
					emission += emit * albedo * a;
					specular += float4(FrostSpecular[slot].rgb * n.a, FrostSpecular[slot].w) * a;
					normalW += nw * a;
				}
			}
			[branch] if (colour.a > 1e-4)
			{
				g.colour = float4(colour.rgb / colour.a, colour.a);
				g.normalTS = normalize(normalW);
				g.emission = emission;
				g.specular = specular / colour.a;
			}
		}
	}
	return g;
}

// worldDx / worldDy: screen derivatives of worldXY, taken outside any branch.
// base: the absolute z of the surface the snow lies on (MarkReaches).
RuneGlyph SampleRunes(float2 worldXY, float2 worldDx, float2 worldDy, float base)
{
	// The frost marks first; a rune's glyph takes the point where it is stronger.
	RuneGlyph g = SampleFrostMarks(worldXY, worldDx, worldDy, base);
	const uint count = min((uint)RuneParams.x, 4u);
	[branch] if (count != 0u)
	{
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float2 tileUV = (worldXY - RuneRects[i].xy) * RuneRects[i].z;
			[branch] if (i < count && all(tileUV > 0.0) && all(tileUV < 1.0) && MarkReaches(base, RuneReach[i].xy))
			{
				// The decal's uv at the nearest texel, carried to this point along
				// its own plane from a neighbour of the same decal: never filtered
				// across two decals, nor across a decal's edge. In from the tile's
				// edge: the neighbour tile is another site.
				const float2 f = tileUV * 512.0;
				const int2 k = clamp(int2(f), 1, 510);
				const int2 at = int2(i & 1u, i >> 1u) * 512 + k;
				const float4 c = RuneAtlas.Load(int3(at, 0));
				[branch] if (c.b > 0.0)
				{
					const float4 px = RuneAtlas.Load(int3(at + int2(1, 0), 0));
					const float4 nx = RuneAtlas.Load(int3(at - int2(1, 0), 0));
					const float4 py = RuneAtlas.Load(int3(at + int2(0, 1), 0));
					const float4 ny = RuneAtlas.Load(int3(at - int2(0, 1), 0));
					const float2 stepX = px.b == c.b ? px.rg - c.rg : (nx.b == c.b ? c.rg - nx.rg : 0.0);
					const float2 stepY = py.b == c.b ? py.rg - c.rg : (ny.b == c.b ? c.rg - ny.rg : 0.0);
					const float2 uv = c.rg + stepX * (f.x - float(k.x) - 0.5) + stepY * (f.y - float(k.y) - 0.5);
					// The uv field is planar, so its world derivative is one
					// texel's difference, and the screen gradient follows from it.
					float texelWorld = 1.0 / (RuneRects[i].z * 512.0);
					float2 uvDx = stepX / texelWorld;
					float2 uvDy = stepY / texelWorld;
					float2 gradX = uvDx * worldDx.x + uvDy * worldDx.y;
					float2 gradY = uvDx * worldDy.x + uvDy * worldDy.y;
					float4 d = RuneDiffuse[i].SampleGrad(SnowSampler, uv, gradX, gradY);
					float a = d.a * c.a;
					[branch] if (a > g.colour.a)
					{
						const float3 albedo = Color::Albedo(ColorManagement::DiffuseToWorking(d.rgb));
						g.colour = float4(albedo / Color::PBRLightingScale, a);
						float3 emit = RuneTints[i].rgb;
						[branch] if (RuneRects[i].w > 0.5)
							emit = MarkGlow(emit, SampleRuneGlow(i, uv, gradX, gradY));
						g.emission = emit * albedo * a;
						g.normalTS = float3(0.0, 0.0, 1.0);
						g.specular = float4(0.0, 0.0, 0.0, RuneSpecular[i].w);
						g.tangent = float3(1.0, 0.0, 0.0);
						g.bitangent = float3(0.0, 1.0, 0.0);
						float det = uvDx.x * uvDy.y - uvDy.x * uvDx.y;
						[branch] if (RuneTints[i].w > 0.5 && abs(det) > 1e-12)
						{
							float4 n = RuneNormal[i].SampleGrad(SnowSampler, uv, gradX, gradY);
							g.normalTS = normalize(n.xyz * 2.0 - 1.0);
							g.specular.rgb = RuneSpecular[i].rgb * n.a;
							// Inverse of [uvDx uvDy]: world xy per unit u, per unit v.
							g.tangent = normalize(float3(uvDy.y, -uvDx.y, 0.0) / det);
							g.bitangent = normalize(float3(-uvDy.x, uvDx.x, 0.0) / det);
						}
					}
				}
			}
		}
	}
	return g;
}
#endif
