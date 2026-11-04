// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Triplanar.hlsli"

#include "TerrainVariation/TerrainVariation.hlsli"
#include "SnowDeformation/SnowColor.hlsli"

#ifdef PSHADER
#	include "SnowDeformation/SnowShellPS.hlsli"
#endif

#include "SnowDeformation/SnowShellCB.hlsli"

cbuffer StaticCB : register(b1)
{
#include "SnowDeformation/StaticCBFields.hlsli"

	float4 ObjParallaxUV;
	float4 ObjParallax;
}

#ifdef SNOW_STATICS_DRAPE
#	define ObjectsDepth 0.0
#	define RoundedDepth 0.0
#	define LegacySkin 0.0
#	define RoadField 0.0
#	define ClassOverride 1.0
#	define ProjPixelEnable 2.0
#endif

Texture2D<float4> DeformationMap : register(t1);
#include "SnowDeformation/SnowFieldMaps.hlsli"
Texture2D<float4> ProjNoiseMap : register(t21);
Texture2D<float4> GameShadowMask : register(t24);
Texture2D<float3> PreSkinMasks : register(t32);
Texture2D<float3> PreDecalMasks : register(t53);

#if defined(PSHADER) || defined(DOMAINSHADER) || ((defined(VSHADER) || defined(HULLSHADER)) && defined(PATCH)) || defined(SNOW_ROAD_MARCH_CS)
Texture2D<float4> TerrainWindow : register(t0);

float3 SampleTerrainStatics(float2 gridLocal)
{
	SHELL_TERRAIN_TEXEL(gridLocal);

	float3 s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).xyz;
	float3 s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).xyz;
	float3 s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).xyz;
	float3 s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).xyz;

	float3 result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	result.y = max(result.y, 0.0) * RimStyle.w + min(result.y, 0.0);
	return result;
}
#endif

#if defined(PSHADER) || defined(DOMAINSHADER)
Texture2D<float4> SnowDiffuse : register(t2);
Texture2D<float4> SnowNormalMap : register(t6);
Texture2D<float4> SnowRmaosMap : register(t7);
Texture2D<float4> SnowHeightMap : register(t8);
SamplerState SnowSampler : register(s0);
#endif

#if defined(PSHADER) && (defined(PATCH) || defined(SNOW_OBJECT_PRINTS))
#	define SNOW_NOISE_TABLE
#endif
#if defined(PSHADER) && defined(SNOW_PATCH_DEFERRED)
static float2 gDeferredGridDx;
static float2 gDeferredGridDy;
static float gDeferredZDx;
static float gDeferredZDy;
#endif
#include "SnowDeformation/SnowFields.hlsli"
#if defined(PSHADER) && defined(SNOW_OBJECT_PRINTS)
#	define SNOW_PRINT_RIM
#	include "SnowDeformation/SnowPrints.hlsli"
#endif
#if defined(PSHADER) && defined(SNOW_OBJECT_PRINTS)
static const float kPrintDentDepth = 4.0;
#endif

#include "SnowDeformation/SnowGrid.hlsli"

static const float kPatchBandVerts[kWarpBands] = { 128.0, 16.0, 8.0, 4.0, 57.0 };
static const float kPatchBandMul[kWarpBands] = { 1.0, 2.0, 4.0, 8.0, 32.0 };
static const float kPatchStep = 8.0;
#define kPatchGridDim 426

float2 PatchWarpXY(float2 u)
{
	return float2(WarpAxisT(u.x, kPatchBandVerts, kPatchBandMul, kPatchStep),
		WarpAxisT(u.y, kPatchBandVerts, kPatchBandMul, kPatchStep));
}

static const float kSnowUVTile = 4096.0 / 24.0;

static const float kMinSkinLift = 0.1;

static const float kProjCoatLift = 2.0;
static const float kSkinShadeSmooth = 0.0;

static const float kCorniceRoll = 4.0;
static const float kEdgeFlankLift = 0.4;
static const float kCoatSolidW = 0.15;
static const float kEdgeReachWeightShift = 0.25;
static const float kRecolorWeightSpan = 0.8;
static const float kRecolorNearSpan = 0.3;
static const float kRecolorNearShare = 5.0 / 6.0;
static const float kDetileRamp = 0.5;
static const float kDetileGrid = 3.4641016;
static const float kCoatMinNz = -1.0;
static const float kCoatSolidReal = 0.5;
float SkinRecolorEnc(float enc)
{
	enc = enc >= 256.0 ? enc / 256.0 : enc;
	return enc < 1.5 ? 0.0 : (enc >= 40.0 ? 3.0 : 2.0);
}
float SkinRecolorWeight(float enc)
{
	enc = enc >= 256.0 ? enc / 256.0 : enc;
	const float t = log2(enc * 0.25) / 3.0;
	const float w = t >= 1.0 - kRecolorNearShare ? (t - 1.0) * (kRecolorNearSpan / kRecolorNearShare) :
	                                               t * ((kRecolorWeightSpan - kRecolorNearSpan) / (1.0 - kRecolorNearShare)) - kRecolorWeightSpan;
	return (enc >= 3.5 && enc < 40.0) ? w : -1.0;
}
#ifdef PSHADER
float2 DetileOffset(int2 a_vertex)
{
	return float2(Random::pcg2d(asuint(a_vertex))) * (1.0 / 4294967296.0);
}

float DetiledNoisePlane(float2 a_uv, float2 a_dx, float2 a_dy, float a_mean)
{
	const float2 g = a_uv * kDetileGrid;
	const float2 skew = float2(g.x - 0.57735027 * g.y, 1.15470054 * g.y);
	const float2 cell = floor(skew);
	const float2 f = skew - cell;
	const float fz = 1.0 - f.x - f.y;
	const int2 c = int2(cell);
	const bool lower = fz > 0.0;
	float3 w = lower ? float3(fz, f.y, f.x) : float3(-fz, 1.0 - f.y, 1.0 - f.x);
	const int2 v1 = lower ? c : c + int2(1, 1);
	const int2 v2 = lower ? c + int2(0, 1) : c + int2(1, 0);
	const int2 v3 = lower ? c + int2(1, 0) : c + int2(0, 1);
	w *= w;
	w /= dot(w, 1.0);
	const float3 n = float3(ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v1), a_dx, a_dy).x,
		ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v2), a_dx, a_dy).x,
		ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v3), a_dx, a_dy).x);
	return a_mean + dot(w, n - a_mean) * rsqrt(dot(w, w));
}

float ProjNoiseGame(float3 a_p, float3 a_triW, float3 a_dx, float3 a_dy)
{
	float n = 0.0;
	[branch] if (a_triW.x > 0.001)
		n += a_triW.x * ProjNoiseMap.SampleGrad(SnowSampler, a_p.yz, a_dx.yz, a_dy.yz).x;
	[branch] if (a_triW.y > 0.001)
		n += a_triW.y * ProjNoiseMap.SampleGrad(SnowSampler, a_p.xz, a_dx.xz, a_dy.xz).x;
	[branch] if (a_triW.z > 0.001)
		n += a_triW.z * ProjNoiseMap.SampleGrad(SnowSampler, a_p.xy, a_dx.xy, a_dy.xy).x;
	return n;
}

float ProjNoiseDetiled(float3 a_p, float3 a_triW, float3 a_dx, float3 a_dy)
{
	const float mean = ProjNoiseMap.SampleLevel(SnowSampler, 0.5.xx, 16.0).x;
	float n = 0.0;
	[branch] if (a_triW.x > 0.001)
		n += a_triW.x * DetiledNoisePlane(a_p.yz, a_dx.yz, a_dy.yz, mean);
	[branch] if (a_triW.y > 0.001)
		n += a_triW.y * DetiledNoisePlane(a_p.xz, a_dx.xz, a_dy.xz, mean);
	[branch] if (a_triW.z > 0.001)
		n += a_triW.z * DetiledNoisePlane(a_p.xy, a_dx.xy, a_dy.xy, mean);
	return saturate(n);
}
#endif
float SkinMasksY(int2 a_px)
{
	float live = PreSkinMasks.Load(int3(a_px, 0)).y;
	[branch] if (HasPreDecalMasks > 0.5)
	{
		float pre = PreDecalMasks.Load(int3(a_px, 0)).y;
		float liveEnc = SkinRecolorEnc(live);
		float preEnc = SkinRecolorEnc(pre);
		float liveN = live >= 256.0 ? live / 256.0 : live;
		float preN = pre >= 256.0 ? pre / 256.0 : pre;
		bool lifted = liveEnc == 2.0 && preEnc < 3.0 && liveN > 32.25 && liveN >= 24.0 + 0.5 * preN;
		bool covered = TexturedSnow > 0.5 && preN >= 1.5 && liveN < 2.5;
		[flatten] if (!covered && (preEnc > liveEnc || (preEnc == liveEnc && SkinRecolorWeight(pre) > SkinRecolorWeight(live))))
			live = pre;
		[flatten] if (covered)
			live = 2.0;
		[flatten] if (lifted)
			live = 48.0;
	}
	return live;
}

static const float kShelterNear = 8.0;
static const float kShelterFar = 32.0;
static const float kShelterDust = 1.0;

static const float kRimBandPx = 1.2;
static const float kRimBandMax = 0.25;

float SampleDeformation(float2 gridLocal)
{
	SNOW_DEFORM_BILINEAR4(v, gridLocal);
	return saturate(v.x);
}

float4 BermFieldPair(float2 gridLocal)
{
	return BermFieldBaked(gridLocal);
}

float BermField(float2 gridLocal)
{
	return BermFieldPair(gridLocal).x;
}

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
#ifdef SNOW_COAT_PARALLAX
	float2 TexCoord0 : TEXCOORD0;
	float4 Bitangent : BINORMAL0;
#endif
	uint VertexID : SV_VertexID;
};

#ifdef VSHADER
StructuredBuffer<float4> SmoothedNormals : register(t10);
#endif

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float4 CurrentClip : TEXCOORD0;
	float4 PreviousClip : TEXCOORD1;
	float3 WorldPos : TEXCOORD2;
	float3 NormalWS : TEXCOORD3;
	float2 GridLocal : TEXCOORD4;
	float Coverage : TEXCOORD5;
	float Flat : TEXCOORD6;
	float Lift : TEXCOORD7;
	float ProjFactor : TEXCOORD8;
	float LiftTarget : TEXCOORD9;
#ifdef SNOW_COAT_PARALLAX
	float2 ObjUV : TEXCOORD10;
	float3 ObjT : TEXCOORD11;
	float3 ObjB : TEXCOORD12;
	float3 ObjN : TEXCOORD13;
#endif
};

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER) || defined(SNOW_ROAD_MARCH_CS)
Texture2D<float> ObjectTopRaw : register(t11);
Texture2D<float> ObjectSnowCone : register(t13);
Texture2D<float> ObjectTopFine : register(t34);
Texture2D<float> ObjectTopFar : register(t40);
#endif
#if defined(PATCH) || defined(PSHADER) || defined(SNOW_ROAD_MARCH_CS)
Texture2D<float2> ObjectSkinDepth : register(t12);
static const float kNoRoadTop = -1000000.0;
static const float kRoadOwnsTop = 8.0;
#endif

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER) || defined(SNOW_ROAD_MARCH_CS)

static const float kHeightTexel = 4.0;

float2 PatchTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - HeightWindowCenter) / HeightHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

float2 FineTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - HeightWindowCenter) / FineHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

float FineTop(float2 worldXY)
{
	float result = -1000000.0;
	float2 d = abs(worldXY - HeightWindowCenter);
	[branch] if (FineHalfExtent > 0.0 && max(d.x, d.y) <= FineHalfExtent)
	{
		float2 dims;
		ObjectTopFine.GetDimensions(dims.x, dims.y);
		float2 tf = FineTexel(worldXY, dims);
		int2 t0 = (int2)tf;
#ifdef PSHADER
		const float4 tops = ObjectTopFine.GatherRed(ShellLinearSampler, (float2(t0) + 1.0) / dims);
		result = max(max(tops.x, tops.y), max(tops.z, tops.w));
#else
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = max(max(ObjectTopFine.Load(int3(t0.x, t0.y, 0)), ObjectTopFine.Load(int3(t1.x, t0.y, 0))),
			max(ObjectTopFine.Load(int3(t0.x, t1.y, 0)), ObjectTopFine.Load(int3(t1.x, t1.y, 0))));
#endif
	}
	return result;
}

float2 FarTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - float2(FarWindowCenterX, FarWindowCenterY)) / FarHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

#	define SNOW_FAR_TOP(result, reduce, worldXY)                                                                    \
		float result = -1000000.0;                                                                                  \
		float2 d = abs(worldXY - float2(FarWindowCenterX, FarWindowCenterY));                                       \
		[branch] if (FarHalfExtent > 0.0 && max(d.x, d.y) <= FarHalfExtent)                                         \
		{                                                                                                           \
			float2 dims;                                                                                            \
			ObjectTopFar.GetDimensions(dims.x, dims.y);                                                             \
			float2 tf = FarTexel(worldXY, dims);                                                                    \
			int2 t0 = (int2)tf;                                                                                     \
			int2 t1 = min(t0 + 1, int2(dims) - 1);                                                                  \
			result = reduce(reduce(ObjectTopFar.Load(int3(t0.x, t0.y, 0)), ObjectTopFar.Load(int3(t1.x, t0.y, 0))), \
				reduce(ObjectTopFar.Load(int3(t0.x, t1.y, 0)), ObjectTopFar.Load(int3(t1.x, t1.y, 0))));            \
		}

float FarTop(float2 worldXY)
{
	SNOW_FAR_TOP(result, max, worldXY)
	return result;
}

float FarTopMin(float2 worldXY)
{
	SNOW_FAR_TOP(result, min, worldXY)
	return result;
}

#	undef SNOW_FAR_TOP

