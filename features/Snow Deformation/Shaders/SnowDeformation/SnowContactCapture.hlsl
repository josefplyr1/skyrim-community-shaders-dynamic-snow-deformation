#ifndef COAT
cbuffer StaticCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;
	float2 HeightWindowCenter;
	float HeightHalfExtent;
}
#endif

#ifdef SKINNED
cbuffer ContactSkinCB : register(b2)
{
	float4 BoneRows[240];
	float2 SkinWindowCenter;
	float SkinHalfExtent;
	float SkinBoneCount;
}

struct VS_INPUT_SKIN
{
	float4 Position : POSITION0;
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
};

float3x4 SkinnedMatrix(VS_INPUT_SKIN input)
{
	int4 rows = int4(765.01 * input.BoneIndices);
	float4 w = input.BoneWeights;
	const int lastRow = int(SkinBoneCount) * 3;
	w.x = rows.x < lastRow ? w.x : 0.0;
	w.y = rows.y < lastRow ? w.y : 0.0;
	w.z = rows.z < lastRow ? w.z : 0.0;
	w.w = rows.w < lastRow ? w.w : 0.0;
	const float wsum = dot(w, 1.0);
	[flatten] if (wsum < 1e-3)
	{
		w = float4(1.0, 0.0, 0.0, 0.0);
		rows = int4(0, 0, 0, 0);
	}
	else
		w /= wsum;
	return float3x4(BoneRows[rows.x], BoneRows[rows.x + 1], BoneRows[rows.x + 2]) * w.x +
	       float3x4(BoneRows[rows.y], BoneRows[rows.y + 1], BoneRows[rows.y + 2]) * w.y +
	       float3x4(BoneRows[rows.z], BoneRows[rows.z + 1], BoneRows[rows.z + 2]) * w.z +
	       float3x4(BoneRows[rows.w], BoneRows[rows.w + 1], BoneRows[rows.w + 2]) * w.w;
}

float3 SkinnedWorld(VS_INPUT_SKIN input)
{
	return mul(float4(input.Position.xyz, 1.0), transpose(SkinnedMatrix(input)));
}
#endif

#ifndef COAT
struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

#	ifdef VSHADER
#		ifdef SKINNED
VS_OUTPUT main(VS_INPUT_SKIN input)
{
	float3 worldAbs = SkinnedWorld(input);

	float2 ndc = (worldAbs.xy - SkinWindowCenter) / SkinHalfExtent;

	VS_OUTPUT output;
	output.Position = float4(ndc, 0.5, 1.0);
	output.WorldZ = worldAbs.z;
	return output;
}
#		else
VS_OUTPUT main(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);

	float2 ndc = (worldAbs.xy - HeightWindowCenter) / HeightHalfExtent;

	VS_OUTPUT output;
	output.Position = float4(ndc, 0.5, 1.0);
	output.WorldZ = worldAbs.z;
	return output;
}
#		endif
#	endif

#	ifdef PSHADER
float2 main(VS_OUTPUT input) : SV_Target
{
	return input.WorldZ.xx;
}
#	endif
#else
struct VS_OUTPUT_COAT
{
	float3 Texel : TEXCOORD0;
	float Below : TEXCOORD1;
	float Wet : TEXCOORD2;
	float Growth : TEXCOORD3;
	float Target : TEXCOORD4;
	float3 World : TEXCOORD5;
	float Open : TEXCOORD6;
	float3 Deposit : TEXCOORD7;
	float Impact : TEXCOORD8;
};

struct GS_OUTPUT_COAT
{
	float4 Position : SV_POSITION;
	nointerpolation uint4 Voxel : TEXCOORD0;
	nointerpolation uint Fall : TEXCOORD1;
	nointerpolation uint4 Cell : TEXCOORD2;
};

static const uint kCoatTileDim = 64;

