// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Blood deposits into the toroidal blood map (BLOOD-DESIGN.md). The game's
// blood decals - engine decals clipped to the ground, Dynamic Bloodpool
// Framework's skinned quads - are re-rasterised from above with their own
// diffuse, so the authored splatter shapes survive; API callers deposit
// plain discs. RT0 = pigment (linear rgb) + concentration, alpha-blended
// with MAX alpha; RT1 = the burial clock and game hours at deposit.
//
// The map is physical (toroidal): world -> logical texel -> + MapOrigin,
// drawn four times shifted by -dim per axis so a mark straddling the seam
// lands on both sides. Fragments outside the LOGICAL window are discarded,
// or ground beyond the window would wrap onto its far edge.

cbuffer BloodCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float2 WindowOrigin;
	float TexelSize;
	float MapDim;

	int2 MapOrigin;
	float2 ClockNow;

	// xy offset, zw scale - the Lighting VS's uv convention.
	float4 TexcoordOffset;

	float Intensity;
	float AlphaThreshold;  // < 0: no alpha test
	float MaterialAlpha;
	float NormalZMin;

	// x = reveal 0..1: the mark soaks in from its dense core outward, so a
	// texel shows once its alpha exceeds (1 - reveal).
	float4 Spread;

	// Overlay: the copied screen rectangle in NDC (x0, y0, x1, y1).
	float4 OverlayRect;
}

#ifdef SKINNED
// Same palette convention as SnowContactCapture.hlsl: absolute world rows.
cbuffer BloodSkinCB : register(b2)
{
	float4 BoneRows[240];
	float SkinBoneCount;
	float3 padSkin;
}
#endif

#ifdef OVERLAY
// OVERLAY: one screen-rectangle pass after the object-snow pass. Per pixel
// it reads the G-buffer as the game left it before its blood decals drew
// (PD) and after them (P), both copied over the decals' screen rectangle
// (Blood.cpp), and puts the decals' own contribution back over thin snow:
// blood leaves no green or blue, so the stack's alpha is
// A = 1 - P.gb / PD.gb and the contribution P - (1 - A) PD, blended
// ONE / INV_SRC_ALPHA into the lit diffuse, the normal + gloss, the albedo,
// the specular and the reflectance. No decal geometry and no per-decal
// alpha: stacked decals, the game's own fade and its depth test are all
// in P already, and pixels the game did not paint stay untouched. Kept
// where the depth after the skin pass sits 0.05..6 units nearer than the
// pre-snow depth (a thin layer drew here); bare rock has no lift and the
// deep shell keeps its extinction smear.
#	ifdef PSHADER
#		include "Common/SharedData.hlsli"
// Pre-snow depth (TerrainBlending's copy) and the skin pass's own depth.
Texture2D<float> SceneDepth : register(t3);
Texture2D<float> SkinDepth : register(t4);
// Lit diffuse, normal + gloss, albedo, specular, reflectance: before any
// snow drew (t5-t9) and before the frame's first blood decal drew (t10-t14).
Texture2D<float4> PreSnow0 : register(t5);
Texture2D<float4> PreSnow2 : register(t6);
Texture2D<float4> PreSnow3 : register(t7);
Texture2D<float4> PreSnow4 : register(t8);
Texture2D<float4> PreSnow5 : register(t9);
Texture2D<float4> PreDecal0 : register(t10);
Texture2D<float4> PreDecal2 : register(t11);
Texture2D<float4> PreDecal3 : register(t12);
Texture2D<float4> PreDecal4 : register(t13);
Texture2D<float4> PreDecal5 : register(t14);
#	endif
// View-distance units. Coat lift is 0.4; a rise or the shell is 10-40.
static const float kOverlayMinLift = 0.05;
static const float kOverlayMaxLift = 6.0;
#endif

#ifdef DISC
// [2i] = xyz world centre, w radius; [2i+1] = rgb pigment, w amount.
StructuredBuffer<float4> Discs : register(t1);
#endif

Texture2D<float4> Diffuse : register(t0);
SamplerState LinearSampler : register(s0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 UV : TEXCOORD0;
	float NormalZ : TEXCOORD1;
	float4 Tint : TEXCOORD2;
	float2 Logical : TEXCOORD3;
};

float4 MapClip(float2 worldXY, uint seam, out float2 logical)
{
	logical = (worldXY - WindowOrigin) / TexelSize;
	float2 phys = logical + float2(MapOrigin) - float2(seam & 1u, seam >> 1u) * MapDim;
	return float4(phys.x / MapDim * 2.0 - 1.0, 1.0 - phys.y / MapDim * 2.0, 0.5, 1.0);
}

float4 PlaceVertex(float3 world, uint seam, out float2 logical)
{
	return MapClip(world.xy, seam, logical);
}

#ifdef VSHADER
#	if defined(OVERLAY)
VS_OUTPUT main(uint vid : SV_VertexID)
{
	const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	float2 c = corners[vid];
	VS_OUTPUT o;
	o.Position = float4(lerp(OverlayRect.x, OverlayRect.z, c.x), lerp(OverlayRect.y, OverlayRect.w, c.y), 0.5, 1.0);
	o.UV = c;
	o.NormalZ = 1.0;
	o.Tint = float4(1, 1, 1, 1);
	o.Logical = float2(0.0, 0.0);
	return o;
}
#	elif defined(DISC)
VS_OUTPUT main(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
	const uint disc = inst >> 2;
	const float4 centre = Discs[disc * 2];
	const float4 look = Discs[disc * 2 + 1];
	// Two triangles, corners in [-1, 1].
	const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	float2 c = corners[vid];
	VS_OUTPUT o;
	o.Position = MapClip(centre.xy + c * centre.w, inst & 3u, o.Logical);
	o.UV = c;
	o.NormalZ = 1.0;
	o.Tint = look;
	return o;
}
#	elif defined(SKINNED)
struct VS_INPUT_SKIN
{
	float4 Position : POSITION0;
	float2 TexCoord : TEXCOORD0;
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
};