float PatchTop(float2 worldXY)
{
	float result = FineTop(worldXY);
	[branch] if (result <= -50000.0)
	{
		float2 windowLocal = abs(worldXY - HeightWindowCenter);
		[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		{
			result = FarTop(worldXY);
		}
		else
		{
			float2 dims;
			ObjectTopRaw.GetDimensions(dims.x, dims.y);
			float2 t = PatchTexel(worldXY, dims);
			int2 t0 = (int2)t;
#ifdef PSHADER
			const float4 tops = ObjectTopRaw.GatherRed(ShellLinearSampler, (float2(t0) + 1.0) / dims);
			result = max(max(tops.x, tops.y), max(tops.z, tops.w));
#else
			int2 t1 = min(t0 + 1, int2(dims) - 1);
			result = max(max(ObjectTopRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopRaw.Load(int3(t1.x, t0.y, 0))),
				max(ObjectTopRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopRaw.Load(int3(t1.x, t1.y, 0))));
#endif
		}
	}
	return result;
}

float ObjectConeDepth(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return 1000000.0;

	float2 dims;
	ObjectSnowCone.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = ObjectSnowCone.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectSnowCone.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectSnowCone.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectSnowCone.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

#endif

#if ((defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)) || defined(PSHADER)

float2 PatchSkinDepth(float2 worldXY)
{
	float2 result = float2(0.0, kNoRoadTop);
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
	{
		float farTop = FarTop(worldXY);
		[flatten] if (farTop > -50000.0)
			result = float2(FarRoadDepth, farTop);
	}
	else
	{
		float2 dims;
		ObjectSkinDepth.GetDimensions(dims.x, dims.y);
		float2 t = PatchTexel(worldXY, dims);
		int2 t0 = (int2)t;
#ifdef PSHADER
		const float2 uv = (float2(t0) + 1.0) / dims;
		const float4 depths = ObjectSkinDepth.GatherRed(ShellLinearSampler, uv);
		const float4 roadTops = ObjectSkinDepth.GatherGreen(ShellLinearSampler, uv);
		result = float2(max(max(depths.x, depths.y), max(depths.z, depths.w)), max(max(roadTops.x, roadTops.y), max(roadTops.z, roadTops.w)));
#else
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = max(max(ObjectSkinDepth.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t0.y, 0))),
			max(ObjectSkinDepth.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t1.y, 0))));
#endif
	}
	return result;
}

float PatchSurfaceTop(float2 worldXY)
{
	float top = PatchTop(worldXY);
	float roadTop = PatchSkinDepth(worldXY).y;
	[flatten] if (roadTop > kNoRoadTop * 0.5 && (top - roadTop) >= kRoadOwnsTop)
		top = roadTop;
	return top;
}

float PatchTopSlope(float p, float n, float c, float step)
{
	bool vp = p > -50000.0;
	bool vn = n > -50000.0;
	[flatten] if (vp && vn)
		return (p - n) / (2.0 * step);
	[flatten] if (vp)
		return (p - c) / step;
	[flatten] if (vn)
		return (c - n) / step;
	return 0.0;
}

float PatchSilhouetteDrop(float2 worldXY)
{
	worldXY += BorderJitter(worldXY, 8.0) * 0.25;

	float result = 0.0;
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) <= HeightHalfExtent)
	{
		float2 dims;
		ObjectTopRaw.GetDimensions(dims.x, dims.y);
		float2 t = PatchTexel(worldXY, dims);
		int2 c0 = (int2)t;
		float2 cf = t - c0;
#ifdef PSHADER
		const float4 tops = ObjectTopRaw.GatherRed(ShellLinearSampler, (float2(c0) + 1.0) / dims).wzxy;
		float top00 = tops.x;
		float top10 = tops.y;
		float top01 = tops.z;
		float top11 = tops.w;
#else
		int2 c1 = min(c0 + 1, int2(dims) - 1);
		float top00 = ObjectTopRaw.Load(int3(c0.x, c0.y, 0));
		float top10 = ObjectTopRaw.Load(int3(c1.x, c0.y, 0));
		float top01 = ObjectTopRaw.Load(int3(c0.x, c1.y, 0));
		float top11 = ObjectTopRaw.Load(int3(c1.x, c1.y, 0));
#endif
		float2 sdims;
		ObjectSkinDepth.GetDimensions(sdims.x, sdims.y);
		float2 st = PatchTexel(worldXY, sdims);
		int2 s0 = (int2)st;
#ifdef PSHADER
		float4 roadTops = ObjectSkinDepth.GatherGreen(ShellLinearSampler, (float2(s0) + 1.0) / sdims).wzxy;
#else
		int2 s1 = min(s0 + 1, int2(sdims) - 1);
		float4 roadTops = float4(ObjectSkinDepth.Load(int3(s0.x, s0.y, 0)).y, ObjectSkinDepth.Load(int3(s1.x, s0.y, 0)).y,
			ObjectSkinDepth.Load(int3(s0.x, s1.y, 0)).y, ObjectSkinDepth.Load(int3(s1.x, s1.y, 0)).y);
#endif
		bool4 onRoad = roadTops > kNoRoadTop * 0.5;
		[flatten] if (any(onRoad))
		{
			top00 = onRoad.x ? roadTops.x : top00;
			top10 = onRoad.y ? roadTops.y : top10;
			top01 = onRoad.z ? roadTops.z : top01;
			top11 = onRoad.w ? roadTops.w : top11;
		}
		float maxTop = max(max(top00, top10), max(top01, top11));
		float4 drops = min(maxTop - float4(top00, top10, top01, top11), 200.0);
		result = lerp(lerp(drops.x, drops.y, cf.x), lerp(drops.z, drops.w, cf.x), cf.y);
	}
	return result;
}

bool RoadOwnsColumn(float2 worldXY)
{
	bool result = false;
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
	{
		result = FarTopMin(worldXY) > -50000.0;
	}
	else
	{
		float top = PatchTop(worldXY);
		float roadTop = PatchSkinDepth(worldXY).y;
		result = top > -50000.0 && roadTop > kNoRoadTop * 0.5 && PatchSilhouetteDrop(worldXY) < 8.0;
	}
	return result;
}
#endif

#if defined(PSHADER) || ((defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH))
Texture2D<float> TerrainFine : register(t52);

float SheetLandFine(float2 gridLocal)
{
	float land = -100000.0;
	float2 tf = (FineWindow.xy + gridLocal) / FineWindow.w;
	[branch] if (FineWindow.z > 0.5 && all(tf >= 0.0) && all(tf < FineWindow.z - 1.0))
	{
		int2 f0 = (int2)tf;
		float2 ff = tf - f0;
#ifdef PSHADER
		const float4 g = TerrainFine.GatherRed(ShellLinearSampler, (float2(f0) + 1.0) / FineWindow.z).wzxy;
		float g00 = g.x;
		float g10 = g.y;
		float g01 = g.z;
		float g11 = g.w;
#else
		float g00 = TerrainFine.Load(int3(f0, 0));
		float g10 = TerrainFine.Load(int3(f0 + int2(1, 0), 0));
		float g01 = TerrainFine.Load(int3(f0 + int2(0, 1), 0));
		float g11 = TerrainFine.Load(int3(f0 + int2(1, 1), 0));
#endif
		bool slash = ((f0.x + f0.y) & 1) == 0;
		float hA = ff.y <= ff.x ? g00 + (g10 - g00) * ff.x + (g11 - g10) * ff.y : g00 + (g01 - g00) * ff.y + (g11 - g01) * ff.x;
		float hB = (ff.x + ff.y) <= 1.0 ? g00 + (g10 - g00) * ff.x + (g01 - g00) * ff.y : g11 + (g10 - g11) * (1.0 - ff.y) + (g01 - g11) * (1.0 - ff.x);
		[flatten] if (min(min(g00, g10), min(g01, g11)) > -50000.0)
			land = slash ? hA : hB;
	}
	return land;
}

float SheetSurfaceZ(float2 gridLocal, float zFloor, out float a_depth, uniform bool a_ownDates, float a_carveAge, float a_bermScale)
{
	float z = -100000.0;
	float3 st = SampleTerrainStatics(gridLocal);
	float depth = ShellEdgeDepth(st.y - 8.0 * saturate(1.0 - st.z), ShellEdgeFade(gridLocal));
	a_depth = depth;
	[branch] if (st.x > -50000.0 && depth > 4.0)
	{
		const float fine = SheetLandFine(gridLocal);
		float land = fine > -50000.0 ? fine : st.x;
		float zMax = land + depth * (1.0 + max(RimStyle.x, 0.0) + max(BermHeightAmp, 0.0)) + abs(UndulationAmp);
		zMax += 0.01 + 1e-6 * abs(zMax);
		[branch] if (zMax > zFloor)
		{
			float2 worldXY = GridOrigin + gridLocal;
			depth = lerp(depth, min(depth, kFireMeltFloor), saturate(SampleExclusionField(worldXY).y));
			a_depth = depth;
#ifdef PATCH
			float deform = saturate(SampleDeformationSmooth(gridLocal));
#else
			float deform = SampleDeformation(gridLocal);
#endif
			float4 berm = BermBakeActive > 0.5 ? BermFieldBaked(gridLocal) : float4(0.0, deform, TrackAge.x, TrackAge.x);
			const float carveAge = a_ownDates ? TrackAgeOf(berm.z) : a_carveAge;
			const float bermScale = a_ownDates ? TrackBermScale(TrackAgeOf(berm.w)) : a_bermScale;
			float carved = CarveProfile(deform, berm.y, depth, worldXY, TrackCarveLook(carveAge)) +
			               BermShape(berm.x) * saturate(1.0 - deform) * depth * BermHeightAmp * BermDepthGate(depth) * bermScale;
			z = land + carved + UndulationSampled(worldXY) * saturate(carved / 8.0);
		}
	}
	return z;
}
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)

struct PatchVertex
{
	float3 WorldAbs;
	float2 GridLocal;
	float3 NormalWS;
	float SkinDepth;
	float Deform;
	float Killed;
	float RoadBit;
};

PatchVertex BuildPatchVertex(float2 worldXY, uniform bool dense, float2 a_trackAge)
{
	PatchVertex v;
	v.WorldAbs = float3(worldXY, 0.0);
	v.GridLocal = worldXY - GridOrigin;
	v.NormalWS = float3(0.0, 0.0, 1.0);
	v.SkinDepth = 0.0;
	v.Deform = 0.0;
	v.Killed = 1.0;
	v.RoadBit = 0.0;

	float top;
	float skinDepth;
	float skinEdgeMin;
	float roadTop;
	[branch] if (dense)
	{
		float2 base = floor(worldXY / kHeightTexel) * kHeightTexel;
		float2 f = saturate((worldXY - base) / kHeightTexel);
		float t00 = PatchTop(base);
		float t10 = PatchTop(base + float2(kHeightTexel, 0.0));
		float t01 = PatchTop(base + float2(0.0, kHeightTexel));
		float t11 = PatchTop(base + float2(kHeightTexel, kHeightTexel));
		top = lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
		float tMin = min(min(t00, t10), min(t01, t11));
		float tMax = max(max(t00, t10), max(t01, t11));
		[flatten] if (tMin < -50000.0 || (tMax - tMin) > 100.0)
			top = PatchTop(worldXY);
		float2 s00 = PatchSkinDepth(base);
		float2 s10 = PatchSkinDepth(base + float2(kHeightTexel, 0.0));
		float2 s01 = PatchSkinDepth(base + float2(0.0, kHeightTexel));
		float2 s11 = PatchSkinDepth(base + float2(kHeightTexel, kHeightTexel));
		skinDepth = lerp(lerp(s00.x, s10.x, f.x), lerp(s01.x, s11.x, f.x), f.y);
		skinEdgeMin = min(min(s00.x, s10.x), min(s01.x, s11.x));
		roadTop = max(max(s00.y, s10.y), max(s01.y, s11.y));
	}
	else
	{
		float2 skin = PatchSkinDepth(worldXY);
		top = PatchTop(worldXY);
		skinDepth = skin.x;
		skinEdgeMin = skinDepth;
		roadTop = skin.y;
	}
	float2 gridLocal = v.GridLocal;

	bool owns = false;
	[branch] if (top > -50000.0 && skinDepth >= 1.0)
	{
		owns = roadTop > kNoRoadTop * 0.5;
		[flatten] if (owns && (top - roadTop) >= kRoadOwnsTop)
			top = roadTop;
	}
	v.RoadBit = owns ? 1.0 : 0.0;

	[branch] if (top > -50000.0 && skinDepth >= 1.0 && owns)
	{
		{
			float landDepth = max(SampleTerrainStatics(gridLocal).y, 0.0);
			float cone = ObjectConeDepth(worldXY);
			skinDepth = max(min(cone, skinDepth), min(landDepth, skinDepth));
		}

		float deform = saturate(SampleDeformationSmooth(gridLocal));

		float4 bermPair = float4(0.0, deform, TrackAge.x, TrackAge.x);
		[branch] if (BermHeightAmp > 0.005 || CompactLook.w > 0.0 || (!dense && TrackAge.y > 0.0))
			bermPair = BermFieldPair(gridLocal);
		const float bermD = BermHeightAmp > 0.005 ? bermPair.x : 0.0;
		const float patchAge = dense ? a_trackAge.x : TrackAgeOf(bermPair.z);
		const float patchBermScale = dense ? a_trackAge.y : TrackBermScale(TrackAgeOf(bermPair.w));
		float depth = CarveProfile(deform, bermPair.y, skinDepth, worldXY, TrackCarveLook(patchAge)) +
		              BermShape(bermD) * saturate(1.0 - deform) * skinDepth * BermHeightAmp * BermDepthGate(skinDepth) * patchBermScale;

#ifdef SNOW_SHADOW_CAST
		float camDist = length(worldXY - WorldRow0.xy);
#else
		float camDist = length(float3(worldXY, top) - ShellCameraPosAdjust.xyz);
#endif
		float pad = min(skinDepth, 0.8 + camDist * 0.004) * smoothstep(0.25, 2.0, skinEdgeMin);
		depth = max(depth, pad);

		depth += UndulationSampled(worldXY) * saturate(depth / 8.0);

		v.WorldAbs = float3(worldXY, top + depth - 0.4);

#ifndef SNOW_SHADOW_CAST
		const float gStep = 4.0;
		float dXP = saturate(SampleDeformationSmooth(gridLocal + float2(gStep, 0.0)));
		float dXN = saturate(SampleDeformationSmooth(gridLocal - float2(gStep, 0.0)));
		float dYP = saturate(SampleDeformationSmooth(gridLocal + float2(0.0, gStep)));
		float dYN = saturate(SampleDeformationSmooth(gridLocal - float2(0.0, gStep)));
		float4 blur = float4(dXP, dXN, dYP, dYN);
		const float3 patchLook = TrackCarveLook(patchAge);
		[branch] if (CompactLook.w > 0.0 || patchLook.x > 0.0)
		{
			blur = float4(BermFieldPair(gridLocal + float2(gStep, 0.0)).y, BermFieldPair(gridLocal - float2(gStep, 0.0)).y,
				BermFieldPair(gridLocal + float2(0.0, gStep)).y, BermFieldPair(gridLocal - float2(0.0, gStep)).y);
		}
		float profDepth = skinDepth;
		[branch] if (ObjectMeetBand > 0.0 && camDist < 2048.0)
		{
			float landDepth;
			const float sheetZ = SheetSurfaceZ(gridLocal, v.WorldAbs.z - ObjectMeetBand * 0.5, landDepth, false, patchAge, patchBermScale);
			const float meet = (1.0 - smoothstep(0.0, ObjectMeetBand * 0.5, abs(v.WorldAbs.z - sheetZ))) * (1.0 - smoothstep(1024.0, 2048.0, camDist));
			[flatten] if (sheetZ > -50000.0)
				profDepth = lerp(skinDepth, landDepth, 0.5 * meet);
		}
		float2 profGrad = float2(
							  CarveProfile(dXP, blur.x, profDepth, worldXY + float2(gStep, 0.0), patchLook) - CarveProfile(dXN, blur.y, profDepth, worldXY - float2(gStep, 0.0), patchLook),
							  CarveProfile(dYP, blur.z, profDepth, worldXY + float2(0.0, gStep), patchLook) - CarveProfile(dYN, blur.w, profDepth, worldXY - float2(0.0, gStep), patchLook)) /
		                  (2.0 * gStep);
		float2 undGrad = float2(0.0, 0.0);
		float undScale = saturate(depth / 8.0);
		[branch] if (undScale > 0.001)
		{
			undGrad = UndulationGradSampled(worldXY) * undScale;
		}
		float2 bermGrad = float2(0.0, 0.0);
		[branch] if (BermHeightAmp > 0.005 && bermD > 0.003)
		{
			const float bStep = BermGradStep();
			float mXP = dXP, mXN = dXN, mYP = dYP, mYN = dYN;
			[branch] if (bStep < gStep - 0.01)
			{
				mXP = saturate(SampleDeformationSmooth(gridLocal + float2(bStep, 0.0)));
				mXN = saturate(SampleDeformationSmooth(gridLocal - float2(bStep, 0.0)));
				mYP = saturate(SampleDeformationSmooth(gridLocal + float2(0.0, bStep)));
				mYN = saturate(SampleDeformationSmooth(gridLocal - float2(0.0, bStep)));
			}
			bermGrad = float2(
				BermShape(BermField(gridLocal + float2(bStep, 0.0))) * (1.0 - mXP) - BermShape(BermField(gridLocal - float2(bStep, 0.0))) * (1.0 - mXN),
				BermShape(BermField(gridLocal + float2(0.0, bStep))) * (1.0 - mYP) - BermShape(BermField(gridLocal - float2(0.0, bStep))) * (1.0 - mYN)) / (2.0 * bStep) *
			          profDepth * BermHeightAmp * BermDepthGate(profDepth) * patchBermScale;
		}
		float2 topGrad = float2(0.0, 0.0);
		{
			const float tStep = 16.0;
			float tXP = PatchSurfaceTop(worldXY + float2(tStep, 0.0));
			float tXN = PatchSurfaceTop(worldXY - float2(tStep, 0.0));
			float tYP = PatchSurfaceTop(worldXY + float2(0.0, tStep));
			float tYN = PatchSurfaceTop(worldXY - float2(0.0, tStep));
			topGrad = float2(PatchTopSlope(tXP, tXN, top, tStep), PatchTopSlope(tYP, tYN, top, tStep));
			topGrad /= max(1.0, length(topGrad));
		}
		v.NormalWS = normalize(float3(-topGrad - profGrad - undGrad - bermGrad, 1.0));
#endif
		v.SkinDepth = skinDepth;
		v.Deform = deform;
		v.Killed = 0.0;
#ifdef SNOW_SHADOW_CAST
		[flatten] if (1.0 - smoothstep(2800.0, 4900.0, length(worldXY - WorldRow0.xy)) < 0.35)
			v.Killed = 1.0;
#endif
	}

	return v;
}