#	if defined(VSHADER) && defined(SKINNED)
#		include "Common/SharedData.hlsli"
#		include "SnowDeformation/SnowDeformation.hlsli"
#		define SKYLIGHTING_PROBE_REGISTER t2
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if (defined(VSHADER) && defined(SKINNED)) || defined(GSHADER)
cbuffer BodyCoatWriteCB : register(b3)
{
	float4 WriteModelToAtlas[3];
	float4 WriteAtlasLo;
	float4 WriteAtlasHi;
	float WriteGrowth;
	float2 WriteHeading;
	float WriteImpactShare;
	float4 WriteTile;
	float WriteTexel;
	float WriteContact;
	float WriteFall;
	float WriteSky;
	float4 WriteModelToCanonical[3];
	float4 WriteTop;
	float3 WriteEye;
	float WriteVoxel;
	float3 WriteFrom;
	float WriteTopZ;
	float WriteMoving;
	int3 WriteScratchShift;
}
#	endif

#	if defined(VSHADER) && defined(SKINNED)
Texture2D<float> CoatTile : register(t0);
Texture2D<float> CoatTop : register(t1);

float CoatTileTop(float2 worldXY)
{
	const float2 g = clamp((worldXY - WriteTile.xy) / WriteTile.z, 0.0, kCoatTileDim - 1.001);
	const int2 i = int2(g);
	const float2 f = g - float2(i);
	const int row = int(WriteTile.w) + i.y;
	return lerp(lerp(CoatTile.Load(int3(i.x, row, 0)), CoatTile.Load(int3(i.x + 1, row, 0)), f.x),
		lerp(CoatTile.Load(int3(i.x, row + 1, 0)), CoatTile.Load(int3(i.x + 1, row + 1, 0)), f.x), f.y);
}

float2 CoatBelowSnow(float3 world)
{
	const float texel = WriteTexel;
	const float4 tops = float4(CoatTileTop(world.xy + float2(texel, 0.0)), CoatTileTop(world.xy - float2(texel, 0.0)),
		CoatTileTop(world.xy + float2(0.0, texel)), CoatTileTop(world.xy - float2(0.0, texel)));
	const float4 over = max(tops - world.z, 0.0);
	const float most = max(max(over.x, over.y), max(over.z, over.w));
	const float2 push = float2(over.x - over.y, over.z - over.w);
	return float2(max(max(tops.x, tops.y), max(tops.z, tops.w)) + 0.25 * texel - world.z, saturate(dot(push, WriteHeading) / max(most, 1e-3)));
}

float3 CoatTopPlace(float3 world)
{
	const float3 across = normalize(cross(float3(1.0, 0.0, 0.0), WriteFrom));
	const float3 along = cross(WriteFrom, across);
	const float3 r = world - float3(WriteTop.xy, WriteTopZ);
	return float3(float2(dot(r, across), dot(r, along)) / WriteTop.z, dot(world, WriteFrom));
}

float CoatTopAt(float2 local)
{
	const int2 p = clamp(int2(floor(float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5) * kCoatTileDim)), 0, int(kCoatTileDim) - 1);
	return CoatTop.Load(int3(p.x, int(WriteTop.w) + p.y, 0));
}

#		if defined(COAT_TOP)
struct VS_OUTPUT_TOP
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

