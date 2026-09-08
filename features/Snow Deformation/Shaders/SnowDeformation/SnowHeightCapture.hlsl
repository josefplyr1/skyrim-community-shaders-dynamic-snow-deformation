// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Top-down height capture of snow statics.
//
// Captured statics are rasterized from above into R32F world-height maps
// with MIN/MAX blending (no depth buffer needed; the highest/lowest surface
// wins in any draw order): object TOPS, object BOTTOMS (together they tell
// whether a texel's object is grounded or floating), and the snow-layer
// depth the texel's model class wears; the surface description later snow
// systems (the trench patch, sheltering, burial mounds) build on.
//
// StaticCB layout must match StaticsCB in SnowDeformation.h and StaticCB in
// SnowStaticsShell.hlsl.

cbuffer StaticCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;
	float2 HeightWindowCenter;
	float HeightHalfExtent;

	float HasSmoothedNormals;  // layout sync with SnowStaticsShell; stats-only here
	float RoundedDepth;
	float VertexCountF;
	float HasObjectTop;  // layout sync with SnowStaticsShell; unused here

	float SkinHeightFadeEnd;  // layout sync with SnowStaticsShell; unused here
	float LegacySkin;         // layout sync with SnowStaticsShell; unused here
	float MoundSteepness;     // layout sync with SnowStaticsShell; unused here
	// >0.5: this object may be trenched. Zero skin depth makes the patch's
	// texels dead for it, which is how a class is switched off.
	float ObjectTrenches;

	float FullCoat;
	float FadeExempt;           // layout sync with SnowStaticsShell; unused here
	// >0.5: this draw is a road-heightfield object; RT2.g carries the bit so
	// the patch can own the column outright.
	float RoadField;
	float ProjThreshold;     // layout sync with SnowStaticsShell; unused here
	float padProjMask;       // layout sync with SnowStaticsShell
	float padProjDensity;    // layout sync with SnowStaticsShell
	// Class override code, matching the skin VS: 0 = flat classifier,
	// 1 = force ROUNDED (mountain/cliff family; every PD draw in
	// authored-relief mode), 2 = force FLAT (plank family). Mirror in
	// SnowDeformation.h.
	float ClassOverride;
	float ProjNoiseScale;   // layout sync with SnowStaticsShell; unused here
	float ProjSnowFillSk;   // layout sync with SnowStaticsShell; unused here
	float ProjNoiseTiling;    // layout sync with SnowStaticsShell; unused here
	float ProjPixelEnable;    // layout sync with SnowStaticsShell; unused here
	float HasSkinNormalCopy;  // layout sync with SnowStaticsShell; unused here
	float ShellMinNz;         // layout sync with SnowStaticsShell; unused here
	// Peel tolerance ("Plane Merge Height" knob): surfaces within this
	// z-band of a layer's top belong to that layer's plane. Mirror in
	// SnowStaticsShell.hlsl / SnowDeformation.h.
	float PeelTol;
	float OverheadIgnore;  // layout sync with SnowStaticsShell; unused here
	float padMeldSk;       // layout sync with SnowStaticsShell

	float PileHeightRatio;  // layout sync with SnowStaticsShell; unused here
	float padSkyExposure;
	float padCorniceLip;
	float padBreakup;
	float padWeld;

	float HasSkinMasksCopy;  // layout sync with SnowStaticsShell; unused here
	float padLumpSize;
	float EdgeFlankWidth;    // layout sync with SnowStaticsShell; unused here
	float EdgeCoat;          // layout sync with SnowStaticsShell; unused here
	float FineHalfExtent;  // layout sync with SnowStaticsShell; unused here
	float ShellCoverage;  // layout sync with SnowStaticsShell; unused here
	float PadStatics2;
}

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
	// x = class layer depth, y = >0.5 when this draw is a road-heightfield
	// object (the PS turns it into the road's top height).
	float2 SkinDepth : TEXCOORD1;
	float2 WorldXY : TEXCOORD2;
	// World-space normal z: the peel passes reject surfaces that cannot
	// carry snow (undersides, walls) from owning a layer.
	float NormalZ : TEXCOORD3;
};

#ifdef VSHADER
// Flatness stats (element past the last vertex); the same GPU
// classification the skin uses, so the skin-depth raster (RT2) reports the
// class layer depth per texel.
StructuredBuffer<float4> SmoothedNormals : register(t10);

VS_OUTPUT main(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);

	// Ortho top-down: world XY window to NDC. +worldY maps to +ndcY, which
	// rasterizes to texture v=0 at the top; the samplers mirror this.
	float2 ndc = (worldAbs.xy - HeightWindowCenter) / HeightHalfExtent;

	float skinDepth = RoundedDepth;
	[branch] if (HasSmoothedNormals > 0.5 && ClassOverride < 0.5)
	{
		// Same flat condition as the skin VS (divergence-only, with the
		// same class overrides).
		float4 flatStats = SmoothedNormals[(uint)VertexCountF];
		[flatten] if (flatStats.w > 0.5 && flatStats.x > 0.5)
			skinDepth = ObjectsDepth;
	}
	[flatten] if (ClassOverride > 1.5)
		skinDepth = ObjectsDepth;
	// Parked: only roads carve until object trenching is done properly.
	[flatten] if (ObjectTrenches < 0.5 && LegacySkin < 0.5)
		skinDepth = 0.0;

	VS_OUTPUT vsout;
	vsout.Position = float4(ndc.x, ndc.y, 0.5, 1.0);
	vsout.WorldZ = worldAbs.z;
	vsout.SkinDepth = float2(skinDepth, RoadField > 0.5 ? 1.0 : 0.0);
	vsout.WorldXY = worldAbs.xy;
	float3 nrmMS = input.Normal.xyz * 2.0 - 1.0;
	float3 nrmWS = float3(
		dot(WorldRow0.xyz, nrmMS),
		dot(WorldRow1.xyz, nrmMS),
		dot(WorldRow2.xyz, nrmMS));
	vsout.NormalZ = nrmWS.z / max(length(nrmWS), 1e-5);
	return vsout;
}
#endif