VS_OUTPUT FinishPatchVertex(PatchVertex v)
{
	VS_OUTPUT vsout;
	vsout.CurrentClip = float4(0.0, 0.0, 0.0, 1.0);
	vsout.PreviousClip = float4(0.0, 0.0, 0.0, 1.0);
	vsout.WorldPos = float3(0.0, 0.0, 0.0);
	vsout.NormalWS = v.NormalWS;
	vsout.GridLocal = v.GridLocal;
	vsout.Coverage = 1.0;
	vsout.Flat = 0.0;
	vsout.Lift = 1e6;
	vsout.LiftTarget = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
	vsout.ProjFactor = 1.0;
	[branch] if (v.Killed > 0.5)
	{
		float nan = asfloat(0x7fc00000);
		vsout.Position = float4(nan, nan, nan, nan);
	}
	else
	{
		float3 rel = v.WorldAbs - ShellCameraPosAdjust.xyz;
		vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
#ifndef SNOW_SHADOW_CAST
		float3 prevRel = v.WorldAbs - ShellCameraPreviousPosAdjust.xyz;
		vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
		vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
		vsout.WorldPos = rel;
#endif
	}
	return vsout;
}
#endif

#if ((defined(VSHADER) && defined(SNOW_TESS)) || defined(HULLSHADER)) && defined(PATCH)
cbuffer PatchCullCB : register(b11)
{
	float4 PatchCullLines[6];
	uint PatchCullLineCount;
	uint PatchRowStart;
	uint PatchColStart;
	uint patchCullPad;
}
#endif

#if (defined(HULLSHADER) || defined(DOMAINSHADER) || defined(SNOW_TESS)) && defined(PATCH)
struct TessControlPointPatch
{
	float2 WorldXY : TEXCOORD0;
};

struct TessControlPointPatchAged
{
	float2 WorldXY : TEXCOORD0;
	float2 CornerAge : TEXCOORD1;
};

struct TessFactorsPatch
{
	float Edge[4] : SV_TessFactor;
	float Inside[2] : SV_InsideTessFactor;
};
#endif

#if defined(VSHADER) && defined(PATCH) && !defined(SNOW_TESS)
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
#	ifdef SNOW_PATCH_INDEXED
	const uint latticeStride = kPatchGridDim + 1;
	float2 gridXY = float2(vertexID % latticeStride, vertexID / latticeStride);
#	else
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 6;
	float2 gridXY = float2(quadIndex % kPatchGridDim, quadIndex / kPatchGridDim) + kCorners[vertexID % 6];
#	endif
	float2 worldXY = WorldRow0.xy + PatchWarpXY(gridXY - kPatchGridDim * 0.5);
	return FinishPatchVertex(BuildPatchVertex(worldXY, false, float2(0.0, 1.0)));
}
#elif defined(VSHADER) && defined(PATCH)
TessControlPointPatch main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	static const float2 kPatchCorners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = (PatchRowStart + instanceID) * kPatchGridDim + PatchColStart + vertexID / 4;
	float2 gridXY = float2(quadIndex % kPatchGridDim, quadIndex / kPatchGridDim) + kPatchCorners[vertexID % 4];
	TessControlPointPatch cp;
	cp.WorldXY = WorldRow0.xy + PatchWarpXY(gridXY - kPatchGridDim * 0.5);
	return cp;
}
#endif

#if defined(PSHADER) && defined(SNOW_COAT_PARALLAX)
Texture2D<float4> ObjectParallaxMap : register(t59);
static bool gObjectParallaxMarch = false;
float ObjectParallaxHeight(float2 uv, float mip)
{
	const float4 h = ObjectParallaxMap.SampleLevel(SnowSampler, uv, mip);
	return ObjParallax.y > 1.5 ? h.w : h.x;
}
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#if defined(PSHADER) && defined(SNOW_COAT_PARALLAX)
#	include "SnowDeformation/SnowParallaxFollow.hlsli"
float3 ObjectParallaxShift(VS_OUTPUT input)
{
	float3 shift = 0.0;
	[branch] if (ObjParallax.x > 0.0)
	{
		const float2 uv = input.ObjUV;
		const float2 uvDx = ddx_fine(uv);
		const float2 uvDy = ddy_fine(uv);
		const float3 posDx = ddx_fine(input.WorldPos);
		const float3 posDy = ddy_fine(input.WorldPos);
		const float3 viewDirection = -normalize(input.WorldPos);
		const float3x3 tbnTr = float3x3(normalize(input.ObjT), normalize(input.ObjB), normalize(input.ObjN));
		DisplacementParams params;
		params.DisplacementScale = 1.0;
		params.DisplacementOffset = 0.0;
		params.HeightScale = ObjParallax.x;
		[flatten] if (SharedData::snowDeformationSettings.ParallaxFlatten > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 && ProjThreshold > -0.9)
			params.HeightScale *= 1.0 - smoothstep(-0.1, ProjNoiseScale + 0.1, tbnTr[2].z * input.ProjFactor - max(ProjThreshold, 0.0));
		params.FlattenAmount = 0.0;
		[branch] if (SharedData::extendedMaterialSettings.EnableParallaxWarpingFix)
			params.FlattenAmount = ExtendedMaterials::GetObjectFlattenAmount(tbnTr[2], input.WorldPos, viewDirection);
		float2 dims;
		ObjectParallaxMap.GetDimensions(dims.x, dims.y);
		const float mip = max(ExtendedMaterials::GetMipLevelFromDims(uv, dims * ObjParallax.z) - ObjParallax.w, 0.0);
		bool march = true;
		[branch] if (ObjParallax.y > 1.5)
			march = ExtendedMaterials::IsComplexMaterial(ObjectParallaxMap.SampleLevel(SnowSampler, uv, 15)) &&
			        ExtendedMaterials::HasComplexHeight(ObjectParallaxMap.Sample(SnowSampler, uv).w);
		[branch] if (march)
		{
			gObjectParallaxMarch = true;
			float pixelOffset;
			const float2 parallaxUV = ExtendedMaterials::GetParallaxCoords(input.CurrentClip.w, uv, mip, viewDirection, tbnTr, 0.0,
				ObjectParallaxMap, SnowSampler, 0, params, false, (StochasticOffsets)0, pixelOffset);
			gObjectParallaxMarch = false;
			shift = SnowParallaxFollow::UVShiftToWorld(parallaxUV - uv, uvDx, uvDy, posDx, posDy);
		}
	}
	return shift;
}
#endif

#if defined(HULLSHADER) && defined(PATCH)
bool PatchOutsideFootprint(float2 a, float2 b, float2 c, float2 d)
{
	float2 lo = min(min(a, b), min(c, d)) - 16.0;
	float2 hi = max(max(a, b), max(c, d)) + 16.0;
	bool outside = false;
	[unroll] for (uint i = 0; i < 6; i++)
	{
		float4 l = PatchCullLines[i];
		float2 pv = float2(l.x > 0.0 ? hi.x : lo.x, l.y > 0.0 ? hi.y : lo.y);
		if (i < PatchCullLineCount && dot(l.xy, pv) + l.z < 0.0)
			outside = true;
	}
	return outside;
}

static const float kPatchEdgeTargetScale = 0.25;

float PatchEdgeTessFactor(float2 worldA, float2 worldB, float zA, float zB, bool screenCap)
{
	float2 mid = 0.5 * (worldA + worldB);
	float dist = length(mid - ShellCameraPosAdjust.xy);
	float deform = max(max(SampleDeformation(worldA - GridOrigin), SampleDeformation(worldB - GridOrigin)), SampleDeformation(mid - GridOrigin));
	float reliefBase = SnowReliefDepth > 0.01 ? 1.0 : 0.0;
	float reach = 1600.0 * lerp(reliefBase, 3.0, smoothstep(0.02, 0.25, deform));
	float factor = clamp(reach / max(dist, 32.0), 1.0, 8.0);
	float quadFactor = clamp(length(worldA - worldB) / 16.0, 1.0, 16.0);
	[branch] if (screenCap && max(factor, quadFactor) > 1.0)
	{
		float4 clipA = mul(CameraViewProj, float4(float3(worldA, zA) - ShellCameraPosAdjust.xyz, 1.0));
		float4 clipB = mul(CameraViewProj, float4(float3(worldB, zB) - ShellCameraPosAdjust.xyz, 1.0));
		[flatten] if (min(clipA.w, clipB.w) > 16.0)
		{
			float edgePx = length((clipA.xy / clipA.w - clipB.xy / clipB.w) * TessScreen.yz);
			factor = min(factor, max(edgePx / TessScreen.x, 1.0));
			quadFactor = min(quadFactor, max(edgePx / (TessScreen.x * kPatchEdgeTargetScale), 1.0));
		}
	}
	return max(factor, quadFactor);
}

TessFactorsPatch PatchConstants(InputPatch<TessControlPointPatch, 4> patch)
{
	TessFactorsPatch f;
	bool anyLive = false;
	float cornerZ[4] = { -1000000.0, -1000000.0, -1000000.0, -1000000.0 };
	[branch] if (!PatchOutsideFootprint(patch[0].WorldXY, patch[1].WorldXY, patch[2].WorldXY, patch[3].WorldXY))
	{
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float2 w = patch[i].WorldXY;
			cornerZ[i] = PatchTop(w);
			if (cornerZ[i] > -50000.0 && PatchSkinDepth(w).x >= 1.0)
				anyLive = true;
		}
		float2 centre = 0.25 * (patch[0].WorldXY + patch[1].WorldXY + patch[2].WorldXY + patch[3].WorldXY);
		if (PatchTop(centre) > -50000.0 && PatchSkinDepth(centre).x >= 1.0)
			anyLive = true;
	}
	if (!anyLive) {
		f.Edge[0] = f.Edge[1] = f.Edge[2] = f.Edge[3] = 0.0;
		f.Inside[0] = f.Inside[1] = 0.0;
		return f;
	}
	const bool screenCap = TessScreen.x > 0.0;
	[branch] if (screenCap)
	{
		[unroll] for (uint c = 0; c < 4; c++)
		{
			[branch] if (cornerZ[c] <= -50000.0)
				cornerZ[c] = SampleTerrainStatics(patch[c].WorldXY - GridOrigin).x;
		}
	}
	f.Edge[0] = PatchEdgeTessFactor(patch[0].WorldXY, patch[3].WorldXY, cornerZ[0], cornerZ[3], screenCap);
	f.Edge[1] = PatchEdgeTessFactor(patch[0].WorldXY, patch[1].WorldXY, cornerZ[0], cornerZ[1], screenCap);
	f.Edge[2] = PatchEdgeTessFactor(patch[1].WorldXY, patch[2].WorldXY, cornerZ[1], cornerZ[2], screenCap);
	f.Edge[3] = PatchEdgeTessFactor(patch[3].WorldXY, patch[2].WorldXY, cornerZ[3], cornerZ[2], screenCap);
	f.Inside[0] = max(f.Edge[1], f.Edge[3]);
	f.Inside[1] = max(f.Edge[0], f.Edge[2]);
	return f;
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstants")]
[maxtessfactor(16.0)]
TessControlPointPatchAged main(InputPatch<TessControlPointPatch, 4> patch, uint i : SV_OutputControlPointID)
{
	TessControlPointPatchAged cp;
	cp.WorldXY = patch[i].WorldXY;
	cp.CornerAge = float2(0.0, 1.0);
	[branch] if (TrackAge.y > 0.0)
	{
		const float2 dates = BermFieldPair(patch[i].WorldXY - GridOrigin).zw;
		cp.CornerAge = float2(TrackAgeOf(dates.x), TrackBermScale(TrackAgeOf(dates.y)));
	}
	return cp;
}
#endif