VS_OUTPUT main(VS_INPUT_SKIN input, uint inst : SV_InstanceID)
{
	int4 rows = int4(765.01 * input.BoneIndices);
	float4 w = input.BoneWeights;
	const int lastRow = int(SkinBoneCount) * 3;
	rows = (rows < lastRow) ? rows : int4(0, 0, 0, 0);
	float4 posMS = float4(input.Position.xyz, 1.0);
	float3 world =
		mul(float3x4(BoneRows[rows.x], BoneRows[rows.x + 1], BoneRows[rows.x + 2]), posMS) * w.x +
		mul(float3x4(BoneRows[rows.y], BoneRows[rows.y + 1], BoneRows[rows.y + 2]), posMS) * w.y +
		mul(float3x4(BoneRows[rows.z], BoneRows[rows.z + 1], BoneRows[rows.z + 2]), posMS) * w.z +
		mul(float3x4(BoneRows[rows.w], BoneRows[rows.w + 1], BoneRows[rows.w + 2]), posMS) * w.w;
	VS_OUTPUT o;
	o.Position = PlaceVertex(world, inst & 3u, o.Logical);
	o.UV = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;
	// Pool quads lie on the ground by construction.
	o.NormalZ = 1.0;
	o.Tint = float4(1, 1, 1, 1);
	return o;
}
#	else
struct VS_INPUT
{
	float4 Position : POSITION0;
	float2 TexCoord : TEXCOORD0;
	float4 Normal : NORMAL0;
};

VS_OUTPUT main(VS_INPUT input, uint inst : SV_InstanceID)
{
	float3 posMS = input.Position.xyz;
	float3 world = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);
	float3 nrmMS = input.Normal.xyz * 2.0 - 1.0;
	float3 nrmWS = float3(
		dot(WorldRow0.xyz, nrmMS),
		dot(WorldRow1.xyz, nrmMS),
		dot(WorldRow2.xyz, nrmMS));
	VS_OUTPUT o;
	o.Position = PlaceVertex(world, inst & 3u, o.Logical);
	o.UV = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;
	o.NormalZ = nrmWS.z / max(length(nrmWS), 1e-5);
	o.Tint = float4(1, 1, 1, 1);
	return o;
}
#	endif
#endif

#if defined(PSHADER) && defined(OVERLAY)
struct OVERLAY_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 NormalGloss : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
};

float4 DecalPart(Texture2D<float4> p, Texture2D<float4> pd, int3 pixel, float a)
{
	return float4(max(p.Load(pixel).rgb - (1.0 - a) * pd.Load(pixel).rgb, 0.0), a);
}

OVERLAY_OUTPUT main(VS_OUTPUT input)
{
	const int3 pixel = int3(input.Position.xy, 0);
	float sceneDist = SharedData::GetScreenDepth(SceneDepth.Load(pixel));
	float lift = sceneDist - SharedData::GetScreenDepth(SkinDepth.Load(pixel));
	if (lift < kOverlayMinLift || lift > kOverlayMaxLift)
		discard;
	float3 p0 = PreSnow0.Load(pixel).rgb;
	float3 d0 = PreDecal0.Load(pixel).rgb;
	float2 ratio = p0.gb / max(d0.gb, 0.02);
	float a = saturate(1.0 - min(ratio.x, ratio.y));
	if (a < 0.02)
		discard;
	OVERLAY_OUTPUT o;
	o.Diffuse = DecalPart(PreSnow0, PreDecal0, pixel, a);
	o.NormalGloss = DecalPart(PreSnow2, PreDecal2, pixel, a);
	o.Albedo = DecalPart(PreSnow3, PreDecal3, pixel, a);
	o.Specular = DecalPart(PreSnow4, PreDecal4, pixel, a);
	o.Reflectance = DecalPart(PreSnow5, PreDecal5, pixel, a);
	return o;
}
#elif defined(PSHADER)
struct PS_OUTPUT
{
	float4 Blood : SV_Target0;
	float2 Clock : SV_Target1;
};

PS_OUTPUT main(VS_OUTPUT input)
{
	if (any(input.Logical < 0.0) || any(input.Logical >= MapDim))
		discard;
#	ifdef DISC
	// Soft disc: full inside 60% of the radius, gone at the rim.
	float a = input.Tint.w * (1.0 - smoothstep(0.6, 1.0, length(input.UV)));
	float3 rgb = input.Tint.rgb;
#	else
	if (input.NormalZ < NormalZMin)
		discard;
	float4 c = Diffuse.Sample(LinearSampler, input.UV);
	float a = c.a * MaterialAlpha;
	[flatten] if (AlphaThreshold >= 0.0)
		a = a >= AlphaThreshold ? 1.0 : 0.0;
	// Spreading: dense texels first, the thin fringe as the reveal reaches
	// it; the map keeps the high-water mark so the shape only grows.
	a *= smoothstep(1.0 - Spread.x, 1.0 - Spread.x + 0.3, c.a);
	float3 rgb = c.rgb * input.Tint.rgb;
#	endif
	a *= Intensity;
	if (a < 0.02)
		discard;
	PS_OUTPUT o;
	o.Blood = float4(rgb, saturate(a));
	o.Clock = ClockNow;
	return o;
}
#endif