VS_OUTPUT_TOP main(VS_INPUT_SKIN input)
{
	const float3 place = CoatTopPlace(SkinnedWorld(input));
	VS_OUTPUT_TOP output;
	output.Position = float4(place.xy, 0.5, 1.0);
	output.WorldZ = place.z;
	return output;
}
#		else
VS_OUTPUT_COAT main(VS_INPUT_SKIN input)
{
	const float3x4 skin = SkinnedMatrix(input);
	const float3 world = mul(float4(input.Position.xyz, 1.0), transpose(skin));
	const float4 model = float4(input.Position.xyz, 1.0);
	VS_OUTPUT_COAT output;
	output.Texel = clamp(float3(dot(WriteModelToAtlas[0], model), dot(WriteModelToAtlas[1], model), dot(WriteModelToAtlas[2], model)),
		WriteAtlasLo.xyz + 0.5, WriteAtlasHi.xyz - 0.5);
	output.Below = -1.0;
	output.Target = 1.0;
	output.Impact = 0.0;
	[branch] if (WriteContact > 0.5)
	{
		const float2 below = CoatBelowSnow(world);
		output.Below = below.x;
		output.Target = 1.0 - WriteImpactShare * (1.0 - below.y);
		output.Impact = WriteMoving * below.y;
	}
	output.Wet = SnowDeformation::UnderWater(world) ? 1.0 : 0.0;
	output.Growth = WriteGrowth;
	output.World = world;
	output.Open = 0.0;
	output.Deposit = 0.0;
	[branch] if (WriteFall > 0.5)
	{
		const float3 place = CoatTopPlace(world);
		float open = place.z >= CoatTopAt(place.xy) - WriteVoxel ? 1.0 : 0.0;
		[branch] if (open > 0.0 && WriteSky > 0.5)
			open = SphericalHarmonics::Unproject(Skylighting::Sample(world - WriteEye, WriteFrom), WriteFrom) > 0.5 ? 1.0 : 0.0;
		output.Open = open;
		const float3 from = WriteFrom.x * skin[0].xyz + WriteFrom.y * skin[1].xyz + WriteFrom.z * skin[2].xyz;
		output.Deposit = float3(dot(WriteModelToCanonical[0].xyz, from), dot(WriteModelToCanonical[1].xyz, from), dot(WriteModelToCanonical[2].xyz, from));
	}
	return output;
}
#		endif
#	endif

#	ifdef GSHADER
#		include "Common/GBuffer.hlsli"

static const uint kCoatLatticeSteps = 6;

uint CoatFallWord(float a_exposure, float3 a_up)
{
	const uint exposure = dot(a_up, a_up) > 1e-8 ? uint(saturate(a_exposure) * 255.0 + 0.5) : 0u;
	const float2 e = saturate(GBuffer::EncodeNormal(-a_up * rsqrt(max(dot(a_up, a_up), 1e-12))));
	return exposure != 0 ? (exposure << 16) | (uint(e.y * 255.0 + 0.5) << 8) | uint(e.x * 255.0 + 0.5) : 0u;
}

void CoatPoint(float3 a_texel, float a_below, float a_wet, float a_target, float a_impact, uint a_growth, uint a_fall, inout PointStream<GS_OUTPUT_COAT> a_stream)
{
	const uint flags = (a_below > 0.0 ? 8u : 0u) | (a_wet >= 0.5 ? 16u : 0u);
	[branch] if (flags != 0 || a_fall != 0)
	{
		GS_OUTPUT_COAT o;
		o.Position = float4(-1.0 + 1.0 / 2048.0, 1.0 - 1.0 / 2048.0, 0.5, 1.0);
		o.Voxel = uint4(uint3(int3(a_texel) + WriteScratchShift), a_growth | (uint(saturate(a_target) * 255.0 + 0.5) << 16) | (uint(saturate(a_impact) * 255.0 + 0.5) << 24));
		o.Fall = a_fall;
		const float3 low = floor(a_texel - 0.5);
		const uint3 reach = low + 1.0 < WriteAtlasHi.xyz ? 1u : 0u;
		o.Cell = uint4(uint3(int3(low) + WriteScratchShift), reach.x | (reach.y << 1) | (reach.z << 2) | flags);
		a_stream.Append(o);
	}
}