#if defined(DOMAINSHADER) && defined(PATCH)
[domain("quad")]
VS_OUTPUT main(TessFactorsPatch factors, float2 domainUV : SV_DomainLocation, const OutputPatch<TessControlPointPatchAged, 4> patch)
{
	float2 worldXY = lerp(
		lerp(patch[0].WorldXY, patch[1].WorldXY, domainUV.x),
		lerp(patch[3].WorldXY, patch[2].WorldXY, domainUV.x), domainUV.y);
	const float2 trackAge = lerp(
		lerp(patch[0].CornerAge, patch[1].CornerAge, domainUV.x),
		lerp(patch[3].CornerAge, patch[2].CornerAge, domainUV.x), domainUV.y);
	PatchVertex v = BuildPatchVertex(worldXY, true, trackAge);

	[branch] if (v.Killed < 0.5 && HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float camDist = length(v.WorldAbs - ShellCameraPosAdjust.xyz);
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		[branch] if (reliefFade > 0.001)
		{
			float2 snowUV = (SnowUVOffset + v.GridLocal) / kSnowUVTile;
			float mip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);
			float h = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, v.WorldAbs.xy), 0.0.xx, mip);
			v.WorldAbs.z += (h - 0.5) * SnowReliefDepth * reliefFade * saturate(v.SkinDepth / 6.0) * (1.0 - v.Deform);
		}
	}

	return FinishPatchVertex(v);
}
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && !defined(PATCH)
#endif

#if (defined(VSHADER) || defined(DOMAINSHADER)) && !defined(PATCH)
struct SkinLift
{
	float3 WorldAbs;
	float Depth;
	float CoverDepth;
	float RimT;
	float Support;
	float UpFacing;
	float ProjFactor;
	float ProjLinear;
	float Target;
	float3 ShadeNormal;
};

SkinLift ApplySkinLift(float3 worldBase, float3 nrmWS, float3 smoothWS, float isFlat, float vertexAlpha)
{
	float3 liftWS = float3(0.0, 0.0, 1.0);
	[flatten] if (LegacySkin > 0.5 && isFlat < 0.5)
		liftWS = smoothWS;
	float depthBase = max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift);
	float depthTarget = depthBase;

	float weldNz = nrmWS.z;
	float upFacing = isFlat > 0.5 ? smoothstep(0.4, 0.7, weldNz) : smoothstep(0.05, 0.85, smoothWS.z);
	float projFactor = 1.0;
	float projLinear = 1.0;
	float3 shadeNormal = nrmWS;
	[branch] if (ProjPixelEnable > 1.5)
	{
		projLinear = vertexAlpha;
		upFacing = 0.0;
	}
	else [branch] if (ProjThreshold > -0.9)
	{
		float projWeight = nrmWS.z * vertexAlpha - max(ProjThreshold, 0.0);
		projFactor = saturate(projWeight) * smoothstep(0.06, 0.16, projWeight);
		upFacing *= projFactor;
	}
	[flatten] if (FullCoat > 0.5)
		upFacing = 1.0;
	float depth = depthBase * upFacing;

	float support = 1.0;
	float rimT = 1.0;
	[branch] if (HasObjectTop > 0.5 && LegacySkin < 0.5 && depth > 0.001 &&
				 (ProjPixelEnable < 1.5 || depthTarget > kShelterDust))
	{
		float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
		rimT = saturate(ObjectConeDepth(worldBase.xy) / coneSeed);
		float rollFrac = saturate(kCorniceRoll / coneSeed);
		float u = saturate(rimT / max(rollFrac, 1e-3));
		float toRim = 1.0 - u;
		rimT = u;
		float allowed = depthBase * sqrt(saturate(1.0 - toRim * toRim));
		depth = min(depth, allowed);
		support = saturate(allowed / max(depthBase, 0.01));

		float objTop = PatchTop(worldBase.xy);
		[flatten] if (objTop > -50000.0)
		{
			float shelterAmt = smoothstep(kShelterNear, kShelterFar, objTop - worldBase.z);
			depth = lerp(depth, min(depth, kShelterDust), shelterAmt);
			depthTarget = lerp(depthTarget, min(depthTarget, kShelterDust), shelterAmt);
		}
	}

	float coverDepth = depth;

	[branch] if (ProjPixelEnable > 1.5)
	{
		depth = depthBase;
		coverDepth = depth;
		upFacing = 1.0;
	}

	SkinLift o;
	o.WorldAbs = worldBase + liftWS * depth;

	o.Depth = depth;
	o.CoverDepth = coverDepth;
	o.Target = max(depthTarget, kMinSkinLift);
	o.RimT = rimT;
	o.Support = support;
	o.UpFacing = upFacing;
	o.ProjFactor = projFactor;
	o.ProjLinear = projLinear;
	o.ShadeNormal = shadeNormal;
	return o;
}

float3 SkinShadingNormal(float3 nrmWS, float3 smoothWS, float isFlat, float depth, float rimT)
{
	float depthBase = max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift);
	float blend = saturate(depth / max(depthBase, 0.01)) * kSkinShadeSmooth;
	[flatten] if (isFlat > 0.5)
		blend = (1.0 - smoothstep(0.0, 0.35, rimT)) * 0.8;
	return normalize(lerp(nrmWS, smoothWS, blend));
}
#endif

#define SNOW_SKIN_CLIP(vsout, worldAbs)                                               \
	float3 rel = worldAbs - ShellCameraPosAdjust.xyz;                                 \
	float3 prevRel = worldAbs - ShellCameraPreviousPosAdjust.xyz;                     \
	[flatten] if (ProjPixelEnable > 1.5)                                              \
	{                                                                                 \
		float3 toEye = normalize(rel);                                                \
		rel -= toEye * kEdgeFlankLift;                                                \
		prevRel -= toEye * kEdgeFlankLift;                                            \
	}                                                                                 \
	VS_OUTPUT vsout;                                                                  \
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));                           \
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));              \
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0)); \
	vsout.WorldPos = rel

#if (defined(HULLSHADER) || defined(DOMAINSHADER) || defined(SNOW_TESS)) && !defined(PATCH)
struct TessControlPoint
{
	float3 WorldBase : TEXCOORD0;
	float3 NormalWS : TEXCOORD1;
	float3 SmoothWS : TEXCOORD2;
	float Flat : TEXCOORD3;
	float VertexAlpha : TEXCOORD4;
};

struct TessFactors
{
	float Edge[3] : SV_TessFactor;
	float Inside : SV_InsideTessFactor;
};
#endif

#if defined(VSHADER) && !defined(PATCH)
struct SkinVertex
{
	float3 WorldBase;
	float3 NormalWS;
	float3 SmoothWS;
	float Flat;
	float VertexAlpha;
};

SkinVertex BuildSkinVertex(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 nrmMS = input.Normal.xyz * 2.0 - 1.0;

	float3 inflateMS = nrmMS;
	float isFlat = 0.0;
	float vertexAlpha = 1.0;
	[branch] if (HasSmoothedNormals > 0.5)
	{
		float4 flatStats = SmoothedNormals[(uint)VertexCountF];
		[flatten] if (flatStats.w > 0.5 && flatStats.x > 0.5)
			isFlat = 1.0;
		float4 smoothEntry = SmoothedNormals[input.VertexID];
		[flatten] if (smoothEntry.w > 0.5)
		{
			inflateMS = smoothEntry.xyz;
			vertexAlpha = saturate(smoothEntry.w - 1.0);
		}
	}
	[flatten] if (ClassOverride > 1.5)
		isFlat = 1.0;
	else [flatten] if (ClassOverride > 0.5)
		isFlat = 0.0;

	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);
	float3 nrmWS = normalize(float3(
		dot(WorldRow0.xyz, nrmMS),
		dot(WorldRow1.xyz, nrmMS),
		dot(WorldRow2.xyz, nrmMS)));
	float3 smoothWS = normalize(float3(
		dot(WorldRow0.xyz, inflateMS),
		dot(WorldRow1.xyz, inflateMS),
		dot(WorldRow2.xyz, inflateMS)));
	SkinVertex v;
	v.WorldBase = worldAbs;
	v.NormalWS = nrmWS;
	v.SmoothWS = smoothWS;
	v.Flat = isFlat;
	v.VertexAlpha = vertexAlpha;
	return v;
}

#if !defined(SNOW_TESS)
VS_OUTPUT main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	SkinLift lift = ApplySkinLift(v.WorldBase, v.NormalWS, v.SmoothWS, v.Flat, v.VertexAlpha);

	SNOW_SKIN_CLIP(vsout, lift.WorldAbs);
	vsout.NormalWS = ProjPixelEnable > 1.5 ? lift.ShadeNormal : SkinShadingNormal(v.NormalWS, v.SmoothWS, v.Flat, lift.Depth, lift.RimT);
	vsout.Coverage = v.NormalWS.z;
	vsout.Flat = v.Flat;
	vsout.GridLocal = lift.WorldAbs.xy - GridOrigin;
	vsout.Lift = lift.CoverDepth;
	vsout.LiftTarget = lift.Target;
	vsout.ProjFactor = ProjPixelEnable > 1.5 ? lift.ProjLinear : lift.ProjFactor;
#	ifdef SNOW_COAT_PARALLAX
	vsout.ObjUV = input.TexCoord0 * ObjParallaxUV.zw + ObjParallaxUV.xy;
	const float3x3 world3 = float3x3(WorldRow0.xyz, WorldRow1.xyz, WorldRow2.xyz);
	vsout.ObjT = normalize(mul(world3, float3(input.Position.w, input.Normal.w * 2.0 - 1.0, input.Bitangent.w * 2.0 - 1.0)));
	vsout.ObjB = normalize(mul(world3, input.Bitangent.xyz * 2.0 - 1.0));
	vsout.ObjN = normalize(mul(world3, input.Normal.xyz * 2.0 - 1.0));
#	endif
	return vsout;
}
#else
TessControlPoint main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	TessControlPoint cp;
	cp.WorldBase = v.WorldBase;
	cp.NormalWS = v.NormalWS;
	cp.SmoothWS = v.SmoothWS;
	cp.Flat = v.Flat;
	cp.VertexAlpha = v.VertexAlpha;
	return cp;
}
#endif
#endif

#if defined(HULLSHADER) && !defined(PATCH)
float EdgeTessFactor(float3 worldA, float3 worldB)
{
	float3 mid = 0.5 * (worldA + worldB);
	float dist = length(mid - ShellCameraPosAdjust.xyz);
	float targetLen = max(4.0, dist * 0.01);
	[branch] if (HasObjectTop > 0.5)
	{
		float coneNear = min(ObjectConeDepth(mid.xy),
			min(ObjectConeDepth(worldA.xy), ObjectConeDepth(worldB.xy)));
		float rimBoost = (1.0 - smoothstep(4.0, 12.0, coneNear)) *
		                 (1.0 - smoothstep(1200.0, 2400.0, dist));
		targetLen = lerp(targetLen, 1.0, rimBoost);
	}
	targetLen = max(targetLen, dist * SkinTessCapSlope);
	return clamp(length(worldA - worldB) / targetLen, 1.0, 16.0);
}

TessFactors PatchConstants(InputPatch<TessControlPoint, 3> patch)
{
	TessFactors f;
	f.Edge[0] = EdgeTessFactor(patch[1].WorldBase, patch[2].WorldBase);
	f.Edge[1] = EdgeTessFactor(patch[2].WorldBase, patch[0].WorldBase);
	f.Edge[2] = EdgeTessFactor(patch[0].WorldBase, patch[1].WorldBase);
	f.Inside = max(max(f.Edge[0], f.Edge[1]), f.Edge[2]);
	return f;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
[maxtessfactor(16.0)]
TessControlPoint main(InputPatch<TessControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
	return patch[i];
}
#endif

#if defined(DOMAINSHADER) && !defined(PATCH)
[domain("tri")]
VS_OUTPUT main(TessFactors factors, float3 bary : SV_DomainLocation, const OutputPatch<TessControlPoint, 3> patch)
{
	float3 worldBase = patch[0].WorldBase * bary.x + patch[1].WorldBase * bary.y + patch[2].WorldBase * bary.z;
	float3 nSum = patch[0].NormalWS * bary.x + patch[1].NormalWS * bary.y + patch[2].NormalWS * bary.z;
	float3 iSum = patch[0].SmoothWS * bary.x + patch[1].SmoothWS * bary.y + patch[2].SmoothWS * bary.z;
	float interpHealth = min(length(nSum), length(iSum));
	float3 normalWS = nSum / max(length(nSum), 1e-3);
	float3 inflateWS = iSum / max(length(iSum), 1e-3);
	float isFlat = patch[0].Flat * bary.x + patch[1].Flat * bary.y + patch[2].Flat * bary.z;
	float vertexAlpha = patch[0].VertexAlpha * bary.x + patch[1].VertexAlpha * bary.y + patch[2].VertexAlpha * bary.z;

	SkinLift lift = ApplySkinLift(worldBase, normalWS, inflateWS, isFlat, vertexAlpha);
	float3 worldAbs = lift.WorldAbs;
	float2 gridLocal = worldAbs.xy - GridOrigin;
	normalWS = ProjPixelEnable > 1.5 ? lift.ShadeNormal : SkinShadingNormal(normalWS, inflateWS, isFlat, lift.Depth, lift.RimT);

	[branch] if (HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float camDist = length(worldAbs - ShellCameraPosAdjust.xyz);
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		[branch] if (reliefFade > 0.001 && lift.Depth > 0.5)
		{
			float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
			float mip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);
			float h = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, worldAbs.xy), 0.0.xx, mip);
			float carve = saturate(SampleDeformation(gridLocal));
			worldAbs += inflateWS * ((h - 0.5) * SnowReliefDepth * reliefFade * saturate(lift.Depth / 6.0) * (1.0 - carve) * saturate(inflateWS.z) * smoothstep(0.3, 0.7, interpHealth));
		}
	}

	SNOW_SKIN_CLIP(vsout, worldAbs);
	vsout.NormalWS = normalWS;
	float smoothZ = nSum.z / max(length(nSum), 1e-3);
	vsout.Coverage = smoothZ;
	vsout.Flat = isFlat;
	vsout.GridLocal = gridLocal;
	vsout.Lift = lift.CoverDepth;
	vsout.LiftTarget = lift.Target;
	vsout.ProjFactor = ProjPixelEnable > 1.5 ? lift.ProjLinear : lift.ProjFactor;
	return vsout;
}
#endif

#undef SNOW_SKIN_CLIP

#ifdef PSHADER

struct PS_OUTPUT
{
	SNOW_GBUFFER_TARGETS
#	if !defined(PATCH) && !defined(SNOW_STATICS_NO_DEPTH_EXPORT)
	float Depth : SV_Depth;
#	endif
};

#ifdef SNOW_PATCH_ATTR
struct PATCH_ATTR_OUTPUT
{
	float4 Grid : SV_Target0;
	float4 Deriv : SV_Target1;
	float4 Depth : SV_Target2;
	float Mark : SV_Target3;
};
#endif

