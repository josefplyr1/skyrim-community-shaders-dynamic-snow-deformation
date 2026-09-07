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
// UndulationAmp/Scale/Bumps, UndulationFieldWindow, BorderStyle), DeformationMap
// (t1) with the shell's DeformTexel torus helper, BermFieldMap
// (t14), ExclusionFieldMap (t15), the frost patterns (t16/t17),
// UndulationFieldMap (t29), SnowSampler
// (PS) and the TerrainVariation include - all declared before this include.
// Deliberately NOT shared: the deformation .x samplers (the landscape
// smooths bicubic, statics stays bilinear by cost) and BermFieldTapped,
// which rides each shell's own sampler.

// ShapeNoiseHash / ShapeNoise / UndulationNorm live in SnowNoise.hlsli so
// the undulation bake CS shares the exact functions without the ShellCB
// context. Callers here are unchanged.
#include "SnowDeformation/SnowNoise.hlsli"

// Two-octave domain warp for snow boundaries: a capped 37-unit coarse wander
// plus a fine 8-unit octave for raggedness. Moves WHERE a border falls without
// touching what is sampled there, so a boundary stops following the data
// lattice and reads as an organic edge instead of a staircase.
//
// The trench patch warps its silhouette with a scaled-down version of this, an
// object footprint being a hard geometric edge rather than a soft data one.
//
// SnowShell.hlsl's SampleTerrainShaped holds the SAME expression inline and
// deliberately still does: routing it through this call changes 5 of its 14
// permutations (shadow VS and four PS variants), which the DXBC harness cannot
// certify as a no-op. Same call as WarpAxis - see the note there. Edit the two
// together; do not "tidy" one away without the runtime A/B harness.
float2 BorderJitter(float2 worldXY)
{
	float coarseAmp = min(BorderNoise, 16.0);
	return float2(
			   ShapeNoise(worldXY / 37.0) - 0.5,
			   ShapeNoise(worldXY / 37.0 + 111.7) - 0.5) *
	           (2.0 * coarseAmp) +
	       float2(
			   ShapeNoise(worldXY / 8.0) - 0.5,
			   ShapeNoise(worldXY / 8.0 + 57.3) - 0.5) *
	           (1.2 * BorderNoise);
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
// map it was baked from (BermFieldCS writes texel-for-texel, same toroidal
// layout and MapOrigin - so partial rebuilds survive scrolls).
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

	float s00 = BermFieldMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float s10 = BermFieldMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float s01 = BermFieldMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float s11 = BermFieldMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));

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

// P6 clod cells: ~2.8x the default churn rubble (24/10-unit cells at
// scale 1.5), so berm clods and trench rubble read at visibly different
// frequencies - the two-scale contrast of RDR2's O3.
static const float kClodSizeScale = 1.5;

float ChurnWeight(float deformation, float bermDeform)
{
	return max(smoothstep(0.05, 0.5, deformation), BermShape(bermDeform));
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

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
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

// Scorch at a point: burnt snow left by a shock discharge, the negative half
// of the same channel.
float SampleScorch(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
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

// Crust at a point: refrozen snow, the map's third channel.
// Scorch and crust read the SAME quad of the SAME texture at the same place -
// one reads the negative part of .y, the other .z - so the shells fetch once
// and split. Same texels, same weights, same order of operations as the two
// helpers below, which are kept for any caller that needs only one.
void SampleScorchCrust(float2 gridLocal, out float scorch, out float crust)
{
	scorch = 0.0;
	crust = 0.0;
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));

	// Interpolate the quad ONCE, then split. Both originals interpolate
	// before their saturate, and scorch negates after interpolating - doing
	// it the other way round changes the answer wherever the quad straddles
	// zero, which is exactly the rim of a scorch mark.
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	scorch = saturate(-v.y);
	crust = saturate(v.z);
}

float SampleCrust(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float c00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).z;
	float c10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).z;
	float c01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).z;
	float c11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).z;
	return saturate(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y));
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
			float2 dims;
			ExclusionFieldMap.GetDimensions(dims.x, dims.y);
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
	return UndulationHeight(worldXY, UndulationScale, UndulationAmp, UndulationBumps.xyz);
}

