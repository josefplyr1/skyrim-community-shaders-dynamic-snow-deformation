// Contact height capture for moving props: each drawn render mesh writes its
// world Z from directly above into an R32G32F target with MIN blending, so a
// column holds the lowest surface standing over it this frame: bodies in R,
// loose props in G (the pass's write mask picks the channel). The stamp
// pass carves where that surface reaches into the snow layer. Drawn with
// culling off: the underside is the surface that touches the snow.
//
// COAT: the body coat's contact writer (BODY-SNOW-PLAN.md section 2.2), the
// same skinned draw again for an actor due an update. A compute pass first
// samples the drawn snow around each due actor (COAT_TILE); the VS tests each
// vertex against it, the GS hands the results to the coat atlas's voxels, the
// PS records them, and a compute pass builds them into the atlas. The same
// pass fades the coats (S4) with no point pass. Snowfall (S5) rides it: a
// draw of the actor along the snow's way (COAT_TOP), at its own scale and
// blended to the surface nearest the snow, first lays its tops for its
// self-cover.
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

// The partition's palette blended for the vertex: its rows take the vertex to
// its absolute world position.
float3x4 SkinnedMatrix(VS_INPUT_SKIN input)
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
	return float3x4(BoneRows[rows.x], BoneRows[rows.x + 1], BoneRows[rows.x + 2]) * w.x +
	       float3x4(BoneRows[rows.y], BoneRows[rows.y + 1], BoneRows[rows.y + 2]) * w.y +
	       float3x4(BoneRows[rows.z], BoneRows[rows.z + 1], BoneRows[rows.z + 2]) * w.z +
	       float3x4(BoneRows[rows.w], BoneRows[rows.w + 1], BoneRows[rows.w + 2]) * w.w;
}

// The vertex's absolute world position under the partition's palette.
float3 SkinnedWorld(VS_INPUT_SKIN input)
{
	// The rows are absolute world, so this is the world position outright -
	// no object transform, and no pivot to add back.
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
// (positive: under), whether it is under the water, the levels a touch grows
// the coat by this update, the share of the cap it may keep, and the share
// that comes with an impact (D30: pushing into fresh snow on the move, the
// lumps' only source). Snowfall (S5): its world position, whether falling
// snow reaches it (the sky open that way, its own body not in the way), and
// the way the snow comes from in its canonical frame.
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

// One voxel to raise: the atlas texel, and the growth in bits 0-15, in 1/256
// levels, the share of the cap in bits 16-23 and the impact's in bits 24-31.
// Fall: the snowfall's exposure in bits 16-23 over the deposit's two bytes,
// as the atlas holds B and A; 0 for none. Cell: the low corner of the voxels
// the look's filter reads at the point, in w along each axis (bits 0-2)
// whether the far corner stays inside the slot's box, and flags (8 contact,
// 16 wash).
struct GS_OUTPUT_COAT
{
	float4 Position : SV_POSITION;
	nointerpolation uint4 Voxel : TEXCOORD0;
	nointerpolation uint Fall : TEXCOORD1;
	nointerpolation uint4 Cell : TEXCOORD2;
};

// The side of a due actor's snow-top tile, in nodes (BodyCoat.cpp kCoatTileDim).
static const uint kCoatTileDim = 64;

#	if defined(VSHADER) && defined(SKINNED)
#		include "Common/SharedData.hlsli"
#		include "SnowDeformation/SnowDeformation.hlsli"
#		define SKYLIGHTING_PROBE_REGISTER t2
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if (defined(VSHADER) && defined(SKINNED)) || defined(GSHADER)
// The draw's shape (BodyCoat.cpp BodyCoatWriteCB): its record's model-to-atlas
// rows and slot box, the levels the actor's touches grow by this update, its
// heading since the last (world xy, unit), the share of the cap kept for the
// parts pushing into fresh snow (0 while it stands), its snow-top tile (xy its
// first node in world, z the node spacing, w its first row in CoatTile) and
// the deformation texel, the taps' reach. S5: whether this update tests
// contact and measures snowfall, and whether Skylighting's probes are bound;
// the record's model-to-canonical rows; the actor's tops (xy the centre of
// its window, z its half extent, w its first row in CoatTop); the eye the
// probes are placed about; a voxel's size, the self-cover's slack; the way
// the snow comes from (world, unit; round 2: tilted by the wind as the game's
// own falling snow is); and the height of the tops' centre. D30: how far it
// has moved since, in deformation texels up to 1, the weight of its impacts.
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
}
#	endif