float CoverageNoise(float2 worldXY)
{
	float2 c = worldXY / 24.0;
	float2 i = floor(c);
	float2 f = frac(c);
	f = f * f * (3.0 - 2.0 * f);
	float n00 = StochasticHash(i).x;
	float n10 = StochasticHash(i + float2(1, 0)).x;
	float n01 = StochasticHash(i + float2(0, 1)).x;
	float n11 = StochasticHash(i + float2(1, 1)).x;
	return lerp(lerp(n00, n10, f.x), lerp(n01, n11, f.x), f.y);
}

Texture2D<float> SceneDepth : register(t3);
Texture2D<float> PreSkinMainDepth : register(t43);

struct SkinShadeInput
{
	float3 WorldPos;
	float4 Position;
	float4 CurrentClip;
	float Flat;
	float2 GridLocal;
	float2 trenchGridLocal;
	float2 worldXY;
	float pixelDist;
	float pixelDeform;
	float screenNoise;
	float selfShadowReject;
	float onObject;
	float detail;
	float pom;
	float geoNz;
	float markBase;
#ifdef SNOW_COAT_PARALLAX
	float3 surfaceShift;
#endif
};
struct SkinShadeResult
{
	float3 normalWS;
	float3 viewNormal;
	float3 ambientPart;
	float3 diffuseLobe;
	float3 directSpecular;
	float3 specularLobe;
	float3 preLit;
	float landVertexAO;
	float snowRoughness;
};

SkinShadeResult SkinShadeSurface(SkinShadeInput input, float3 normalWS)
{
	float3 shadePos = input.WorldPos;
#ifdef SNOW_OBJECT_PRINTS
	const float3 printAt = float3(input.worldXY, input.WorldPos.z + ShellCameraPosAdjust.z);
	const float3 print = SnowPrints::DepthSlope(printAt);
	const float printRim = SnowPrints::Rim(printAt) / max(SharedData::snowDeformationSettings.PrintFullDepth, 0.001);
	const float printTop = smoothstep(0.5, 0.75, input.geoNz);
#endif
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
#	ifdef SNOW_PATCH_DEFERRED
	const float detailLod = TrenchDetailLod(gDeferredGridDx, gDeferredGridDy);
#	else
	const float detailLod = TrenchDetailLod(ddx(input.worldXY), ddy(input.worldXY));
#	endif
	const float detailFade = 1.0 - smoothstep(1200.0, 2000.0, input.CurrentClip.w);
	float detailMask = 0.0;
	float detailRelief = 0.0;
	TrenchDetailTaps trenchTaps = (TrenchDetailTaps)0;
	float detailHitH = 0.0;
	float detailImpact = 0.0;
	const float2 detailPlane = -normalWS.xy / max(normalWS.z, 0.1);
	[branch] if (detailFade > 0.0 && (TrenchDetailRelief > 0.001 || abs(TrenchDetailRoughness - 1.0) > 0.001))
	{
#	ifdef PATCH
		const float4 patchField = BermFieldPair(input.trenchGridLocal);
		detailMask = TrenchDetailMask(input.pixelDeform, patchField.xy, TrenchDetailCoverage) * detailFade;
		float2 marks = 0.0;
		[branch] if (SpeedSpoil > 0.5)
		{
			marks = SampleDetailMarks(input.trenchGridLocal);
			detailMask = max(detailMask, TrenchSpoilMask(marks.y) * detailFade);
		}
#	else
		const float printRel = print.x / max(SharedData::snowDeformationSettings.PrintFullDepth, 0.001);
		detailMask = TrenchDetailMask(printRel, float2(0.0, max(printRel, printRim)), TrenchDetailCoverage) * detailFade * printTop;
#	endif
		[branch] if (detailMask > 0.001)
		{
#	ifdef PATCH
			[branch] if (SpeedSpoil < 0.5)
				marks = SampleDetailMarks(input.trenchGridLocal);
			const float meltWet = smoothstep(kSpellMarkStart, 0.15, marks.x);
			const float detailAge = lerp(TrackAgeOf(patchField.w), TrackAgeOf(patchField.z), saturate(10.0 * input.pixelDeform));
			const float impact = marks.y * TrackDetailImpactScale(detailAge);
			detailImpact = max(meltWet, impact);
			detailRelief = TrenchDetailRelief * detailMask * max(saturate(PatchSkinDepth(input.worldXY).x / 10.0), meltWet) * TrampleDetailScale(input.worldXY, input.pixelDeform) *
			               (1.0 + kImpactReliefBoost * impact) * TrackDetailReliefScale(detailAge);
#	else
			detailRelief = 2.0 * TrenchDetailRelief * detailMask * max(saturate(kPrintDentDepth / 10.0), saturate(printRim));
#	endif
			trenchTaps = TrenchDetailTapsAt(input.worldXY);
		}
	}
	[branch] if (detailRelief > 0.05)
	{
		float3 dir = normalize(input.WorldPos);
		const float4 duv = TrenchDetailStep(trenchTaps, dir.xy);
		float t = TrenchDetailParallaxT(dir, detailPlane, trenchTaps, duv,
			float3(detailRelief, detailLod, lerp(0.6, 2.0, saturate(input.CurrentClip.w / 1500.0))), detailImpact, input.screenNoise);
		detailHitH = (dir.z - dot(dir.xy, detailPlane)) * t;
		shadePos = input.WorldPos + dir * t;
		input.worldXY += dir.xy * t;
		input.trenchGridLocal += dir.xy * t;
		input.GridLocal += dir.xy * t;
		trenchTaps.uv += duv * t;
	}
#endif
	const float2 trenchGridLocal = input.trenchGridLocal;
	const float2 worldXY = input.worldXY;
	const float pixelDist = input.pixelDist;
	const float pixelDeform = input.pixelDeform;
	const float screenNoise = input.screenNoise;
#ifdef PATCH
	const float patchMelt = saturate(SampleExclusionField(worldXY).y);
	const float iceParallax = SnowIceParallax(pixelDeform, patchMelt, input.WorldPos.z + ShellCameraPosAdjust.z, trenchGridLocal, true);
#elif defined(SNOW_OBJECT_PRINTS)
	const float iceParallax = SnowIceParallax(print.x, 0.0, input.WorldPos.z + ShellCameraPosAdjust.z, 0.0, false);
#else
	const float iceParallax = 1.0;
#endif
	float2 snowUV = (SnowUVOffset + trenchGridLocal) / kSnowUVTile;
#ifdef PATCH
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
#else
	float nzPlane = abs(normalWS.z);
	[flatten] if (input.geoNz >= 0.0)
		nzPlane = clamp(nzPlane, input.geoNz - 0.3, input.geoNz + 0.3);
	float snowSteepness = smoothstep(0.75, 0.55, nzPlane);
#endif
	float snowWorldZAbs = shadePos.z + ShellCameraPosAdjust.z;
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXY.y, snowWorldZAbs) : float2(worldXY.x, snowWorldZAbs);
	float2 snowUVSideUnfolded = snowSidePlane / kSnowUVTile;
	float2 snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, pixelDist);
#ifdef SNOW_COAT_PARALLAX
	const float4 snowUVDeriv = float4(ddx(snowUV), ddy(snowUV));
	const float4 snowUVSideDeriv = float4(ddx(snowUVSideUnfolded), ddy(snowUVSideUnfolded));
	const float2 sideShift = snowSideDropsX ? input.surfaceShift.yz : input.surfaceShift.xz;
	snowUV += input.surfaceShift.xy / kSnowUVTile;
	snowSidePlane += sideShift;
	snowUVSideUnfolded += sideShift / kSnowUVTile;
	snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	shadePos += input.surfaceShift;
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXY + input.surfaceShift.xy);
	snowTaps.duvdx = snowUVDeriv.xy;
	snowTaps.duvdy = snowUVDeriv.zw;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = snowUVSideDeriv.xy;
	snowTapsSide.duvdy = snowUVSideDeriv.zw;
	float snowHeightMip = SnowHeightMipGrad(snowUVDeriv.xy, snowUVDeriv.zw);
	float snowHeightMipSide = SnowHeightMipGrad(snowUVSideDeriv.xy, snowUVSideDeriv.zw);
#elif defined(SNOW_PATCH_DEFERRED)
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXY);
	snowTaps.duvdx = gDeferredGridDx / kSnowUVTile;
	snowTaps.duvdy = gDeferredGridDy / kSnowUVTile;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = (snowSideDropsX ? float2(gDeferredGridDx.y, gDeferredZDx) : float2(gDeferredGridDx.x, gDeferredZDx)) / kSnowUVTile;
	snowTapsSide.duvdy = (snowSideDropsX ? float2(gDeferredGridDy.y, gDeferredZDy) : float2(gDeferredGridDy.x, gDeferredZDy)) / kSnowUVTile;
	float snowHeightMip = SnowHeightMipGrad(snowTaps.duvdx, snowTaps.duvdy);
	float snowHeightMipSide = SnowHeightMipGrad(snowTapsSide.duvdx, snowTapsSide.duvdy);
#else
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXY);
	SnowTaps snowTapsSide = ComputeSnowTaps(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = ddx(snowUVSideUnfolded);
	snowTapsSide.duvdy = ddy(snowUVSideUnfolded);
	float snowHeightMip = SnowHeightMip(snowUV);
	float snowHeightMipSide = SnowHeightMip(snowUVSideUnfolded);
#endif

	float4 bermField = 0.0;
	[branch] if (BermHeightAmp > 0.005)
		bermField = BermFieldPair(trenchGridLocal);
	const float bermC = bermField.x;
#ifdef PATCH
	[branch] if (false)
#else
	[branch] if (BermHeightAmp > 0.005 && bermC > 0.003 && input.detail > 0.001)
#endif
	{
		const float bStep = BermGradStep();
		float2 bermGrad = float2(
			BermShape(BermField(trenchGridLocal + float2(bStep, 0.0))) - BermShape(BermField(trenchGridLocal - float2(bStep, 0.0))),
			BermShape(BermField(trenchGridLocal + float2(0.0, bStep))) - BermShape(BermField(trenchGridLocal - float2(0.0, bStep)))) / (2.0 * bStep);
		float bermDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0);
		normalWS = normalize(normalWS + float3(-bermGrad * saturate(1.0 - pixelDeform) * bermDepth * BermHeightAmp * BermDepthGate(bermDepth) * TrackBermScale(TrackAgeOf(bermField.w)), 0.0));
	}

	const float3 frameNormalWS = normalWS;
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	[branch] if (detailRelief > 0.001)
	{
		const float2 g = -normalWS.xy / max(normalWS.z, 0.1) + TrenchDetailGrad(trenchTaps, detailLod, detailImpact) * detailRelief;
		normalWS = normalize(float3(-g, 1.0));
	}
#endif
#ifdef SNOW_OBJECT_PRINTS
	[branch] if (printTop > 0.0 && any(abs(print.yz) > 1e-4))
	{
		const float2 g = -normalWS.xy / max(normalWS.z, 0.1) - kPrintDentDepth * printTop * print.yz;
		normalWS = normalize(float3(-g, 1.0));
	}
	float printLit = 1.0;
	[branch] if (printTop > 0.0 && print.x > 0.005 && SnowParallax.y > 0.001)
		printLit = SnowPrints::DentSunShadow(printAt, SharedData::DirLightDirection.xyz, -frameNormalWS.xy / max(frameNormalWS.z, 0.1), kPrintDentDepth,
			SnowParallax.y * printTop * iceParallax, screenNoise);
	[branch] if (detailRelief > 0.05 && TrenchDetailShadow > 0.001 && SnowParallax.y > 0.001)
		printLit *= lerp(1.0, TrenchDetailSunShadow(trenchTaps, detailHitH, SharedData::DirLightDirection.xyz, detailPlane, float3(detailRelief, detailLod, SnowParallax.y * iceParallax), detailImpact, screenNoise),
			min(TrenchDetailShadow, SnowParallax.z * iceParallax));
#endif

	SNOW_TANGENT_FRAME(bumpT, bumpB, frameNormalWS);

	float3 V = -normalize(input.WorldPos);

	const bool glintSide = snowSteepness > 0.5;
	const float2 glintDuvdx = glintSide ? snowTapsSide.duvdx : snowTaps.duvdx;
	const float2 glintDuvdy = glintSide ? snowTapsSide.duvdy : snowTaps.duvdy;

	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001 && input.detail > 0.001 && input.pom > 0.5)
	{
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z * iceParallax;

		float3x3 tbnTop = float3x3(bumpT, bumpB, frameNormalWS);
		float2 offsetTop = SnowParallaxOffset(snowTaps, snowUV, V, tbnTop, pixelDist, snowHeightMip, screenNoise, pomParams);
		offsetTop *= 1.0 - snowSteepness;
		snowUV += offsetTop;
		snowTaps = OffsetSnowTaps(snowTaps, offsetTop);

		[branch] if (snowSteepness > 0.001)
		{
			float3 sideT = snowSideDropsX ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
			float3 sideB = float3(0.0, 0.0, 1.0);
			float3 sideN = normalize(snowSideDropsX ? float3(frameNormalWS.x, 0.0, 0.0) : float3(0.0, frameNormalWS.y, 0.0));
			sideN = dot(V, sideN) < 0.0 ? -sideN : sideN;
			float3x3 tbnSide = float3x3(sideT, sideB, sideN);
			float2 offsetSide = SnowParallaxOffset(snowTapsSide, snowUVSide, V, tbnSide, pixelDist, snowHeightMipSide, screenNoise, pomParams);
			snowUVSide += offsetSide;
			snowTapsSide = OffsetSnowTaps(snowTapsSide, offsetSide);
		}
	}

	[branch] if (HasSnowNormal > 0.5 && bumpFade > 0.001)
		normalWS = normalize(normalWS + bumpFade * SampleSnowNormalOffset(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness, normalWS,
										   bumpT, bumpB, snowSideDropsX));
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
	{
		const float kBumpTile = 64.0;
		const float kBumpHeight = 0.55;
		float2 texDims;
		SnowDiffuse.GetDimensions(texDims.x, texDims.y);
		float e = 1.5 / texDims.x;
		float2 detailUV = worldXY / kBumpTile;
		const float3 kLum = float3(0.30, 0.45, 0.25);
#ifdef SNOW_PATCH_DEFERRED
		const float2 detailDx = gDeferredGridDx / kBumpTile;
		const float2 detailDy = gDeferredGridDy / kBumpTile;
		float h0 = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV, detailDx, detailDy).rgb, kLum);
		float hx = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(e, 0.0), detailDx, detailDy).rgb, kLum);
		float hy = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(0.0, e), detailDx, detailDy).rgb, kLum);
#else
		float h0 = dot(SnowDiffuse.Sample(SnowSampler, detailUV).rgb, kLum);
		float hx = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(e, 0.0)).rgb, kLum);
		float hy = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(0.0, e)).rgb, kLum);