[maxvertexcount(28)]
void main(triangle VS_OUTPUT_COAT tri[3], inout PointStream<GS_OUTPUT_COAT> stream)
{
	const uint growth = uint(clamp(tri[0].Growth, 0.0, 255.0) * 256.0 + 0.5);
	const float3 n = cross(tri[1].World - tri[0].World, tri[2].World - tri[0].World);
	const float facing = max(max(tri[0].Open, tri[1].Open), tri[2].Open) > 0.0 ? abs(dot(n, WriteFrom)) * rsqrt(max(dot(n, n), 1e-12)) : 0.0;
	const float span = max(max(length(tri[1].Texel - tri[0].Texel), length(tri[2].Texel - tri[1].Texel)), length(tri[0].Texel - tri[2].Texel));
	const uint steps = clamp(uint(ceil(span)), 1u, kCoatLatticeSteps);
	[loop] for (uint i = 0; i <= steps; ++i)
	{
		[loop] for (uint j = 0; j <= steps - i; ++j)
		{
			const float3 b = float3(i, j, steps - i - j) / float(steps);
			const uint fall = facing > 0.0 ? CoatFallWord(facing * (b.x * tri[0].Open + b.y * tri[1].Open + b.z * tri[2].Open), b.x * tri[0].Deposit + b.y * tri[1].Deposit + b.z * tri[2].Deposit) : 0u;
			CoatPoint(b.x * tri[0].Texel + b.y * tri[1].Texel + b.z * tri[2].Texel, b.x * tri[0].Below + b.y * tri[1].Below + b.z * tri[2].Below,
				b.x * tri[0].Wet + b.y * tri[1].Wet + b.z * tri[2].Wet, b.x * tri[0].Target + b.y * tri[1].Target + b.z * tri[2].Target,
				b.x * tri[0].Impact + b.y * tri[1].Impact + b.z * tri[2].Impact, growth, fall, stream);
		}
	}
}
#	endif

#	ifdef PSHADER
RWTexture3D<uint> CoatContact : register(u1);
RWTexture3D<uint> CoatFall : register(u2);

float2 main(GS_OUTPUT_COAT input) : SV_Target
{
	[branch] if ((input.Cell.w & 8u) != 0)
		InterlockedMax(CoatContact[input.Voxel.xyz], input.Voxel.w);
	const bool wash = (input.Cell.w & 16u) != 0;
	[branch] if (wash || input.Fall != 0)
	{
		const uint3 reach = uint3(input.Cell.w, input.Cell.w >> 1, input.Cell.w >> 2) & 1u;
		[unroll] for (uint i = 0; i < 8; ++i)
		{
			const uint3 v = input.Cell.xyz + reach * uint3(i & 1u, (i >> 1) & 1u, i >> 2);
			[branch] if (wash)
				InterlockedOr(CoatFall[v], 0x80000000u);
			[branch] if (input.Fall != 0)
				InterlockedMax(CoatFall[v], input.Fall);
		}
	}
	return float2(1.0e30, 1.0e30);
}
#	endif

#	if defined(CSHADER) && defined(COAT_TILE)
#		define SNOW_SURFACE_ONLY
#		define SNOW_SURFACE_NO_BERM
#		include "SnowDeformation/SnowShell.hlsl"

cbuffer BodyCoatTileCB : register(b1)
{
	float4 TileOrigin[4];
	uint TileCount;
	uint3 TilePad;
}

RWTexture2D<float> CoatTileOut : register(u0);

float CoatSnowTop(float2 worldXY)
{
	float coverage, terrainHeight;
	float top = ShellSurfaceZ(worldXY - GridOrigin, coverage, terrainHeight);
	const float2 road = RoadRasterAt(worldXY);
	[flatten] if (road.y > kNoRoadTop * 0.5 && road.x >= 1.0)
		top = max(top, road.y + road.x);
	return top;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	[branch] if (id.z < TileCount)
	{
		const float4 o = TileOrigin[id.z];
		CoatTileOut[uint2(id.x, uint(o.w) + id.y)] = CoatSnowTop(o.xy + float2(id.xy) * o.z);
	}
}
#	elif defined(CSHADER)
#		include "Common/GBuffer.hlsli"
#		include "Common/Random.hlsli"

RWTexture3D<uint> CoatAtlasWords : register(u0);
static const uint kCoatImpactX = 32;
static const uint kCoatSlotsPerRow = 32;
static const uint kCoatNoScratch = 0xFFu;
RWTexture3D<uint> CoatContact : register(u1);
RWTexture3D<uint> CoatFall : register(u2);
RWByteAddressBuffer CoatChanged : register(u3);

cbuffer BodyCoatResolveCB : register(b0)
{
	uint4 ResolveSlots[16];
	uint ResolveCount;
	uint ResolveCap;
	uint ResolveSeed;
	uint ResolvePad;
	uint4 ResolveFade[32];
	float4 ResolveFall[32];
}

groupshared uint changedInGroup;