#	if defined(VSHADER) && defined(SKINNED)
// The drawn snow's top around the actor, sampled on a grid this update (the
// COAT_TILE pass): the taps read it instead of the surface itself.
Texture2D<float> CoatTile : register(t0);
// The actor's own surface nearest the snow over each column, drawn along the
// snow's way this update (S5's self-cover).
Texture2D<float> CoatTop : register(t1);

// The tile's snow top at a point, bilinear between its nodes.
float CoatTileTop(float2 worldXY)
{
	const float2 g = clamp((worldXY - WriteTile.xy) / WriteTile.z, 0.0, kCoatTileDim - 1.001);
	const int2 i = int2(g);
	const float2 f = g - float2(i);
	const int row = int(WriteTile.w) + i.y;
	return lerp(lerp(CoatTile.Load(int3(i.x, row, 0)), CoatTile.Load(int3(i.x + 1, row, 0)), f.x),
		lerp(CoatTile.Load(int3(i.x, row + 1, 0)), CoatTile.Load(int3(i.x + 1, row + 1, 0)), f.x), f.y);
}

// How far a point lies under the snow that touches it (x), a quarter texel of
// slack included: the snow one deformation texel out, the highest of four
// sides. Its own column holds the print it carved; the snow beside it is what
// it pushes against (section 2.3). And how squarely it moves into that snow
// (y, 0-1): how far the snow stands over it ahead along the heading, past the
// side behind, against the most it stands over it on any side (fresh snow
// ahead, the trench behind).
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

// A point in the actor's tops: across the snow's way over its window (xy, -1
// to 1, on two axes made alike in both passes) and how far along the way the
// snow comes from it lies (z).
float3 CoatTopPlace(float3 world)
{
	const float3 across = normalize(cross(float3(1.0, 0.0, 0.0), WriteFrom));
	const float3 along = cross(WriteFrom, across);
	const float3 r = world - float3(WriteTop.xy, WriteTopZ);
	return float3(float2(dot(r, across), dot(r, along)) / WriteTop.z, dot(world, WriteFrom));
}

// The highest the actor's own body reaches toward the snow at a place in its
// tops: the texel, laid as the capture lays its field (v = 0 at the top).
float CoatTopAt(float2 local)
{
	const int2 p = clamp(int2(floor(float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5) * kCoatTileDim)), 0, int(kCoatTileDim) - 1);
	return CoatTop.Load(int3(p.x, int(WriteTop.w) + p.y, 0));
}