#endif
		float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
		normalWS = normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
	}

	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		kSnowAlbedo = SnowColor::SetAlbedo(kSnowAlbedo, SnowTextureIsLinear != 0.0);
	}
	{
		float scorch = SampleScorch(input.GridLocal) * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
#ifdef PATCH
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, pixelDeform, patchMelt, PatchSkinDepth(worldXY).x,
		input.WorldPos.z + ShellCameraPosAdjust.z, input.trenchGridLocal, true);
#elif defined(SNOW_OBJECT_PRINTS)
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, print.x, 0.0, kPrintDentDepth, input.WorldPos.z + ShellCameraPosAdjust.z, 0.0, false);
#else
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, 0.0, 0.0, 100.0, input.WorldPos.z + ShellCameraPosAdjust.z, 0.0, false);
#endif

	static const float kSnowRoughness = 0.6;
	static const float3 kSnowF0 = float3(0.028, 0.028, 0.028);

	float snowRoughness = kSnowRoughness;
	float3 snowF0 = kSnowF0;
	float snowAO = 1.0;
	SNOW_RMAOS(snowRoughness, snowF0, snowAO, snowTaps, snowTapsSide, snowSteepness)
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	snowRoughness = clamp(snowRoughness * lerp(1.0, TrenchDetailRoughness, detailMask), 0.05, 1.0);
#endif

	snowRoughness *= 1.0 - wetLook.x;

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(shadePos, ShellCameraPosAdjust.xyz);
	float farShadowT = smoothstep(6000.0, 15000.0, pixelDist);
	float4 gameShadowMask = float4(-1.0, 0.0, 0.0, 0.0);
	float2 gameMaskDim;
	GameShadowMask.GetDimensions(gameMaskDim.x, gameMaskDim.y);
	[branch] if (input.onObject > 0.5 && gameMaskDim.x > 0.5)
		gameShadowMask = GameShadowMask.Load(int3(input.Position.xy, 0));
	float sunShadow;
	[branch] if (gameShadowMask.x >= 0.0)
	{
		sunShadow = worldShadow * (ShadowSampling::HasDirectionalShadows() ? gameShadowMask.x : 1.0);
	}
	else [branch] if (CrispShadows > 0.5)
	{
		sunShadow = worldShadow * SnowShadow::GetCascadeShadowReject(shadePos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w), L, input.selfShadowReject);
	}
	else
	{
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(shadePos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}
	[branch] if ((ShellFlags.x & 1) != 0 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01 && input.detail > 0.001)
	{
		const float sunLen2D = max(length(L.xy), 1e-4);
		const float sunTan = L.z / sunLen2D;
		const float2 stepDir = L.xy / sunLen2D;
		const float surfZ = shadePos.z + ShellCameraPosAdjust.z;
		const bool nearCache = MarchCacheCovers(input.GridLocal, stepDir, kSunMarchDist[4]);
		[branch] if (nearCache || MarchFarCovers(input.GridLocal, stepDir, kSunMarchDist[4]))
		{
			const float h0 = nearCache ? MarchHeightNear(input.GridLocal) : MarchHeightFarAt(input.GridLocal);
			const float refTer = max(h0 > -50000.0 ? h0 : surfZ, surfZ - 2.0);
			const float soft = lerp(0.03, 0.35, farShadowT);
			bool objectTopUsable = nearCache && HasObjectTop > 0.5 && (ShellFlags.y & 1) != 0;
			[branch] if (objectTopUsable)
			{
				float2 selfLocal = (GridOrigin + input.GridLocal - HeightWindowCenter) / HeightHalfExtent;
				[flatten] if (all(abs(selfLocal) < 0.98))
				{
					float2 topDims;
					ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
					float2 selfUV = float2(selfLocal.x * 0.5 + 0.5, 0.5 - selfLocal.y * 0.5);
					float selfTop = ObjectTopRaw.Load(int3((int2)clamp(selfUV * topDims, 0.0, topDims - 1.0), 0));
					if (selfTop > -50000.0 && selfTop > surfZ + 6.0)
						objectTopUsable = false;
				}
			}
			float marchLight;
			[branch] if (nearCache)
			{
				float2 horizon = MarchNearHorizon(input.GridLocal, stepDir, sunTan, refTer);
				[branch] if (objectTopUsable)
				{
					[unroll] for (uint marchI = 0; marchI < 3; marchI++)
					{
						const float d = kSunMarchDist[marchI];
						const float2 sampleLocal = input.GridLocal + stepDir * d;
						const float2 topLocal = (GridOrigin + sampleLocal - HeightWindowCenter) / HeightHalfExtent;
						float2 fRoad;
						const float4 road = MarchCacheTexels(MarchHeightRoad, sampleLocal, fRoad);
						[flatten] if (all(abs(topLocal) < 0.98) && all(road > -50000.0))
						{
							horizon.x = max(horizon.x, (MarchCacheBilinear(road, fRoad) - surfZ - 1.0) / d);
						}
					}
				}
				marchLight = MarchNearLight(horizon, sunTan, soft, lerp(2.0, 8.0, farShadowT));
			}
			else
			{
				float h[6];
				MarchCachedHeights(input.GridLocal, stepDir, false, h);
				float horizonTan = -10.0;
				[unroll] for (uint marchI = 0; marchI < 5; marchI++)
					horizonTan = max(horizonTan, (h[marchI + 1] - refTer - 1.0) / kSunMarchDist[marchI]);
				marchLight = smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan);
			}
			float marchFactor = lerp(marchLight, 1.0, 0.7 * farShadowT);
			marchFactor = lerp(1.0, marchFactor, smoothstep(0.04, 0.12, sunTan));
			sunShadow *= marchFactor;
		}
	}
	const bool sssSeesSkin = SkinSssSeen > 0.5;
	[branch] if (ScreenSpaceShadowsActive > 0.5 && sssSeesSkin && !SharedData::InInterior && dot(normalWS, L) >= 0.0)
		sunShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);
	[branch] if (ScreenSpaceShadowsActive > 0.5 && !sssSeesSkin)
	{
		float sceneZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
		float shellZ = input.CurrentClip.w;
		float sssRayGap = sceneZ - shellZ;
		float sssVertGap = abs(input.WorldPos.z) * sssRayGap / max(shellZ, 1e-3);
		float sssBlend = (1.0 - smoothstep(8.0, 24.0, sssVertGap)) *
		                 (1.0 - smoothstep(150.0, 400.0, sssRayGap));
		sssBlend *= SnowShadow::GetSssHandoff(shellZ);
		[flatten] if (gameShadowMask.x >= 0.0)
			sssBlend = SharedData::InInterior ? 0.0 : 1.0;
		sunShadow *= lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0), sssBlend);
	}

	[branch] if (HasSnowHeight > 0.5 && SnowParallax.y > 0.001 && bumpFade > 0.001 &&
		sunShadow > 0.01 && satNdotL > 0.001)
	{
		float2 lightUVTop = float2(dot(L, bumpT), dot(L, bumpB));
		float2 lightUVSide = snowSideDropsX ? float2(L.y, L.z) : float2(L.x, L.z);

		float occlusion = SnowParallaxOcclusionPlanar(snowTaps, snowTapsSide, snowSteepness,
			lightUVTop, lightUVSide, snowHeightMip, snowHeightMipSide,
			SnowParallaxQuality(pixelDist), screenNoise, SnowDisplacementParams());

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y * iceParallax);
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}
#ifdef PATCH
	[branch] if (detailRelief > 0.05 && TrenchDetailShadow > 0.001 && SnowParallax.y > 0.001 && sunShadow > 0.01 && satNdotL > 0.001)
		sunShadow *= lerp(1.0, TrenchDetailSunShadow(trenchTaps, detailHitH, L, detailPlane, float3(detailRelief, detailLod, SnowParallax.y * iceParallax), detailImpact, screenNoise),
			min(TrenchDetailShadow, SnowParallax.z * iceParallax));
#endif
#ifdef SNOW_OBJECT_PRINTS
	sunShadow *= printLit;
#endif

	const float2 glintUV = glintSide ? (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile :
	                                   fmod(shadePos.xy + ShellCameraPosAdjust.xy, 4096.0) / kSnowUVTile;
	bool glintsLit = sunShadow != 0.0;
	[branch] if (!glintsLit && PointLightsActive > 0.5)
	{
		float glintViewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 glintClip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 glintClusterUV = glintClip.xy / max(glintClip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		glintsLit = SnowLights::ClusterLightCount(glintClusterUV, glintViewZ) > 0;
	}
	const float glintsHere = glintsLit ? EnableGlints * wetLook.y : 0.0;
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		SnowGlintParams, glintsHere, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
	const float4 markSpec = 0.0;
	SnowSunLighting sunLit = SnowEvaluateSunPBR(snowMtl, normalWS, V, shadePos, ShellCameraPosAdjust.xyz, sunShadow,
		glintUV, glintDuvdx, glintDuvdy, markSpec);
	float3 specularLobe = sunLit.specularLobe;
	float3 diffuseLobe = sunLit.diffuseLobe;
	float3 directDiffuse = sunLit.directDiffuse;
	float3 directSpecular = sunLit.directSpecular;
	float3 markSpecular = sunLit.markSpecular;

	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 clip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 screenUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		SnowLights::AccumulatePointLights(snowMtl, shadePos, shadePos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, screenUV, glintUV, glintDuvdx, glintDuvdy, gameShadowMask, markSpec,
			directDiffuse, directSpecular, markSpecular);
	}

	float3 ambientColor = SnowAmbientColor(normalWS);
	float3 ambientPart = ambientColor * diffuseLobe;
	float2 terrainLocal = (shadePos.xy + ShellCameraPosAdjust.xy) - GridOrigin;
	float landVertexAO = ColorManagement::SRGBToWorking(SampleTerrainVertexAO(terrainLocal));
	SNOW_SKYLIGHTING(ambientPart, diffuseLobe, directDiffuse, directSpecular, landVertexAO, shadePos, normalWS);
	float3 preLit = ambientPart + directDiffuse;

	SkinShadeResult r;
	r.normalWS = normalWS;
	r.viewNormal = viewNormal;
	r.ambientPart = ambientPart;
	r.diffuseLobe = diffuseLobe;
	r.directSpecular = directSpecular;
	r.specularLobe = specularLobe;
	r.preLit = preLit;
	r.landVertexAO = landVertexAO;
	r.snowRoughness = snowRoughness;
	return r;
}

#if !defined(PATCH)
float3 DrapeSheetMeet(float3 normalWS, VS_OUTPUT input, bool pdMode, float pixelDist, float3 groundData, float pixelAbsZ)
{
	float sheetMeet = 0.0;
	[branch] if (pdMode && ObjectMeetBand > 0.0 && pixelDist < 2048.0 && groundData.x > -50000.0)
	{
		float sheetDepth;
		float sheetZ = SheetSurfaceZ(input.GridLocal, pixelAbsZ - ObjectMeetBand * 0.5, sheetDepth, true, 0.0, 1.0);
		float sheetGap = pixelAbsZ - sheetZ;
		[branch] if (sheetZ > -50000.0 && sheetGap < ObjectMeetBand * 0.5)
		{
			sheetMeet = (1.0 - smoothstep(0.0, ObjectMeetBand * 0.5, sheetGap)) * smoothstep(0.35, 0.6, normalize(input.NormalWS).z) *
			            (1.0 - smoothstep(1024.0, 2048.0, pixelDist));
			const float mStep = 4.0;
			float3 mXP = SampleTerrainStatics(input.GridLocal + float2(mStep, 0.0));
			float3 mXN = SampleTerrainStatics(input.GridLocal - float2(mStep, 0.0));
			float3 mYP = SampleTerrainStatics(input.GridLocal + float2(0.0, mStep));
			float3 mYN = SampleTerrainStatics(input.GridLocal - float2(0.0, mStep));
			[flatten] if (min(min(mXP.x, mXN.x), min(mYP.x, mYN.x)) > -50000.0)
			{
				float3 sheetN = normalize(float3(-((mXP.x + max(mXP.y, 0.0)) - (mXN.x + max(mXN.y, 0.0))) / (2.0 * mStep),
					-((mYP.x + max(mYP.y, 0.0)) - (mYN.x + max(mYN.y, 0.0))) / (2.0 * mStep), 1.0));
				normalWS = normalize(lerp(normalWS, sheetN, 0.5 * sheetMeet));
			}
		}
	}
	return normalWS;
}
#endif

#if !defined(SNOW_PATCH_DEFERRED)
#ifdef SNOW_STATICS_DEPTH_PREPASS
bool SkinQuadHidden(float z, float2 pixel)
{
	float slope = max(abs(ddx_fine(z)), abs(ddy_fine(z)));
	float reach = DecalMode > 0.5 ? (abs(DebugSkinDepth.x) + 1.5) * exp2(-23.0) + (abs(DebugSkinDepth.y) + 0.65) * slope * 1.01 : 1e-5;
	float hidden = (z - reach > PreSkinMainDepth.Load(int3(pixel, 0)) + 2e-7) ? 1.0 : 0.0;
	float hx = ddx_fine(hidden);
	return hidden > 0.5 && hx == 0.0 && ddy_fine(hidden) == 0.0 && ddy_fine(hx) == 0.0;
}
#endif

