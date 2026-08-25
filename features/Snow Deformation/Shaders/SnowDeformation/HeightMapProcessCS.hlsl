// Object height-window processing.
//
// ScrollCS   persistence: carries the accumulated raw top/bottom maps into
//            the current window position (whole-texel offsets). The maps
//            must not depend on what the camera renders this frame; the
//            capture list is frustum-culled, and rebuilding from it alone
//            makes object heights vanish behind the camera.
// CombineCS  builds the base snow-height field (terrain, lifted only by
//            corpse burial mounds) and the shelter mask: where the raw maps
//            show a structure floating well above the ground (walkways,
//            roofs, bridges), the ground beneath is sheltered from snowfall
//            (no snow under roofs).
//            Exclusion zones (doors, campfires) clear the field and add to
//            the mask before the cone runs, so surrounding snow re-slopes
//            into every clearing at the angle of repose.
// ConeCS     angle of repose: iterative min-plus cone transform. No point of
//            the field may rise steeper than SlopePerUnit from its
//            neighbors, so thin or tall features barely lift the field while
//            broad raises settle into natural mounds.
//
// Sentinels: top empty = -100000, bottom empty = +100000.

cbuffer HeightProcessCB : register(b0)
{
	int2 ScrollDelta;
	uint ClearAll;
	uint ConeStep;  // texel step for this cone iteration

	float2 HeightWindowCenter;
	float HeightHalfExtent;
	float SlopePerUnit;  // max rise per world unit (1.0 = 45 degrees)

	float2 TerrainWindowOrigin;  // world XY of terrain window texel (0,0)
	float TerrainTexelSize;
	uint TerrainDim;

	float GhostDecay;  // units/frame the accumulated maps drift toward empty
	float2 DeformWindowOriginH;  // deformation-map addressing (refill gate)
	float DeformInvWorldSizeH;

	uint CorpseSphereCount;  // resting dead actors' collision spheres
	float CorpseMoundCap;    // max mound height above terrain
	float2 padH;
	float4 CorpseSpheres[32];  // xyz world center, w radius
}

#define MAX_EXCLUSIONS 96

cbuffer DoorCB : register(b1)
{
	float4 ExclusionPosRadius[MAX_EXCLUSIONS];   // xyz = position, w = radius
	float4 ExclusionDirExtType[MAX_EXCLUSIONS];  // xy = facing, z = forward extent, w = type (0 door, 1 fire)
	uint ExclusionCount;
	float3 exclusionPad;
}

Texture2D<float> InA : register(t0);
Texture2D<float> InB : register(t1);
Texture2D<float4> TerrainWindow : register(t2);
Texture2D<float> DeformMap : register(t3);  // CombineCS: corpse-mound refill gate
RWTexture2D<float> OutA : register(u0);
RWTexture2D<float> OutB : register(u1);

// Cheap value noise for organic clearing edges.
float ExclusionNoise(float2 worldXY)
{
	float2 c = worldXY / 24.0;
	float2 i = floor(c);
	float2 f = frac(c);
	f = f * f * (3.0 - 2.0 * f);
	float4 h;
	h.x = frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453);
	h.y = frac(sin(dot(i + float2(1, 0), float2(127.1, 311.7))) * 43758.5453);
	h.z = frac(sin(dot(i + float2(0, 1), float2(127.1, 311.7))) * 43758.5453);
	h.w = frac(sin(dot(i + float2(1, 1), float2(127.1, 311.7))) * 43758.5453);
	return lerp(lerp(h.x, h.y, f.x), lerp(h.z, h.w, f.x), f.y);
}

// World XY of a height-map texel (v axis mirrors world +Y).
float2 TexelWorldXY(uint2 p, uint2 dims)
{
	float texel = HeightHalfExtent * 2.0 / dims.x;
	return float2(
		HeightWindowCenter.x + (float(p.x) - dims.x * 0.5 + 0.5) * texel,
		HeightWindowCenter.y + (dims.y * 0.5 - float(p.y) - 0.5) * texel);
}

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

[numthreads(8, 8, 1)] void ScrollCS(uint3 dtid
									: SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float top = -100000.0;
	float bottom = 100000.0;

	if (!ClearAll) {
		int2 src = int2(dtid.xy) + ScrollDelta;
		if (all(src >= 0) && all(src < int2(dims))) {
			// Ghost decay: accumulated heights fade unless re-rasterized this
			// frame; live objects re-assert themselves every frame, but
			// stale imprints (disabled/harvested/moved objects) melt away
			// instead of persisting until the window scrolls past them.
			top = InA[uint2(src)] - GhostDecay;
			bottom = InB[uint2(src)] + GhostDecay;
		}
	}

	OutA[dtid.xy] = top;
	OutB[dtid.xy] = bottom;
}

