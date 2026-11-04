// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "SnowDeformation/SnowCommon.hlsli"

cbuffer StaticCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;
	float2 HeightWindowCenter;
	float HeightHalfExtent;

	float HasSmoothedNormals;
	float RoundedDepth;
	float VertexCountF;
	float HasObjectTop;

	float DebugSkinId;
	float LegacySkin;
	float padMoundSteepness;
	float AlphaTested;

	float FullCoat;
	float FadeExempt;
	float RoadField;
	float ProjThreshold;
	float ProjNoiseWeight;
	float ParallaxFollow;
	float ClassOverride;
	float ProjNoiseScale;
	float SteepThin;
	float ProjNoiseTiling;
	float ProjPixelEnable;
	float padSkinNormalCopy;
	float padShellMinNz;
	float PeelTol;
	float SkinSssSeen;
	float padMeldSk;

	float FarWindowCenterX;
	float FarWindowCenterY;
	float FarHalfExtent;
	float FarRoadDepth;
	float DecalMode;

	float HasSkinMasksCopy;
	float HasPreDecalMasks;
	float EdgeFlankWidth;
	float EdgeCoat;
	float FineHalfExtent;
	float LODBatch;
	float TexturedSnow;
}

#if defined(WATER)
struct WATER_VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

#	if defined(VSHADER)
WATER_VS_OUTPUT main(float4 position : POSITION0)
{
	float3 posMS = position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);
	float2 ndc = (worldAbs.xy - HeightWindowCenter) / HeightHalfExtent;
	WATER_VS_OUTPUT vsout;
	vsout.Position = float4(ndc.x, ndc.y, 0.5, 1.0);
	vsout.WorldZ = worldAbs.z;
	return vsout;
}
#	endif

#	if defined(PSHADER)
float main(WATER_VS_OUTPUT input) : SV_Target0
{
	return input.WorldZ;
}
#	endif
#else

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
	float2 SkinDepth : TEXCOORD1;
	float2 WorldXY : TEXCOORD2;
	float NormalZ : TEXCOORD3;
};

#ifdef VSHADER
StructuredBuffer<float4> SmoothedNormals : register(t10);

VS_OUTPUT main(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);

	float2 ndc = (worldAbs.xy - HeightWindowCenter) / HeightHalfExtent;

	float skinDepth = RoundedDepth;
	[branch] if (HasSmoothedNormals > 0.5 && ClassOverride < 0.5)
	{
		float4 flatStats = SmoothedNormals[(uint)VertexCountF];
		[flatten] if (flatStats.w > 0.5 && flatStats.x > 0.5)
			skinDepth = ObjectsDepth;
	}
	[flatten] if (ClassOverride > 1.5)
		skinDepth = ObjectsDepth;
	[flatten] if (LegacySkin < 0.5)
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

#if defined(PSHADER) && defined(PEEL)
Texture2D<float> Layer1Top : register(t3);

struct PEEL_OUTPUT
{
	float Top : SV_Target0;
};

PEEL_OUTPUT main(VS_OUTPUT input)
{
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
	PEEL_OUTPUT o;
	o.Top = input.WorldZ;
	return o;
}
#elif defined(PSHADER) && defined(TOP_ONLY)
float main(VS_OUTPUT input) : SV_Target0
{
	return input.WorldZ;
}
#elif defined(PSHADER) && defined(SKIN_DEPTH)
static const float kNoRoadTop = -1000000.0;

float2 main(VS_OUTPUT input) : SV_Target0
{
	return float2(input.SkinDepth.x, input.SkinDepth.y > 0.5 ? input.WorldZ : kNoRoadTop);
}
#elif defined(PSHADER)
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

float2 main(VS_OUTPUT input) : SV_Target0
{
	float terrain = TerrainWindowHeight(TerrainWindowCapture, (input.WorldXY - TerrainWindowOriginH) / TerrainTexelSizeH, TerrainDimH);
	float bottom = input.WorldZ - terrain < 40.0 ? 100000.0 : input.WorldZ;
	return float2(input.WorldZ, -bottom);
}
#endif
#endif