#ifdef SNOW_STATICS_DEPTH_PREPASS
void main(VS_OUTPUT input)
#elif defined(SNOW_PATCH_ATTR)
PATCH_ATTR_OUTPUT main(VS_OUTPUT input)
#else
PS_OUTPUT main(VS_OUTPUT input)
#endif
{
#ifdef SNOW_STATICS_DEPTH_PREPASS
	[branch] if (SkinQuadHidden(input.Position.z, input.Position.xy))
	{
		discard;
		return;
	}
#endif
#ifdef SNOW_PATCH_ATTR
	const float4 attrGridDeriv = float4(ddx_coarse(input.GridLocal), ddy_coarse(input.GridLocal));
	const float2 attrZDeriv = float2(ddx_coarse(input.WorldPos.z), ddy_coarse(input.WorldPos.z));
#endif
	[branch] if (FadeExempt < 0.5 && length(input.WorldPos.xy) >= SkinFadeEnd)
		discard;
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float3 normalWS = normalize(input.NormalWS);
#ifndef PATCH
	[branch] if (normalWS.z < -0.3)
		discard;
#endif

	float2 worldXY = GridOrigin + input.GridLocal;
	float pixelDist = length(input.WorldPos);
	bool pdMode = ProjPixelEnable > 1.5;
	[branch] if (LODBatch > 0.5)
	{
		[flatten] if (SeamRampInv > 0.0)
		{
			bool lodOwnsPixel = HasSkinMasksCopy > 0.5 && PreSkinMasks.Load(int3(input.Position.xy, 0)).y >= 256.0;
			[flatten] if (!lodOwnsPixel && all(worldXY > SeamBounds.xy) && all(worldXY < SeamBounds.zw))
				discard;
		}
		float lodZ = input.CurrentClip.w;
		float lodBand = (1.0 + 0.0005 * lodZ) * SharedData::CameraData.w / (SharedData::CameraData.z * lodZ * lodZ);
		float lodSceneD = SceneDepth.Load(int3(input.Position.xy, 0));
		if (abs(lodSceneD - input.Position.z) > max(lodBand, 6e-6))
			discard;
	}
	else [branch] if (AlphaTested > 0.5 && DecalMode < 0.5)
	{
		float atSceneZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
		float3 atView = normalize(input.WorldPos);
		float atGrazing = max(abs(dot(atView, normalize(input.NormalWS))), 0.25);
		if (atSceneZ - input.CurrentClip.w > (max(input.Lift, 0.0) + 6.0) / atGrazing)
			discard;
	}

	float shoulderWall = 0.0;
	float wallClearance = 0.0;
#	ifndef PATCH
	[branch] if (HasObjectTop > 0.5 && (!pdMode || LegacySkin > 0.5))
	{
		float objTop = PatchTop(worldXY);
		[flatten] if (objTop > -50000.0)
		{
			float liftBase = max(lerp(RoundedDepth, ObjectsDepth, input.Flat), kMinSkinLift);
			wallClearance = ((input.WorldPos.z + ShellCameraPosAdjust.z) - objTop) / liftBase;
			shoulderWall = smoothstep(0.0, 0.15, wallClearance);
		}
	}
#	endif

	float pixelCoverage = smoothstep(0.4, 0.7, input.Coverage);
#	ifndef PATCH
	float3 dPosX = ddx(input.WorldPos);
	float3 dPosY = ddy(input.WorldPos);
	float3 geoFacing = normalize(cross(dPosY, dPosX));
	float footprint = length(abs(dPosX) + abs(dPosY));
	float liftBase = max(input.LiftTarget, kMinSkinLift);
	float coatRef = min(kProjCoatLift, liftBase);

	float3 projWorldPos = input.WorldPos + ShellCameraPosAdjust.xyz;
	projWorldPos.z -= input.Lift;
	float3 projGradX, projGradY;
	Triplanar::ComputeGradients(projWorldPos, ProjNoiseTiling, projGradX, projGradY);
	float pdCoverage = 0.0;
	float edgeNz = normalWS.z;
	float edgeW = -1.0;
	float edgeThr = kCoatSolidW;
	[branch] if (pdMode)
	{
		float nzPix = normalWS.z;
		edgeNz = nzPix;
		float3 triW = Triplanar::GetWeights(normalWS, normalWS);
		float noise = Triplanar::SampleGrad(ProjNoiseMap, SnowSampler, projWorldPos, triW, ProjNoiseTiling, projGradX, projGradY).x;
		float wpix = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1 - ProjNoiseScale * noise;
		float wSmooth = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1;
		float wFill = wpix > 0.003 ? max(wpix, 0.2) : wpix;
		float wSmoothFill = wSmooth > 0.003 ? max(wSmooth, 0.2) : wSmooth;
		pdCoverage = smoothstep(edgeThr - 0.03, edgeThr, wFill) * smoothstep(edgeThr - 0.18, edgeThr + 0.08, wSmoothFill);
		edgeW = wFill;
		pdCoverage *= smoothstep(0.3 * coatRef, coatRef, input.Lift);
		[branch] if (HasSkinMasksCopy > 0.5 && input.Lift < 2.0 * coatRef)
		{
			float realEnc = SkinRecolorEnc(SkinMasksY(int2(input.Position.xy)));
			[flatten] if (realEnc >= 1.5)
				pdCoverage *= (saturate(realEnc - 2.0) >= kCoatSolidReal) ? 1.0 : 0.0;
		}
		float liftFrac = input.Lift / max(input.LiftTarget, kMinSkinLift);
		pdCoverage = max(pdCoverage, max(
										smoothstep(0.5, 0.85, liftFrac),
										smoothstep(1.5 * coatRef, 4.0 * coatRef, input.Lift)));
	}
	else [flatten] if (ProjThreshold > -0.9)
		pixelCoverage *= smoothstep(0.06, 0.14, input.ProjFactor);
	[flatten] if (FullCoat > 0.5)
		pixelCoverage = 1.0;

	float liftEdge = 0.06 * liftBase * (0.6 + 0.8 * CoverageNoise(worldXY * 3.0));
	float liftBand = 0.45 * liftEdge;
	float liftEdgeLOD = min(max(liftEdge, kRimBandPx * fwidth(input.Lift)), kRimBandMax * liftBase);
	float liftCoverage = smoothstep(liftEdgeLOD - liftBand, liftEdgeLOD, input.Lift);
	pixelCoverage *= liftCoverage;

	[flatten] if (pdMode)
		pixelCoverage = pdCoverage;
#	endif
	pixelCoverage = max(pixelCoverage, shoulderWall);

	float pixelDeform = saturate(SampleDeformation(input.GridLocal));
#ifdef PATCH
	bool carveObject = true;
#else
	bool carveObject = LegacySkin > 0.5;
#endif
	float2 trenchGridLocal = input.GridLocal;
	float3 viewDirWS = normalize(input.WorldPos);
	float trenchHitS = 0.0;
#	ifndef PATCH
	[branch] if (RoadField > 0.5 && RoundedDepth > 1.0)
	{
		[branch] if (RoadOwnsColumn(worldXY))
			discard;
	}

	[branch] if (carveObject && input.Flat < 0.5 && RoundedDepth > 1.0 && pixelDeform > 0.005 && length(input.WorldPos.xy) < 950.0)
	{
		if (abs(geoFacing.z) > 0.55 && Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount) < smoothstep(0.01, 0.06, pixelDeform))
			discard;
	}

	[branch] if (carveObject && input.Flat < 0.5 && pixelDeform > 0.001 && pixelCoverage > 0.35)
	{
		float pomDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 25.0);
		[branch] if (pomDepth > 0.5)
		{
			float carveCap = pomDepth;
			float invRayZ = 1.0 / max(-viewDirWS.z, 0.25);
			float2 stepXY = viewDirWS.xy * invRayZ;
			float tPrev = 0.0;
			float carvePrev = min(pomDepth * pixelDeform, carveCap);
			[loop] for (uint pomI = 1; pomI <= 8; pomI++) {
				float t = carveCap * float(pomI) / 8.0;
				float2 xy = input.GridLocal + stepXY * t;
				float carve = min(pomDepth * saturate(SampleDeformation(xy)), carveCap);
				[branch] if (t >= carve)
				{
					float w = saturate((carvePrev - tPrev) / max((carvePrev - tPrev) + (t - carve), 1e-4));
					float tHit = lerp(tPrev, t, w);
					trenchGridLocal = input.GridLocal + stepXY * tHit;
					trenchHitS = tHit * invRayZ;
					break;
				}
				tPrev = t;
				carvePrev = carve;
			}
			[flatten] if (trenchHitS == 0.0 && carvePrev > 0.0)
			{
				trenchGridLocal = input.GridLocal + stepXY * carveCap;
				trenchHitS = carveCap * invRayZ;
			}
			pixelDeform = saturate(SampleDeformation(trenchGridLocal));
		}

		const float step = 4.0;
		float dXP = SampleDeformation(trenchGridLocal + float2(step, 0.0));
		float dXN = SampleDeformation(trenchGridLocal - float2(step, 0.0));
		float dYP = SampleDeformation(trenchGridLocal + float2(0.0, step));
		float dYN = SampleDeformation(trenchGridLocal - float2(0.0, step));
		float2 deformGradient = float2(dXP - dXN, dYP - dYN) / (2.0 * step);

		float pixelDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0) * pixelCoverage;
		normalWS = normalize(normalWS + float3(deformGradient * pixelDepth * 0.6, 0.0));
	}
#	endif

	float coverageGate = saturate(pixelCoverage + (CoverageNoise(worldXY) - 0.5) * 0.3 * saturate(pixelCoverage * 4.0));
	float coverageAlpha = smoothstep(0.05, 0.35, coverageGate);
	float downKill = smoothstep(-0.05, 0.1, input.Coverage);
	[flatten] if (pdMode)
		downKill = smoothstep(-0.08, -0.02, input.Coverage);
	coverageAlpha *= max(downKill, shoulderWall);

	float edgeBlend = SnowHeightBlendSharpness(pixelDist);
	float2 edgeSnowUV = (SnowUVOffset + input.GridLocal) / kSnowUVTile;
	float edgeSnowMip = SnowHeightMip(edgeSnowUV);
	bool edgeBlendOn = HasSnowHeight > 0.5 && edgeBlend > 1.0;
	[branch] if (edgeBlendOn && coverageAlpha > 0.001 && coverageAlpha < 0.999)
	{
		float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, worldXY), 0.0.xx, edgeSnowMip);
		coverageAlpha = SnowHeightBlendOneSided(coverageAlpha, edgeSnowH, edgeBlend);
	}

	float pixelAbsZ = input.WorldPos.z + ShellCameraPosAdjust.z;
	float seamTotal = 1.0;
	float3 groundData = SampleTerrainStatics(input.GridLocal);
	[flatten] if (groundData.x > -50000.0)
	{
		float groundShellZ = groundData.x + max(groundData.y, 0.0);
		const float kSeamBandLow = -36.0;
		const float kSeamBandHigh = 10.0;
		float groundBand = smoothstep(kSeamBandLow, kSeamBandHigh, pixelAbsZ - groundShellZ);
		if (edgeBlendOn && groundBand > 0.001 && groundBand < 0.999)
		{
			float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, worldXY), 0.0.xx, edgeSnowMip);
			groundBand = SnowHeightBlendOneSided(groundBand, edgeSnowH, edgeBlend);
		}
		seamTotal = groundBand;
	}
	[flatten] if (pdMode)
		seamTotal = 1.0;
	coverageAlpha *= seamTotal;

#	ifndef PATCH
	[branch] if (!pdMode && HasSnowHeight > 0.5 && groundData.x > -50000.0 && groundData.y > 0.5 && pixelDist < 2048.0)
	{
		float blanketTopZ = groundData.x + max(groundData.y, 0.0);
		float dzTop = pixelAbsZ - blanketTopZ;
		float normalBand = smoothstep(-6.0, -2.0, dzTop) * (1.0 - smoothstep(0.5, 6.0, dzTop));
		normalBand *= smoothstep(0.3, 0.6, normalWS.z);
		[branch] if (normalBand > 0.001)
		{
			const float nStep = 4.0;
			float3 gXP = SampleTerrainStatics(input.GridLocal + float2(nStep, 0.0));
			float3 gXN = SampleTerrainStatics(input.GridLocal - float2(nStep, 0.0));
			float3 gYP = SampleTerrainStatics(input.GridLocal + float2(0.0, nStep));
			float3 gYN = SampleTerrainStatics(input.GridLocal - float2(0.0, nStep));
			[flatten] if (min(min(gXP.x, gXN.x), min(gYP.x, gYN.x)) > -50000.0)
			{
				float zXP = gXP.x + max(gXP.y, 0.0);
				float zXN = gXN.x + max(gXN.y, 0.0);
				float zYP = gYP.x + max(gYP.y, 0.0);
				float zYN = gYN.x + max(gYN.y, 0.0);
				float3 blanketN = normalize(float3(-(zXP - zXN) / (2.0 * nStep), -(zYP - zYN) / (2.0 * nStep), 1.0));
				normalWS = normalize(lerp(normalWS, blanketN, normalBand * (1.0 - smoothstep(1024.0, 2048.0, pixelDist))));
			}
		}
	}
#	endif

#	ifndef PATCH
#	ifndef SNOW_STATICS_DEPTH_PREPASS
	normalWS = DrapeSheetMeet(normalWS, input, pdMode, pixelDist, groundData, pixelAbsZ);
#	endif
#	endif

	coverageAlpha = max(coverageAlpha, smoothstep(0.15, 0.5, pixelDeform) * smoothstep(0.35, 0.6, pixelCoverage));

#	ifndef PATCH
	coverageAlpha *= max(liftCoverage, shoulderWall);
#	else
	coverageAlpha *= 1.0 - smoothstep(8.0, 24.0, PatchSilhouetteDrop(worldXY));
#	endif

	float fadeAlpha = 1.0;
	[flatten] if (FadeExempt < 0.5)
		fadeAlpha = 1.0 - smoothstep(SkinFadeStart, SkinFadeEnd, length(input.WorldPos.xy));