float3 CoatDeposit(uint a_bytes)
{
	return -GBuffer::DecodeNormal(float2(a_bytes & 0xFFu, (a_bytes >> 8) & 0xFFu) / 255.0);
}

uint CoatDepositBytes(float3 a_up)
{
	const float2 e = saturate(GBuffer::EncodeNormal(-a_up));
	return (uint(e.y * 255.0 + 0.5) << 8) | uint(e.x * 255.0 + 0.5);
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	if (index == 0)
		changedInGroup = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint k = id.z / 48u;
	[branch] if (k < ResolveCount)
	{
		const uint entry = ResolveSlots[k >> 2][k & 3];
		const uint slot = entry & 0xFFu;
		const uint scratch = entry >> 8;
		const uint3 t = uint3(id.x, (slot / kCoatSlotsPerRow) * 16u + id.y, (slot % kCoatSlotsPerRow) * 48u + id.z % 48u);
		const uint3 ts = uint3(id.xy, scratch * 48u + id.z % 48u);
		const bool drawn = scratch != kCoatNoScratch;
		const uint side = slot * 2u + (id.x >> 4);
		const uint fade = ResolveFade[side >> 2][side & 3];
		uint contact = 0u, fallWord = 0u;
		[branch] if (drawn)
		{
			contact = CoatContact[ts];
			fallWord = CoatFall[ts];
		}
		[branch] if ((contact | fallWord | fade) != 0)
		{
			const uint was = CoatAtlasWords[t];
			const uint3 ti = t + uint3(kCoatImpactX, 0u, 0u);
			const uint wasImpact = CoatAtlasWords[ti];
			uint r = was & 0xFFu;
			uint g = (was >> 8) & 0xFFu;
			uint i = wasImpact & 0xFFu;
			uint deposit = was >> 16;
			const float dither = float(Random::murmur3(t, ResolveSeed) >> 8) / 16777216.0;
			const uint target = (ResolveCap * ((contact >> 16) & 0xFFu) + 127u) / 255u;
			const uint impactTarget = (ResolveCap * (contact >> 24) + 127u) / 255u;
			const uint grow = uint(float(contact & 0xFFFFu) / 256.0 + dither);
			[branch] if (contact != 0 && r < target)
				r = min(r + grow, target);
			[branch] if (contact != 0 && i < impactTarget)
				i = min(i + grow, impactTarget);
			const uint fall = fallWord & 0xFFFFFFu;
			[branch] if (fall != 0)
			{
				const float grown = 255.0 - (255.0 - float(g)) * exp(-ResolveFall[side >> 2][side & 3] * float(fall >> 16) / 255.0);
				const uint add = uint(max(grown - float(g), 0.0) + dither);
				[branch] if (add != 0)
				{
					deposit = CoatDepositBytes(normalize(CoatDeposit(deposit) * float(g) + CoatDeposit(fall) * float(add)));
					g = min(g + add, 255u);
				}
			}
			[branch] if (fade != 0)
			{
				r -= min(r, uint(float(r * ((fade >> 16) & 0xFFu)) / 255.0 + dither));
				i -= min(i, uint(float(i * ((fade >> 16) & 0xFFu)) / 255.0 + dither));
				g -= min(g, uint(float(g * (fade >> 24)) / 255.0 + dither));
				r -= min(r, fade & 0xFFu);
				i -= min(i, fade & 0xFFu);
				g -= min(g, (fade >> 8) & 0xFFu);
			}
			[flatten] if ((fallWord >> 31) != 0)
			{
				r = 0;
				g = 0;
				i = 0;
			}
			const uint now = (deposit << 16) | (g << 8) | r;
			CoatAtlasWords[t] = now;
			CoatAtlasWords[ti] = i;
			[branch] if (drawn)
			{
				CoatContact[ts] = 0;
				CoatFall[ts] = 0;
			}
			[branch] if (now != was || i != wasImpact)
				InterlockedAdd(changedInGroup, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (index == 0 && changedInGroup != 0)
		CoatChanged.InterlockedAdd(0, changedInGroup);
}
#	endif
#endif