#		if defined(COAT_TOP)
// S5's tops pass: the actor drawn along the snow's way, the blend keeping the
// highest per texel; the capture's own PS writes the height it is handed.
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
	// Clamped into the slot as Lighting clamps its read.
	output.Texel = clamp(float3(dot(WriteModelToAtlas[0], model), dot(WriteModelToAtlas[1], model), dot(WriteModelToAtlas[2], model)),
		WriteAtlasLo.xyz + 0.5, WriteAtlasHi.xyz - 0.5);
	output.Below = -1.0;
	output.Target = 1.0;
	output.Impact = 0.0;
	[branch] if (WriteContact > 0.5)
	{
		const float2 below = CoatBelowSnow(world);
		output.Below = below.x;
		// All of the cap pushing into fresh snow, Impact Share less of it elsewhere.
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
		// Snow reaches a point its own body is no more than a voxel in front of
		// along the snow's way (not a mouth, nor under a hood's brim or a cloak;
		// BODY-SNOW-PLAN.md section 2.4), where the sky is open that way as
		// Wetness Effects reads it for rain.
		const float3 place = CoatTopPlace(world);
		float open = place.z >= CoatTopAt(place.xy) - WriteVoxel ? 1.0 : 0.0;
		[branch] if (open > 0.0 && WriteSky > 0.5)
			open = SphericalHarmonics::Unproject(Skylighting::Sample(world - WriteEye, WriteFrom), WriteFrom) > 0.5 ? 1.0 : 0.0;
		output.Open = open;
		// The way the snow comes from, in the shape's own space through the skin's
		// rotation taken back (its transpose), then on into the canonical frame.
		const float3 from = WriteFrom.x * skin[0].xyz + WriteFrom.y * skin[1].xyz + WriteFrom.z * skin[2].xyz;
		output.Deposit = float3(dot(WriteModelToCanonical[0].xyz, from), dot(WriteModelToCanonical[1].xyz, from), dot(WriteModelToCanonical[2].xyz, from));
	}
	return output;
}
#		endif
#	endif

#	ifdef GSHADER
#		include "Common/GBuffer.hlsli"

// The lattice: a triangle wider than a voxel takes points every voxel or
// closer, up to this many steps an edge; (6 + 1)(6 + 2) / 2 points at most.
static const uint kCoatLatticeSteps = 6;

// A point's snowfall word: its exposure over the deposit as the atlas holds
// it, B then A. The deposit is GBuffer's encoding of the way down, which puts
// up at the octahedron's centre: GBuffer encodes -n, so up itself would sit
// on all four corners, and the atlas's filter between two of them reads down.
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
		// Pixel (0, 0) of the capture's target: every point is one PS invocation.
		o.Position = float4(-1.0 + 1.0 / 2048.0, 1.0 - 1.0 / 2048.0, 0.5, 1.0);
		o.Voxel = uint4(uint3(a_texel), a_growth | (uint(saturate(a_target) * 255.0 + 0.5) << 16) | (uint(saturate(a_impact) * 255.0 + 0.5) << 24));
		o.Fall = a_fall;
		// The texel lies at least half a voxel inside the box, as the look clamps it.
		const float3 low = floor(a_texel - 0.5);
		const uint3 reach = low + 1.0 < WriteAtlasHi.xyz ? 1u : 0u;
		o.Cell = uint4(uint3(low), reach.x | (reach.y << 1) | (reach.z << 2) | flags);
		a_stream.Append(o);
	}
}

