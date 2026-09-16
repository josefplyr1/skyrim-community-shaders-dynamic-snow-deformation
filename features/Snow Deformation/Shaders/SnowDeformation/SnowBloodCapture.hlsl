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

#ifdef VSHADER
#	if defined(DISC)
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
	o.Position = MapClip(world.xy, inst & 3u, o.Logical);
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
	o.Position = MapClip(world.xy, inst & 3u, o.Logical);
	o.UV = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;
	o.NormalZ = nrmWS.z / max(length(nrmWS), 1e-5);
	o.Tint = float4(1, 1, 1, 1);
	return o;
}
#	endif
#endif

#ifdef PSHADER
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