#	ifndef PATCH
	float edgeFlankLift = 0.0;
	float coatPush = 0.0;
	[flatten] if (pdMode)
		coverageAlpha = 0.0;
	[flatten] if (FullCoat > 0.5)
		coverageAlpha = 1.0;
	bool coatOn = pdMode && EdgeCoat > 0.5;
	bool lumpsOn = coatOn && EdgeFlankWidth > 0.001;
	[branch] if (LegacySkin < 0.5 && (coatOn || fadeAlpha < 0.5))
	{
		const float fadeIn = fadeAlpha;
		bool inside = coverageAlpha >= 0.5;
		bool solid = inside;
		bool needField = inside && fadeIn < 0.5;
		float nearPaint = 0.0;
		[branch] if (!inside && coatOn)
		{
			float2 masksDim;
			PreSkinMasks.GetDimensions(masksDim.x, masksDim.y);
			float2 masksUV = input.Position.xy / masksDim;
			float realEnc = 0.0;
			float realW = -1.0;
			[branch] if (HasSkinMasksCopy > 0.5)
			{
				int2 mp = int2(input.Position.xy);
				float rawEnc = SkinMasksY(mp);
				realEnc = SkinRecolorEnc(rawEnc);
				realW = SkinRecolorWeight(rawEnc);
				[branch] if (realEnc < 1.5)
				{
					int2 mmax = int2(masksDim) - 1;
					float4 rawN = float4(SkinMasksY(min(mp + int2(1, 0), mmax)),
						SkinMasksY(max(mp - int2(1, 0), 0)),
						SkinMasksY(min(mp + int2(0, 1), mmax)),
						SkinMasksY(max(mp - int2(0, 1), 0)));
					realEnc = max(max(SkinRecolorEnc(rawN.x), SkinRecolorEnc(rawN.y)), max(SkinRecolorEnc(rawN.z), SkinRecolorEnc(rawN.w)));
					realW = max(max(SkinRecolorWeight(rawN.x), SkinRecolorWeight(rawN.y)), max(SkinRecolorWeight(rawN.z), SkinRecolorWeight(rawN.w)));
				}
			}
			bool realKnown = realEnc >= 1.5;
			bool painted = realKnown ? (saturate(realEnc - 2.0) >= kCoatSolidReal) : (LODBatch < 0.5 && AlphaTested < 0.5 && TexturedSnow < 0.5 && edgeW >= edgeThr);
			solid = painted && input.Coverage >= kCoatMinNz;
			[branch] if (!painted && lumpsOn && realKnown && input.Coverage > kCoatMinNz - 0.1)
			{
				float reachW = kEdgeReachWeightShift * EdgeFlankWidth;
				const float swapW = ParallaxFollow > 0.5 ? 0.0 : ProjNoiseWeight * saturate(EdgeFlankWidth / kDetileRamp);
				const float steepMin = 1.0 - SteepThin;
				[branch] if (swapW > 0.0 && realW > -0.9 && realW + reachW + swapW >= -0.01 && realW + max(reachW - swapW, 0.0) * steepMin <= 0.02)
				{
					const float sceneViewZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
					const float3 sceneP = (input.WorldPos * (sceneViewZ / input.CurrentClip.w) + ShellCameraPosAdjust.xyz) * ProjNoiseTiling;
					const float3 swapN = normalize(input.NormalWS);
					const float3 swapTriW = Triplanar::GetWeights(swapN, swapN);
					const float gameNoise = ProjNoiseGame(sceneP, swapTriW, projGradX, projGradY);
					[branch] if (realW + reachW + swapW * gameNoise >= -0.01 && realW + max(reachW + swapW * (gameNoise - 1.0), 0.0) * steepMin <= 0.02)
						reachW += swapW * (gameNoise - ProjNoiseDetiled(sceneP, swapTriW, projGradX, projGradY));
				}
#ifdef SNOW_STATICS_DEPTH_PREPASS
				const float npLo = saturate((realW + max(reachW * (1.0 - SteepThin), 0.0)) * 50.0 + 0.5);
				const float npHi = saturate((realW + max(reachW, 0.0)) * 50.0 + 0.5);
				const float kLo = (npLo - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
				const float kHi = (npHi - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
				const bool surelyKeep = npLo > 0.15 + 1e-4 && kLo >= 1e-4;
				const bool surelyCut = !(npHi > 0.15 - 1e-4 && kHi >= -1e-4);
				float steepKeep = surelyKeep ? 1.0 : 1.0 - SteepThin;
				[branch] if (!surelyKeep && !surelyCut)
				{
					float3 bentN = DrapeSheetMeet(normalWS, input, pdMode, pixelDist, groundData, pixelAbsZ);
					steepKeep = 1.0 - SteepThin * (1.0 - smoothstep(0.15, 0.5, bentN.z));
				}
#else
				float steepKeep = 1.0 - SteepThin * (1.0 - smoothstep(0.15, 0.5, normalWS.z));
#endif
				nearPaint = saturate((realW + max(reachW * steepKeep, 0.0)) * 50.0 + 0.5);
			}
			needField = solid ? (fadeIn < 0.5) : (nearPaint > 0.15);
		}
		fadeAlpha = 1.0;
		float keep = solid ? 1.0 : -1.0;
		[branch] if (needField)
		{
			[flatten] if (solid)
				keep = 2.4 * fadeIn - 0.7;
			else
				keep = (nearPaint - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
		}
		[flatten] if (inside && keep < 0.0)
			coverageAlpha = 0.0;
		[flatten] if (inside && keep >= 0.0)
			coatPush = kEdgeFlankLift;
		[flatten] if (!inside && keep >= 0.0)
		{
			coverageAlpha = 1.0;
			edgeFlankLift = kEdgeFlankLift;
		}
	}
#	endif

	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	if (coverageAlpha < 0.5)
		discard;
	coverageAlpha = 1.0;
	float ditherRef = pdMode ? screenNoise : screenNoise * screenNoise;
	if (ditherRef >= fadeAlpha)
		discard;

#ifdef SNOW_STATICS_DEPTH_PREPASS
	return;
#endif
#ifdef SNOW_PATCH_ATTR
	PATCH_ATTR_OUTPUT attr;
	attr.Grid = float4(input.GridLocal, normalWS.xy);
	attr.Deriv = attrGridDeriv;
	attr.Depth = float4(input.CurrentClip.w, attrZDeriv, 1.0);
	attr.Mark = 1.0;
	return attr;
#endif
#if !defined(SNOW_STATICS_DEPTH_PREPASS) && !defined(SNOW_PATCH_ATTR)
	SkinShadeInput ssi;
	ssi.WorldPos = input.WorldPos;
	ssi.Position = input.Position;
	ssi.CurrentClip = input.CurrentClip;
	ssi.Flat = input.Flat;
	ssi.GridLocal = input.GridLocal;
	ssi.trenchGridLocal = trenchGridLocal;
	ssi.worldXY = worldXY;
	ssi.pixelDist = pixelDist;
	ssi.pixelDeform = pixelDeform;
	ssi.screenNoise = screenNoise;
#	ifndef PATCH
	const float raisedDepth = max(RoundedDepth, ObjectsDepth);
	ssi.selfShadowReject = (edgeFlankLift > 0.0 && raisedDepth > kMinSkinLift) ? raisedDepth + 8.0 : 0.0;
	ssi.onObject = raisedDepth > kMinSkinLift ? 0.0 : 1.0;
#	else
	ssi.selfShadowReject = 0.0;
	ssi.onObject = 0.0;
#	endif
	ssi.detail = 1.0;
	ssi.pom = 1.0;
#ifdef PATCH
	ssi.geoNz = -1.0;
#else
	ssi.geoNz = abs(geoFacing.z);
	ssi.markBase = input.WorldPos.z + ShellCameraPosAdjust.z - input.Lift;
#endif
#ifdef SNOW_COAT_PARALLAX
	ssi.surfaceShift = ObjectParallaxShift(input);
#endif
	SkinShadeResult ssr = SkinShadeSurface(ssi, normalWS);
	normalWS = ssr.normalWS;
	float3 viewNormal = ssr.viewNormal;
	float3 ambientPart = ssr.ambientPart;
	float3 diffuseLobe = ssr.diffuseLobe;
	float3 directSpecular = ssr.directSpecular;
	float3 specularLobe = ssr.specularLobe;
	float3 preLit = ssr.preLit;
	float landVertexAO = ssr.landVertexAO;
	float snowRoughness = ssr.snowRoughness;

	float stochasticBlend = (screenNoise * screenNoise) < coverageAlpha ? 1.0 : 0.0;

	PS_OUTPUT psout;
#	if !defined(PATCH) && !defined(SNOW_STATICS_NO_DEPTH_EXPORT)
	psout.Depth = input.Position.z;
	float pushS = trenchHitS > 0.0 ? trenchHitS : -max(edgeFlankLift, coatPush);
	[branch] if (pushS != 0.0)
	{
		float4 rawClip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float4 pushClip = mul(CameraViewProj, float4(input.WorldPos + viewDirWS * pushS, 1.0));
		psout.Depth = input.Position.z + (pushClip.z / max(pushClip.w, 1e-4) - rawClip.z / max(rawClip.w, 1e-4));
	}
#	endif
	psout.Diffuse = float4(preLit, coverageAlpha);
	psout.MotionVectors = float4(motionVector, 0.0, coverageAlpha);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(viewNormal), 1.0 - snowRoughness, stochasticBlend);
	psout.Albedo = float4(diffuseLobe, coverageAlpha);
	psout.Specular = float4(directSpecular, coverageAlpha);
	psout.Reflectance = float4(specularLobe, coverageAlpha);
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ambientPart).x, coverageAlpha);
	psout.Masks2 = float4(1.0 - landVertexAO, 0.0, 0.0, coverageAlpha);
	return psout;
#endif
}
#endif

#if defined(SNOW_PATCH_DEFERRED) && defined(PATCH)
Texture2D<float4> PatchAttrGrid : register(t91);
Texture2D<float4> PatchAttrDeriv : register(t92);
Texture2D<float4> PatchAttrDepth : register(t93);
Texture2D<float> PatchAttrMark : register(t94);

PS_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	if (PatchAttrMark.Load(px) < 0.5)
		discard;
	const float4 depth = PatchAttrDepth.Load(px);
	const float4 grid = PatchAttrGrid.Load(px);
	const float4 deriv = PatchAttrDeriv.Load(px);
	gDeferredGridDx = deriv.xy;
	gDeferredGridDy = deriv.zw;
	gDeferredZDx = depth.y;
	gDeferredZDy = depth.z;
	const float2 ndc = float2(pos.x / TessScreen.y - 1.0, 1.0 - pos.y / TessScreen.z);
	const float3 r0 = CameraViewProj[0].xyz;
	const float3 r1 = CameraViewProj[1].xyz;
	const float3 r3 = CameraViewProj[3].xyz;
	const float3 b = float3(ndc.x * depth.x - CameraViewProj[0].w, ndc.y * depth.x - CameraViewProj[1].w, depth.x - CameraViewProj[3].w);
	const float3 c0 = cross(r1, r3);
	const float3 rel = (b.x * c0 + b.y * cross(r3, r0) + b.z * cross(r0, r1)) / dot(r0, c0);
	const float4 currentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	const float4 previousClip = mul(CameraPreviousViewProjUnjittered, float4(rel + ShellCameraPosAdjust.xyz - ShellCameraPreviousPosAdjust.xyz, 1.0));
	const float2 motionVector = float2(-0.5, 0.5) * (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w);

	SkinShadeInput ssi;
	ssi.WorldPos = rel;
	ssi.Position = float4(pos.xy, 0.0, 1.0);
	ssi.CurrentClip = currentClip;
	ssi.Flat = 0.0;
	ssi.GridLocal = grid.xy;
	ssi.trenchGridLocal = grid.xy;
	ssi.worldXY = GridOrigin + grid.xy;
	ssi.pixelDist = length(rel);
	ssi.pixelDeform = saturate(SampleDeformation(grid.xy));
	ssi.screenNoise = Random::InterleavedGradientNoise(pos.xy, SharedData::FrameCount);
	ssi.selfShadowReject = 0.0;
	ssi.onObject = 0.0;
	ssi.detail = 1.0;
	ssi.pom = 1.0;
	ssi.geoNz = -1.0;
	const SkinShadeResult ssr = SkinShadeSurface(ssi, float3(grid.zw, sqrt(saturate(1.0 - dot(grid.zw, grid.zw)))));

	PS_OUTPUT psout;
	psout.Diffuse = float4(ssr.preLit, 1.0);
	psout.MotionVectors = float4(motionVector, 0.0, 1.0);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(ssr.viewNormal), 1.0 - ssr.snowRoughness, 1.0);
	psout.Albedo = float4(ssr.diffuseLobe, 1.0);
	psout.Specular = float4(ssr.directSpecular, 1.0);
	psout.Reflectance = float4(ssr.specularLobe, 1.0);
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ssr.ambientPart).x, 1.0);
	psout.Masks2 = float4(1.0 - ssr.landVertexAO, 0.0, 0.0, 1.0);
	return psout;
}
#endif
#endif

#if defined(COMPUTESHADER) && defined(SNOW_ROAD_MARCH_CS)
RWTexture2D<float> RoadMarchOut : register(u0);
#	include "SnowDeformation/SnowMarchCacheCB.hlsli"

float RoadTapHeight(float2 sampleLocal)
{
	float sh = -100000.0;
	const float2 topLocal = (GridOrigin + sampleLocal - HeightWindowCenter) / HeightHalfExtent;
	[branch] if (all(abs(topLocal) < 0.98))
	{
		float2 topDims;
		ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
		const float2 tapWorld = GridOrigin + sampleLocal;
		const float2 bt = PatchTexel(tapWorld, topDims);
		const int2 bt0 = (int2)bt;
		const float2 btf = bt - bt0;
		const int2 bt1 = min(bt0 + 1, int2(topDims) - 1);
		const float4 tapTops = float4(
			ObjectTopRaw.Load(int3(bt0.x, bt0.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt0.y, 0)),
			ObjectTopRaw.Load(int3(bt0.x, bt1.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt1.y, 0)));
		[branch] if (all(tapTops > -50000.0))
		{
			const float2 sd00 = ObjectSkinDepth.Load(int3(bt0.x, bt0.y, 0));
			const float2 sd10 = ObjectSkinDepth.Load(int3(bt1.x, bt0.y, 0));
			const float2 sd01 = ObjectSkinDepth.Load(int3(bt0.x, bt1.y, 0));
			const float2 sd11 = ObjectSkinDepth.Load(int3(bt1.x, bt1.y, 0));
			const float topSmooth = lerp(lerp(tapTops.x, tapTops.y, btf.x), lerp(tapTops.z, tapTops.w, btf.x), btf.y);
			float depthSmooth = lerp(lerp(sd00.x, sd10.x, btf.x), lerp(sd01.x, sd11.x, btf.x), btf.y);
			const float tapRoadTop = max(max(sd00.y, sd10.y), max(sd01.y, sd11.y));
			[branch] if (tapRoadTop > kNoRoadTop * 0.5 && depthSmooth >= 1.0)
			{
				const float topRoad = (topSmooth - tapRoadTop) >= kRoadOwnsTop ? tapRoadTop : topSmooth;
				const float tapLand = max(SampleTerrainStatics(sampleLocal).y, 0.0);
				const float tapCone = ObjectConeDepth(tapWorld);
				depthSmooth = max(min(tapCone, depthSmooth), min(tapLand, depthSmooth));
				const float tapDeform = SampleDeformation(sampleLocal);
				const float4 tapBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : float4(0.0, tapDeform, TrackAge.x, TrackAge.x);
				const float tapDepth = CarveProfileNoTeeth(tapDeform, tapBerm.y, depthSmooth, TrackCarveLook(TrackAgeOf(tapBerm.z))) +
				                       BermShape(tapBerm.x) * saturate(1.0 - tapDeform) * depthSmooth * BermHeightAmp * BermDepthGate(depthSmooth) * TrackBermScale(TrackAgeOf(tapBerm.w));
				sh = topRoad + tapDepth + Undulation(tapWorld) * saturate(tapDepth / 8.0);
			}
		}
	}
	return sh;
}

void RoadMarchTexel(uint2 phys)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (any(float2(phys) >= dims))
		return;
	const float2 logical = float2((int2(phys) - DeformMapOrigin) & (int2(dims) - 1));
	RoadMarchOut[phys] = RoadTapHeight((logical + 0.5) / dims / DeformInvWorldSize - GridToDeformOffset);
}

[numthreads(8, 8, 1)] void RoadMarchTilesCS(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= MarchTilesIn[0])
		return;
	const uint packed = MarchTilesIn[1 + listIndex];
	RoadMarchTexel(uint2(packed & 0xFFFFu, packed >> 16u) * 8u + GTid.xy);
}

[numthreads(8, 8, 1)] void RoadMarchRowsCS(uint3 DTid : SV_DispatchThreadID)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (DTid.y >= MarchRowCount)
		return;
	RoadMarchTexel(uint2(DTid.x, (MarchRowStart + DTid.y) % (uint)dims.y));
}
#endif