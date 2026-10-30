// Contact height capture for moving props: each drawn render mesh writes its
// world Z from directly above into an R32G32F target with MIN blending, so a
// column holds the lowest surface standing over it this frame: bodies in R,
// loose props in G (the pass's write mask picks the channel). The stamp
// pass carves where that surface reaches into the snow layer. Drawn with
// culling off: the underside is the surface that touches the snow.
//
// COAT: the body coat's contact writer (BODY-SNOW-PLAN.md section 2.2), the
// same skinned draw again for an actor due an update. The VS tests each vertex
// against the drawn snow, the GS hands the results to the coat atlas's voxels,
// the PS raises them, and a compute pass resolves them into the atlas.
#ifndef COAT
// Layout prefix of StaticsCB (SnowDeformation.h) - only these fields are
// read, and a bound buffer may be larger than its declaration.
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
// Bone palette for one skin partition, built CPU-side as absolute world
// transforms - deliberately NOT the game's Bones buffer, whose rows carry a
// pivot subtracted out. Three float4 rows per bone, so a UNORM bone index
// times 765 addresses its first row, exactly as Lighting.hlsl does it.
cbuffer ContactSkinCB : register(b2)
{
	float4 BoneRows[240];
	float2 SkinWindowCenter;
	float SkinHalfExtent;
	// Bones in this partition's palette; an index past it names no bone.
	float SkinBoneCount;
}

struct VS_INPUT_SKIN
{
	float4 Position : POSITION0;
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
};

