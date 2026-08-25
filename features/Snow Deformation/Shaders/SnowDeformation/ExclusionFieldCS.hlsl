// Wide exclusion field bake.
//
// The near mask (CombineCS) carries clearings only as far as the object height
// window reaches - about 57 m - because it lives in a texture sized for the
// SHELTER term, which needs a top-down render of real geometry. Clearings
// themselves need no geometry at all: a fire is a position and a radius. This
// pass evaluates them over a far wider window so a camp read from a hillside
// still shows its melted ground.
//
// Coarse on purpose. Clearings are 100-450 units across with soft, noisy
// edges, so a 32-unit texel resolves them with room to spare - the same
// reasoning that let the berm field be baked.

#include "SnowDeformation/SnowExclusions.hlsli"

// Terrain heights for the exclusion Z gate. The shell's window covers far more
// ground than this field does, at 128-unit texels.
Texture2D<float4> TerrainWindow : register(t0);
RWTexture2D<float2> OutField : register(u0);

cbuffer ExclusionFieldCB : register(b0)
{
	// Texel-snapped world XY at the centre of the field window.
	float2 FieldCenter;
	// World units from the centre to an edge.
	float FieldHalfExtent;
	float FieldTexelSize;

	// Terrain window addressing, mirroring the shell's SampleTerrain.
	float2 TerrainWindowOrigin;
	float TerrainTexelSize;
	uint TerrainDim;
}

// Threads per group per axis; the tile list below is shared by the whole group.
#define TILE_DIM 16

groupshared uint TileCount;
groupshared uint TileList[MAX_EXCLUSIONS];

float2 FieldTexelWorldXY(uint2 texel, float2 dims)
{
	return FieldCenter + (float2(texel) - dims * 0.5 + 0.5) * FieldTexelSize;
}

// Bilinear terrain height, matching the shell's own sampling so the Z gate
// agrees with the surface the clearing is judged against.
float SampleTerrainHeight(float2 worldXY)
{
	float2 t = (worldXY - TerrainWindowOrigin) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));

	float s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).x;

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

[numthreads(TILE_DIM, TILE_DIM, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gindex : SV_GroupIndex)
{
	float2 dims;
	OutField.GetDimensions(dims.x, dims.y);

	// Per-group cull. A brute-force loop would be 256 exclusions for every one
	// of a million texels; almost every tile is touched by none of them, so
	// each group tests the list once against its own bounds and the texels
	// below iterate only over survivors.
	if (gindex == 0)
		TileCount = 0;
	GroupMemoryBarrierWithGroupSync();

	{
		float2 tileMin = FieldTexelWorldXY(gid.xy * TILE_DIM, dims) - FieldTexelSize * 0.5;
		float2 tileMax = tileMin + FieldTexelSize * TILE_DIM;
		for (uint testI = gindex; testI < ExclusionCount; testI += TILE_DIM * TILE_DIM) {
			// Sealed containers (type 3) are deliberately NEAR-ONLY. Every
			// other clearing is 100-450 units across, which a 32-unit texel
			// resolves with room to spare; a sarcophagus footprint is around
			// 50, so this field cannot represent one, and bilinear spread
			// would ring it with bare ground WIDER than the coffin itself -
			// a worse artefact than the snow it removes. The near mask runs
			// at 4-unit texels and owns them; nobody reads the inside of a
			// coffin from 60 m out.
			if (ExclusionDirExtType[testI].w > 2.5)
				continue;
			float3 center = ExclusionPosRadius[testI].xyz;
			// Widest reach any type can have from its centre: doors extend by
			// their forward extent, fire bowls by their noise (0.7 + 0.35 +
			// 0.35 = 1.4 at the maximum).
			float reach = ExclusionPosRadius[testI].w * 1.4 + max(ExclusionDirExtType[testI].z, 0.0);
			float2 closest = clamp(center.xy, tileMin, tileMax);
			[branch] if (dot(closest - center.xy, closest - center.xy) <= reach * reach)
			{
				uint slot;
				InterlockedAdd(TileCount, 1, slot);
				if (slot < MAX_EXCLUSIONS)
					TileList[slot] = testI;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (any(float2(dtid.xy) >= dims))
		return;

	float suppress = 0.0;
	float melt = 0.0;
	uint tileCount = min(TileCount, MAX_EXCLUSIONS);
	[branch] if (tileCount > 0)
	{
		float2 worldXY = FieldTexelWorldXY(dtid.xy, dims);
		float terrain = SampleTerrainHeight(worldXY);
		// EvaluateExclusions walks the whole buffer; feed it the culled list by
		// evaluating per surviving entry instead.
		for (uint listI = 0; listI < tileCount; listI++) {
			ExclusionResult one = EvaluateExclusionAt(TileList[listI], worldXY, terrain);
			suppress = max(suppress, one.Suppress);
			melt = max(melt, one.Melt);
		}
	}

	OutField[dtid.xy] = float2(suppress, melt);
}