#if defined(PSHADER) && (defined(PEEL) || defined(PEEL2))
// S4 phase 2 - layer peels (SKIN-PLACEMENT-PLAN): re-rasterize the
// captures keeping only fragments a peel tolerance BELOW this frame's
// accumulated layer-1 top (PEEL2: below layer 2 as well); MAX blending
// then yields the next-highest surface per column. Every plank, tread
// and beam below a roof or railing gets its own plane, its own rims,
// its own roll.
Texture2D<float> Layer1Top : register(t3);
#	if defined(PEEL2)
Texture2D<float> Layer2Top : register(t4);
#	endif

struct PEEL_OUTPUT
{
	float Top : SV_Target0;
};

PEEL_OUTPUT main(VS_OUTPUT input)
{
	// Only up-facing surfaces may OWN a peeled layer. The capture
	// rasterizes both faces (the bottoms map needs undersides), and a
	// roof's own underside claiming layer 2 starved the real floor
	// beneath it of any plane at all - the "no snow under roofs"
	// remnant. Undersides and walls can never carry snow.
	[branch] if (input.NormalZ < 0.05)
		discard;
	float2 dims;
	Layer1Top.GetDimensions(dims.x, dims.y);
	float2 local = (input.WorldXY - HeightWindowCenter) / HeightHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	int2 t = int2(clamp(uv * dims - 0.5, 0.0, dims.x - 1.001));
	float top1 = Layer1Top.Load(int3(t, 0));
	[branch] if (top1 > -50000.0 && input.WorldZ > top1 - PeelTol)
		discard;
#	if defined(PEEL2)
	// No third layer without a second, and only strictly below it.
	float top2 = Layer2Top.Load(int3(t, 0));
	[branch] if (top2 < -50000.0 || input.WorldZ > top2 - PeelTol)
		discard;
#	endif
	PEEL_OUTPUT o;
	o.Top = input.WorldZ;
	return o;
}
#elif defined(PSHADER)
// Prefix mirror of HeightProcessCB (SnowDeformation.h) - only the terrain
// window addressing is read here; names carry an H so they cannot clash
// with StaticCB's.
cbuffer HeightProcessCB : register(b0)
{
	int2 ScrollDeltaH;
	uint ClearAllH;
	uint ConeStepH;
	float2 HeightWindowCenterH;
	float HeightHalfExtentH;
	float SlopePerUnitH;
	float2 TerrainWindowOriginH;
	float TerrainTexelSizeH;
	uint TerrainDimH;
}

Texture2D<float4> TerrainWindowCapture : register(t2);

float CaptureTerrainHeight(float2 worldXY)
{
	float2 t = (worldXY - TerrainWindowOriginH) / TerrainTexelSizeH;
	t = clamp(t, 0.0, (float)(TerrainDimH - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDimH - 1, TerrainDimH - 1));
	float s00 = TerrainWindowCapture.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = TerrainWindowCapture.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = TerrainWindowCapture.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = TerrainWindowCapture.Load(int3(t1.x, t1.y, 0)).x;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

struct PS_OUTPUT
{
	// RT0 blends MAX (object top surface), RT1 blends MIN (object bottom),
	// RT2 blends MAX on BOTH channels: x = the snow-layer depth this texel's
	// class wears, y = the highest ROAD surface in the column (kNoRoadTop
	// where no road drew). The patch compares y against RT0 to decide whether
	// the road actually owns the column, rather than merely reaching it.
	float Top : SV_Target0;
	float Bottom : SV_Target1;
	float2 SkinDepth : SV_Target2;
};

// Mirror of SnowDeformation.h kNoRoadTop.
static const float kNoRoadTop = -1000000.0;

PS_OUTPUT main(VS_OUTPUT input)
{
	PS_OUTPUT psout;
	psout.Top = input.WorldZ;
	// Bottoms accept only genuinely ELEVATED undersides: grounded geometry
	// (support posts, rocks, low clutter) min-blending into the channel
	// vetoed the floating-structure shelter test under walkways and roofs,
	// leaving unmelted snow plateaus beneath them. Grounded fragments write
	// the bottom-empty sentinel, a no-op under MIN blending.
	float terrain = CaptureTerrainHeight(input.WorldXY);
	psout.Bottom = input.WorldZ - terrain < 40.0 ? 100000.0 : input.WorldZ;
	psout.SkinDepth = float2(input.SkinDepth.x,
		input.SkinDepth.y > 0.5 ? input.WorldZ : kNoRoadTop);
	return psout;
}
#endif