[maxvertexcount(28)]
void main(triangle VS_OUTPUT_COAT tri[3], inout PointStream<GS_OUTPUT_COAT> stream)
{
	const uint growth = uint(clamp(tri[0].Growth, 0.0, 255.0) * 256.0 + 0.5);
	// Snow settles on a face by how squarely it faces the way the snow comes
	// from, on either side: the deposit keeps the side it lay on, and its own
	// body covers the far side.
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
// The snowfall's largest word in bits 0-23 (its exposure over its deposit),
// and the wash in bit 31, which clears the voxel whatever fell.
RWTexture3D<uint> CoatFall : register(u2);

// Under the capture's MIN blend the field keeps what it holds. Contact raises
// the point's own voxel; the wash and the snowfall go to every voxel the look's
// filter reads at the point, so a full coat reads full (S5 round 4).
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

// BodyCoat.cpp BodyCoatTileCB: each due actor's tile, its first node (world
// xy), node spacing and first row in CoatTileOut.
cbuffer BodyCoatTileCB : register(b1)
{
	float4 TileOrigin[4];
	uint TileCount;
	uint3 TilePad;
}

RWTexture2D<float> CoatTileOut : register(u0);

// The drawn snow's top at a point, without the berm (the spoil beside a
// trench is loose): the ground's, or a road's untouched layer where the road
// raster carries one, the higher.
float CoatSnowTop(float2 worldXY)
{
	float coverage, terrainHeight;
	float top = ShellSurfaceZ(worldXY - GridOrigin, coverage, terrainHeight);
	const float2 road = RoadRasterAt(worldXY);
	[flatten] if (road.y > kNoRoadTop * 0.5 && road.x >= 1.0)
		top = max(top, road.y + road.x);
	return top;
}

// One node of a due actor's tile: the surface the coat's four taps read,
// sampled once an update instead of four times a vertex.
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

// The atlas as R8G8B8A8 words (R contact, G snowfall, BA the deposit direction),
// and beside it along x the impact store (D30: R the contact that came with an
// impact, the lumps' only source).
RWTexture3D<uint> CoatAtlasWords : register(u0);
// BodyCoat.cpp kBodyCoatSlotDim[0]: the impact store's offset.
static const uint kCoatImpactX = 32;
RWTexture3D<uint> CoatContact : register(u1);
// Bits 0-23 the snowfall (exposure over deposit), bit 31 the wash.
RWTexture3D<uint> CoatFall : register(u2);
// Voxels this resolve changed, for the readout.
RWByteAddressBuffer CoatChanged : register(u3);

// BodyCoat.cpp BodyCoatResolveCB: the slots the writer drew into or that fade,
// the contact cap in atlas levels, this update's rounding seed, and each slot
// half's fade (S4, index slot x 2 + half): bits 0-7 the contact levels
// melted, 8-15 the snowfall's, 16-23 the share of contact shaken off in
// 1/255, 24-31 the snowfall's. S5: each half's snowfall this update, the
// fill's exponent at full exposure (intensity x the game time since its last
// / the fill time), four halves to a register.
cbuffer BodyCoatResolveCB : register(b0)
{
	uint4 ResolveSlots[8];
	uint ResolveCount;
	uint ResolveCap;
	uint ResolveSeed;
	uint ResolvePad;
	uint4 ResolveFade[16];
	float4 ResolveFall[16];
}

groupshared uint changedInGroup;

// The deposit as the atlas's two bytes hold it (B low) and back: GBuffer's
// encoding of the way down, so up sits at the octahedron's centre.
float3 CoatDeposit(uint a_bytes)
{
	return -GBuffer::DecodeNormal(float2(a_bytes & 0xFFu, (a_bytes >> 8) & 0xFFu) / 255.0);
}

uint CoatDepositBytes(float3 a_up)
{
	const float2 e = saturate(GBuffer::EncodeNormal(-a_up));
	return (uint(e.y * 255.0 + 0.5) << 8) | uint(e.x * 255.0 + 0.5);
}

// Contact grows R toward the point's share of the cap by the touch's growth, a
// fraction of a level rounded up at random in proportion, so slow growth still
// adds up, and the impact store toward the impact's share the same way (D30).
// Snowfall fills G by the room left, exactly over the time since the actor's
// last (BODY-SNOW-PLAN.md section 2.4), and the deposit leans toward the new
// snow's by its share of what lies there. A fade shakes off its share of R and
// G, the impact's with R, rounded the same way, then melts its whole levels.
// Water clears them all. The scratch is left at zero for the next update.
[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	if (index == 0)
		changedInGroup = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint k = id.z / 48u;
	[branch] if (k < ResolveCount)
	{
		const uint slot = ResolveSlots[k >> 2][k & 3];
		const uint3 t = uint3(id.xy, slot * 48u + id.z % 48u);
		// A component holds half a slot, 16 texels across.
		const uint side = slot * 2u + (id.x >> 4);
		const uint fade = ResolveFade[side >> 2][side & 3];
		const uint contact = CoatContact[t];
		const uint fallWord = CoatFall[t];
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
			CoatContact[t] = 0;
			CoatFall[t] = 0;
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