// InA = raw tops, InB = raw bottoms. OutA = base field, OutB = shelter mask.
[numthreads(8, 8, 1)] void CombineCS(uint3 dtid
									 : SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float2 worldXY = TexelWorldXY(dtid.xy, dims);
	float terrain = SampleTerrainHeight(worldXY);
	float field = terrain;
	float suppress = 0.0;

	// Shelter only: grounded object tops deliberately do NOT raise the field
	// (an object-top "blanket" lift was tried and removed; it produced seams
	// against the landscape shell and 45-degree spike cones at range). The
	// raster feeds just the floating-structure test: a bottom well clear of
	// the ground with a top high above it is a walkway/roof/bridge, and the
	// ground beneath it is sheltered from snowfall.
	float top = InA[dtid.xy];
	[branch] if (top > -50000.0)
	{
		float bottom = InB[dtid.xy];
		if (bottom - terrain >= 40.0 && top - terrain > 60.0)
			suppress = 1.0;  // floating structure: bare ground beneath
	}

	// Exclusion zones: pull the field back to terrain and suppress snow.
	// Doors use an ELLIPSE stretched along their facing axis (both ways;
	// the recess and the doorstep); campfires use a noisy-edged circle for
	// an organic melt ring. Z-gated (300) so upper-floor doors do not clear
	// ground snow far below, while sunken cave entrances still qualify.
	for (uint exclusionI = 0; exclusionI < ExclusionCount; exclusionI++) {
		float3 center = ExclusionPosRadius[exclusionI].xyz;
		float radius = ExclusionPosRadius[exclusionI].w;
		float4 dirExtType = ExclusionDirExtType[exclusionI];
		[branch] if (abs(center.z - terrain) < 300.0)
		{
			float2 d = worldXY - center.xy;
			float influence;
			[branch] if (dirExtType.w < 0.5)
			{
				// Door: symmetric ellipse, long axis along the facing, edge
				// perturbed by the same noise as fire clearings so no two
				// doorway hollows read as identical stamped shapes.
				float u = dot(d, dirExtType.xy);
				float v = dot(d, float2(-dirExtType.y, dirExtType.x));
				float a = radius + dirExtType.z;
				float b = radius * 0.85;
				float e = sqrt((u * u) / (a * a) + (v * v) / (b * b));
				e /= 0.8 + 0.4 * ExclusionNoise(worldXY);
				influence = 1.0 - smoothstep(0.45, 1.0, e);
			}
			else
			{
				// Campfire: melt ring with a noise-perturbed edge.
				float noisyRadius = radius * (0.8 + 0.5 * ExclusionNoise(worldXY));
				float dist = length(d);
				influence = 1.0 - smoothstep(noisyRadius * 0.4, noisyRadius, dist);
			}
			field = lerp(field, terrain, influence);
			suppress = max(suppress, influence);
		}
	}

	// Corpse burial mounds: resting dead actors inject their collision-
	// sphere caps as field tops; CAPPED above terrain so a mammoth makes a
	// bump, not a hill; gated by the LOCAL REFILL state so the story reads
	// fall -> imprint -> snow closes -> mound swells over the buried body.
	// Stateless: loot or move the corpse and the mound is gone next frame.
	// The cone transform downstream rounds every mound into a natural lump.
	for (uint corpseI = 0; corpseI < CorpseSphereCount; corpseI++) {
		float4 sphere = CorpseSpheres[corpseI];
		float2 dc = worldXY - sphere.xy;
		float sqDist = dot(dc, dc);
		[branch] if (sqDist < sphere.w * sphere.w)
		{
			// Grounded corpses only: a body on a high ledge must not mound
			// the snow far below it.
			[branch] if (sphere.z - sphere.w - terrain < 40.0)
			{
				float capZ = sphere.z + sqrt(sphere.w * sphere.w - sqDist);
				float mound = min(capZ, terrain + CorpseMoundCap);

				float deform = 0.0;
				float2 deformUV = (worldXY - DeformWindowOriginH) * DeformInvWorldSizeH;
				[branch] if (all(deformUV >= 0.0) && all(deformUV <= 1.0))
				{
					float2 deformDims;
					DeformMap.GetDimensions(deformDims.x, deformDims.y);
					deform = DeformMap.Load(int3(int2(deformUV * deformDims), 0));
				}
				// Refill first, then the mound: no mound while the death
				// imprint is still carved open.
				float refillGate = 1.0 - saturate(deform / 0.3);
				field = max(field, lerp(terrain, mound, refillGate));
			}
		}
	}

	OutA[dtid.xy] = field;
	OutB[dtid.xy] = suppress;
}

// InA = field. OutA = slope-limited field (one iteration at ConeStep).
[numthreads(8, 8, 1)] void ConeCS(uint3 dtid
								  : SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float texel = HeightHalfExtent * 2.0 / dims.x;
	float h = InA[dtid.xy];

	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			if (dx == 0 && dy == 0)
				continue;
			int2 p = int2(dtid.xy) + int2(dx, dy) * int(ConeStep);
			if (any(p < 0) || any(p >= int2(dims)))
				continue;
			float dist = length(float2(dx, dy)) * ConeStep * texel;
			h = min(h, InA[uint2(p)] + SlopePerUnit * dist);
		}
	}

	// The field can never sink below the actual terrain.
	float terrain = SampleTerrainHeight(TexelWorldXY(dtid.xy, dims));
	OutA[dtid.xy] = max(h, terrain);
}