// ---- Baked undulation (UndulationFieldCS) ----
// Height in world units (x) and its +-kUndulationGradStep gradient (yz) in a
// camera-snapped world window: UndulationFieldWindow = (centre XY,
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
		result = UndulationBakedHG(worldXY).x;
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
		result = UndulationBakedHG(worldXY).yz;
	else {
		const float uStep = kUndulationGradStep;
		result = float2(
					 Undulation(worldXY + float2(uStep, 0.0)) - Undulation(worldXY - float2(uStep, 0.0)),
					 Undulation(worldXY + float2(0.0, uStep)) - Undulation(worldXY - float2(0.0, uStep))) /
		         (2.0 * uStep);
	}
	return result;
}

// Deformation carves the layer toward the trench floor; the floor rides the
// live Trench Floor Height slider (BorderStyle.y).
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
float CarveProfile(float deformation, float uncarvedDepth, float2 worldXY)
{
	float depthT = smoothstep(6.0, 18.0, uncarvedDepth);
	float d = saturate(deformation);

	// P5 teeth: the border work's own two-octave recipe (37-unit wander +
	// 8-unit raggedness, HEIGHT-BLEND-PLAN - reused, not reinvented),
	// applied to the carve value inside the rim band only.
	// The contour breaks into teeth; floors (d high) and open snow (d = 0)
	// sit outside the band and never move. Faded by depthT: teeth are
	// cut-wall vocabulary, dimples stay smooth.
	[branch] if (RimStyle.y > 0.001)
	{
		float notch = (ShapeNoise(worldXY / 37.0) - 0.5) * 0.8 +
		              (ShapeNoise(worldXY / 8.0) - 0.5) * 1.2;
		float rimBand = smoothstep(0.02, 0.12, d) * (1.0 - smoothstep(0.30, 0.55, d));
		d = saturate(d + notch * RimStyle.y * 0.35 * rimBand * depthT);
	}

	float soft = d * d * d * (d * (d * 6.0 - 15.0) + 10.0);
	d = lerp(soft, d, depthT);
	float floorDepth = min(uncarvedDepth, BorderStyle.y * smoothstep(0.5, 8.0, uncarvedDepth));
	float profile = max(uncarvedDepth * (1.0 - d), floorDepth);

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
		profile += RimStyle.x * uncarvedDepth * hump * depthT;
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
// Frost pattern taps, shared by the normal, the albedo and the polish so the
// texture is fetched once and the three always agree about where a crystal
// is.
struct FrostTaps
{
	float3 normal;   // tangent-space, already flipped for our v direction
	float crystal;   // 0 in the gaps, 1 on the crystal
	bool valid;
};

// The lattice the stochastic sampler scatters over, kept inside float
// precision. ComputeStochasticOffsets multiplies by WORLD_SCALE (332.54) and
// the hash then multiplies by another 1271, both tuned for landscape UVs that
// live in 0-1. World coordinates are five digits, so the product lands past
// 1e8 - far beyond the ~1.6e7 where a float32 still has a fraction to take -
// and frac() returns the same number across whole regions, which is a
// stochastic sampler that has quietly stopped scattering.
//
// So the tile index is wrapped before it ever reaches the hash. The scatter
// pattern then repeats every WRAP tiles, which at any sane crystal size is
// tens of thousands of units away. DeformationUpdateCS guards its own noise
// the same way and for the same reason.
#define FROST_LATTICE_WRAP 512.0

FrostTaps SampleFrostPattern(float2 worldXY, float tileSize)
{
	FrostTaps taps;
	taps.normal = float3(0.0, 0.0, 1.0);
	taps.crystal = 0.0;
	taps.valid = false;

	float2 tileUV = worldXY / max(tileSize, 4.0);
	// Derivatives from the UNWRAPPED coordinate: the wrap below is a cliff one
	// pixel wide, and a mip level chosen across it would band there.
	g_terrainStochasticLodBase = ComputeTerrainStochasticLodBase(tileUV);
	float2 wrapped = tileUV - FROST_LATTICE_WRAP * floor(tileUV / FROST_LATTICE_WRAP);

	// Divided back out because the sampler expects a landscape UV and converts
	// it to lattice cells itself; this hands it one cell per texture tile.
	StochasticOffsets offsets = ComputeStochasticOffsets(wrapped / WORLD_SCALE);
	float3 n = StochasticEffect(FrostPatternNormal, SnowSampler, wrapped, offsets).xyz * 2.0 - 1.0;
	n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
	n.y = -n.y;  // DDS v grows down; our uv v grows with world +Y
	taps.normal = n;
	taps.crystal = saturate(StochasticEffect(FrostPatternDiffuse, SnowSampler, wrapped, offsets).x);
	taps.valid = true;
	return taps;
}
#endif  // PSHADER (frost)

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