// The vertex's absolute world position under the partition's palette.
float3 SkinnedWorld(VS_INPUT_SKIN input)
{
	// Bone indices arrive as UNORM bytes; 765.01 = 255 * 3.0004 turns each
	// into the index of its first row, the game's own convention.
	int4 rows = int4(765.01 * input.BoneIndices);
	// The weights are half floats and sum to 1 only to about 2e-3. The rows
	// carry ABSOLUTE world translations (~1e5), so an unnormalized blend
	// shifts a vertex by that error times the world position - hundreds of
	// units, along the world position's own direction. The game's rows are
	// pivot-relative and never see this; ours must renormalize.
	// A weight on an index past the palette, or a vertex with no weight at
	// all, would blend zero rows and slide toward the world origin - the
	// comb. The game hides such vertices at its pivot; here they ride the
	// partition's first bone instead.
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
	float3x4 m =
		float3x4(BoneRows[rows.x], BoneRows[rows.x + 1], BoneRows[rows.x + 2]) * w.x +
		float3x4(BoneRows[rows.y], BoneRows[rows.y + 1], BoneRows[rows.y + 2]) * w.y +
		float3x4(BoneRows[rows.z], BoneRows[rows.z + 1], BoneRows[rows.z + 2]) * w.z +
		float3x4(BoneRows[rows.w], BoneRows[rows.w + 1], BoneRows[rows.w + 2]) * w.w;

	// The rows are absolute world, so this is the world position outright -
	// no object transform, and no pivot to add back.
	return mul(float4(input.Position.xyz, 1.0), transpose(m));
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

	// Ortho top-down, the height capture's convention: +worldY maps to +ndcY,
	// texture v = 0 at the top. The stamp pass mirrors this when it samples.
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
// A vertex's atlas texel (fractional), how far it lies under the drawn snow
// (positive: under), whether it is under the water, and the actor's adhesion.
struct VS_OUTPUT_COAT
{
	float3 Texel : TEXCOORD0;
	float Below : TEXCOORD1;
	float Wet : TEXCOORD2;
	float Adhesion : TEXCOORD3;
};

// One voxel to raise: the atlas texel, and flags (1 contact, 2 wash) with the
// adhesion in bits 8-15.
struct GS_OUTPUT_COAT
{
	float4 Position : SV_POSITION;
	nointerpolation uint4 Voxel : TEXCOORD0;
};

#	if defined(VSHADER) && defined(SKINNED)
#		define SNOW_SURFACE_ONLY
#		include "SnowDeformation/SnowShell.hlsl"
#		include "SnowDeformation/SnowDeformation.hlsli"

// The draw's shape (BodyCoat.cpp BodyCoatWriteCB): its record's model-to-atlas
// rows and slot box, and the actor's adhesion.
cbuffer BodyCoatWriteCB : register(b3)
{
	float4 WriteModelToAtlas[3];
	float4 WriteAtlasLo;
	float4 WriteAtlasHi;
	float WriteAdhesion;
	float3 WritePad;
}

// The drawn snow's top at a point: the ground's, or a road's untouched layer
// where the road raster carries one, the higher.
float CoatSnowTop(float2 worldXY)
{
	float coverage, terrainHeight;
	float top = ShellSurfaceZ(worldXY - GridOrigin, coverage, terrainHeight);
	const float2 road = RoadRasterAt(worldXY);
	[flatten] if (road.y > kNoRoadTop * 0.5 && road.x >= 1.0)
		top = max(top, road.y + road.x);
	return top;
}

// How far a point lies under the snow that touches it, a quarter texel of
// slack included: the snow one deformation texel out, the highest of four
// sides. Its own column holds the print it carved; the snow beside it is what
// it pushes against (section 2.3). Above the highest the layer can stand there
// (its class depth with the berm on top, plus undulation), nothing is read.
float CoatBelowSnow(float3 world)
{
	const float texel = 1.0 / max(DeformInvWorldSize * FieldDims.x, 1e-6);
	const float3 terrain = SampleTerrain(world.xy - GridOrigin);
	const float2 road = RoadRasterAt(world.xy);
	const float reach = max(terrain.x + max(terrain.y, 0.0) * (1.0 + BermHeightAmp) + UndulationAmp, road.y + road.x) + texel;
	float below = -1e6;
	[branch] if (world.z < reach)
	{
		const float top = max(max(CoatSnowTop(world.xy + float2(texel, 0.0)), CoatSnowTop(world.xy - float2(texel, 0.0))),
			max(CoatSnowTop(world.xy + float2(0.0, texel)), CoatSnowTop(world.xy - float2(0.0, texel))));
		below = top + 0.25 * texel - world.z;
	}
	return below;
}

VS_OUTPUT_COAT main(VS_INPUT_SKIN input)
{
	const float3 world = SkinnedWorld(input);
	const float4 model = float4(input.Position.xyz, 1.0);
	VS_OUTPUT_COAT output;
	// Clamped into the slot as Lighting clamps its read.
	output.Texel = clamp(float3(dot(WriteModelToAtlas[0], model), dot(WriteModelToAtlas[1], model), dot(WriteModelToAtlas[2], model)),
		WriteAtlasLo.xyz + 0.5, WriteAtlasHi.xyz - 0.5);
	output.Below = CoatBelowSnow(world);
	output.Wet = SnowDeformation::UnderWater(world) ? 1.0 : 0.0;
	output.Adhesion = WriteAdhesion;
	return output;
}
#	endif

#	ifdef GSHADER
// The lattice: a triangle wider than a voxel takes points every voxel or
// closer, up to this many steps an edge; (6 + 1)(6 + 2) / 2 points at most.
static const uint kCoatLatticeSteps = 6;

void CoatPoint(float3 a_texel, float a_below, float a_wet, uint a_adhesion, inout PointStream<GS_OUTPUT_COAT> a_stream)
{
	const uint flags = (a_below > 0.0 ? 1u : 0u) | (a_wet >= 0.5 ? 2u : 0u);
	[branch] if (flags != 0)
	{
		GS_OUTPUT_COAT o;
		// Pixel (0, 0) of the capture's target: every point is one PS invocation.
		o.Position = float4(-1.0 + 1.0 / 2048.0, 1.0 - 1.0 / 2048.0, 0.5, 1.0);
		o.Voxel = uint4(uint3(a_texel), flags | (a_adhesion << 8));
		a_stream.Append(o);
	}
}

[maxvertexcount(28)]
void main(triangle VS_OUTPUT_COAT tri[3], inout PointStream<GS_OUTPUT_COAT> stream)
{
	const uint adhesion = uint(saturate(tri[0].Adhesion) * 255.0 + 0.5);
	const float span = max(max(length(tri[1].Texel - tri[0].Texel), length(tri[2].Texel - tri[1].Texel)), length(tri[0].Texel - tri[2].Texel));
	const uint steps = clamp(uint(ceil(span)), 1u, kCoatLatticeSteps);
	[loop] for (uint i = 0; i <= steps; ++i)
	{
		[loop] for (uint j = 0; j <= steps - i; ++j)
		{
			const float3 b = float3(i, j, steps - i - j) / float(steps);
			CoatPoint(b.x * tri[0].Texel + b.y * tri[1].Texel + b.z * tri[2].Texel, b.x * tri[0].Below + b.y * tri[1].Below + b.z * tri[2].Below,
				b.x * tri[0].Wet + b.y * tri[1].Wet + b.z * tri[2].Wet, adhesion, stream);
		}
	}
}
#	endif

#	ifdef PSHADER
RWTexture3D<uint> CoatContact : register(u1);
RWTexture3D<uint> CoatWash : register(u2);

// Under the capture's MIN blend the field keeps what it holds.
float2 main(GS_OUTPUT_COAT input) : SV_Target
{
	const uint3 t = input.Voxel.xyz;
	[branch] if ((input.Voxel.w & 1u) != 0)
		InterlockedMax(CoatContact[t], input.Voxel.w >> 8);
	[branch] if ((input.Voxel.w & 2u) != 0)
		CoatWash[t] = 1u;
	return float2(1.0e30, 1.0e30);
}
#	endif

#	ifdef CSHADER
// The atlas as R8G8B8A8 words (R contact, G snowfall, BA the deposit direction).
RWTexture3D<uint> CoatAtlasWords : register(u0);
RWTexture3D<uint> CoatContact : register(u1);
RWTexture3D<uint> CoatWash : register(u2);
// Voxels this resolve changed, for the readout.
RWByteAddressBuffer CoatChanged : register(u3);

// BodyCoat.cpp BodyCoatResolveCB: the slots the writer drew into.
cbuffer BodyCoatResolveCB : register(b0)
{
	uint4 ResolveSlots[8];
	uint ResolveCount;
	uint3 ResolvePad;
}

groupshared uint changedInGroup;

// Contact raises R to the strongest touch; water clears R and G. The scratch
// is left at zero for the next update.
[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	if (index == 0)
		changedInGroup = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint k = id.z / 48u;
	[branch] if (k < ResolveCount)
	{
		const uint3 t = uint3(id.xy, ResolveSlots[k >> 2][k & 3] * 48u + id.z % 48u);
		const uint contact = CoatContact[t];
		const uint wash = CoatWash[t];
		[branch] if ((contact | wash) != 0)
		{
			const uint was = CoatAtlasWords[t];
			uint r = max(was & 0xFFu, contact);
			uint g = (was >> 8) & 0xFFu;
			[flatten] if (wash != 0)
			{
				r = 0;
				g = 0;
			}
			const uint now = (was & 0xFFFF0000u) | (g << 8) | r;
			CoatAtlasWords[t] = now;
			CoatContact[t] = 0;
			CoatWash[t] = 0;
			[branch] if (now != was)
				InterlockedAdd(changedInGroup, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (index == 0 && changedInGroup != 0)
		CoatChanged.InterlockedAdd(0, changedInGroup);
}
#	endif
#endif
