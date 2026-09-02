// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Object height-window processing.
//
// ScrollCS   persistence: carries the accumulated raw top/bottom maps into
//            the current window position (whole-texel offsets). The maps
//            must not depend on what the camera renders this frame; the
//            capture list is frustum-culled, and rebuilding from it alone
//            makes object heights vanish behind the camera.
// CombineCS  builds the base snow-height field and the shelter mask: a
//            structure floating well above the ground shelters what is under
//            it, as a soft melt to a dusting rather than a coverage kill.
//            Doors clear the field and add to the mask; fires write NEGATIVE
//            mask values, a melt fraction that thins depth toward a floor, so
//            fire pits keep a thin floor instead of sinking below terrain.
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
	float ObjectSnowDepth;  // rounded-class depth, for the object snow cone seed
	float RimStep;  // the seed's slope-discontinuity rim threshold (user knob)
	// "Ignore Cover Above" (user knob): a neighbouring surface more than
	// this far ABOVE is a separate world (wall, roof, railing) - it does
	// not split the plane; the dome keeps its height and clips through.
	// Rises within [RimStep, OverheadIgnore] still rim (stair treads).
	float OverheadIgnore;

	// "Meld Co-Planar Surfaces" A/B: >0.5 = the drop-bridge reaches 3
	// texels (same-height planes a sliver apart meld into one dome);
	// 0 = no bridging - every shell clings to its own raster edge and
	// nearby shells just clip into each other.
	float MeldPlanes;
	// P4 "Snow Settling": per-iteration Jacobi blend toward the 4-neighbour
	// average, applied to the finished cone depth fields (0 = off).
	float DiffuseLambda;
	float2 padHeight;
}

// Shelter melt strength: snow under roofs/tents/walkways thins to a light
// dusting (the shell keeps covering the ground - bare ground would expose
// the mismatched projected snow diffuse beneath).
#define SHELTER_MELT 0.9
// Soft-shelter ring radius in texels (4 world units each): widens the
// per-texel roofline test into a gradual transition band. 10 texels =
// a ~40-unit band, so a full-depth sink slopes at ~20 degrees instead
// of presenting a snow cliff at the roofline.
#define SHELTER_RING_TEXELS 10


#include "SnowDeformation/SnowExclusions.hlsli"

Texture2D<float> InA : register(t0);
Texture2D<float> InB : register(t1);
Texture2D<float4> TerrainWindow : register(t2);
// ObjectConeSeedCS only: the NEXT peeled layer's top map, for the
// continuation test behind tall cover (see the rise rim below).
Texture2D<float> InC : register(t3);
RWTexture2D<float> OutA : register(u0);
RWTexture2D<float> OutB : register(u1);
// CombineCS only: the shelter mask, TWO independent channels (R = door
// suppression, G = melt fraction). A single winner-takes-all channel
// discarded melt wherever a door's faint influence tail reached - a ring
// of full-depth plateau snow around every sheltered door.
RWTexture2D<float2> OutMask : register(u2);

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

// How strongly the raster at p reads as a floating structure above the
// given terrain height (walkway, roof, bridge, tent canvas). CONTINUOUS:
// a binary test quantized the ring average into visible melt terraces
// under eaves - stairs descending toward the wall.
float ShelterTap(int2 p, int2 dims, float terrain)
{
	p = clamp(p, int2(0, 0), dims - 1);
	float result = 0.0;
	float top = InA[p];
	[branch] if (top > -50000.0)
	{
		float bottom = InB[p];
		result = smoothstep(20.0, 60.0, bottom - terrain) * smoothstep(40.0, 80.0, top - terrain);
	}
	return result;
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
	float melt = 0.0;

	// Shelter only: grounded object tops deliberately do NOT raise the field
	// (an object-top "blanket" lift was tried and removed; it produced seams
	// against the landscape shell and 45-degree spike cones at range). The
	// raster feeds just the floating-structure test: a bottom well clear of
	// the ground with a top high above it is a walkway/roof/bridge, and the
	// ground beneath it is sheltered from snowfall. Consumed as MELT (thin
	// shell floor, snow texture kept - a coverage kill exposed the
	// mismatched projected snow beneath and cut a cliff at the roofline);
	// the per-texel test is binary, so a center + 8-tap ring fraction turns
	// the cut into a smooth sink under the eaves. Taps reuse the center
	// terrain height: terrain varies slowly at ring scale.
	{
		int2 texel = int2(dtid.xy);
		int2 dimsI = int2(dims);
		static const int2 kShelterRing[8] = {
			int2(SHELTER_RING_TEXELS, 0), int2(-SHELTER_RING_TEXELS, 0),
			int2(0, SHELTER_RING_TEXELS), int2(0, -SHELTER_RING_TEXELS),
			int2(7, 7), int2(7, -7), int2(-7, 7), int2(-7, -7)
		};
		static const int2 kShelterRingInner[8] = {
			int2(5, 0), int2(-5, 0), int2(0, 5), int2(0, -5),
			int2(4, 4), int2(4, -4), int2(-4, 4), int2(-4, -4)
		};
		float shelterFrac = ShelterTap(texel, dimsI, terrain) * 2.0;
		[unroll] for (uint ringI = 0; ringI < 8; ringI++)
			shelterFrac += ShelterTap(texel + kShelterRing[ringI], dimsI, terrain) +
			               ShelterTap(texel + kShelterRingInner[ringI], dimsI, terrain);
		shelterFrac /= 18.0;
		// Deliberately no edge noise: roofline sinks read best smooth (fire
		// bowls keep their noisy rims; sheltered snow follows the structure).
		melt = max(melt, SHELTER_MELT * saturate(shelterFrac));
	}

	// Exclusion zones: pull the field back to terrain, then either suppress
	// snow (doors: coverage fades to bare ground) or melt it (fires: depth
	// thins toward a floor). Shared with the wide field bake so the near and
	// far answers are the same function of the same constant buffer.
	{
		ExclusionResult exclusion = EvaluateExclusions(worldXY, terrain);
		field = lerp(field, terrain, exclusion.Flatten);
		suppress = max(suppress, exclusion.Suppress);
		melt = max(melt, exclusion.Melt);
	}

	OutA[dtid.xy] = field;
	OutMask[dtid.xy] = float2(suppress, melt);
}

// Object snow cone seed. InA = raw object top raster. OutA = snow DEPTH above
// the local surface, not an absolute height: a field in absolute height cannot
// climb from the terrain to the top of a tall narrow object within its own
// footprint, so stacked stones and pillars would carry no snow at all.
// Zero marks a rim the layer must taper to: off the footprint, or a column
// standing well above a neighbour (an internal step, e.g. one stone on
// another). The cone passes then raise the interior at the angle of repose.
[numthreads(8, 8, 1)] void ObjectConeSeedCS(uint3 dtid
											: SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float top = InA[dtid.xy];
	if (top < -50000.0) {
		OutA[dtid.xy] = 0.0;
		return;
	}

	// Per-texel seed, floored by the class constant. InB is the skin-depth
	// raster: ROADS write their own class there, so the cone over a road
	// interior reaches the road depth and the patch's verge blend can ride
	// it down to the landscape class - seeded at the constant alone, the
	// blend silently capped every road at min(road, landscape), which is how
	// Road Meshes stopped responding above the snow01 depth. The constant
	// floor is what keeps everything else byte-identical: non-carving
	// objects write ZERO depth to the raster, and an unfloored seed would
	// collapse their cone and with it every skin's edge taper.
	float texelDepth = max(InB[dtid.xy], 0.0);
	float seed = max(texelDepth, ObjectSnowDepth);

	// Internal rims: a LEVEL BREAK sheds the layer the same way the outer
	// silhouette does. Detected as a slope DISCONTINUITY, not an absolute
	// drop: on a continuous slope the drop to the neighbour matches the
	// drop continuing one texel beyond (second difference ~ 0), while a
	// stair tread or ledge drops the full step against a flat run. The old
	// absolute test (drop > max(seed, 8)) let the class depth bury every
	// step shallower than the slider - at depth 25 a whole staircase read
	// as ONE plane and the fillet arced across the treads. BRIDGED two
	// texels out: single empty texels are the cracks between walkway
	// boards, and TWO low/empty texels with a SAME-HEIGHT surface beyond
	// are the small gap between a stair assembly and the walkway it meets
	// - co-planar planes MELD across it (Josef's rule: separated by
	// height = distinct; same height but a sliver apart = one surface).
	// A real silhouette is empty far wider and still rims. The threshold
	// is the "Plane Split Step" knob.
	bool rim = false;
	[unroll] for (int i = 0; i < 4; i++)
	{
		int2 offs = int2(i == 0 ? 1 : (i == 1 ? -1 : 0), i == 2 ? 1 : (i == 3 ? -1 : 0));
		int2 p = int2(dtid.xy) + offs;
		if (any(p < 0) || any(p >= int2(dims)))
			continue;
		float n1 = InA[uint2(p)];
		float n2 = -100000.0;
		float n3 = -100000.0;
		int2 p2 = int2(dtid.xy) + offs * 2;
		[flatten] if (all(p2 >= 0) && all(p2 < int2(dims)))
			n2 = InA[uint2(p2)];
		int2 p3 = int2(dtid.xy) + offs * 3;
		[flatten] if (all(p3 >= 0) && all(p3 < int2(dims)))
			n3 = InA[uint2(p3)];
		// "Meld Co-Planar Surfaces" OFF = cling: no bridging at all, the
		// shell rolls at its own raster edge whatever sits nearby.
		float n = MeldPlanes > 0.5 ? max(max(n1, n2), n3) : n1;
		if (n < -50000.0) {
			rim = true;
			continue;
		}
		float drop = top - n;
		// Slope carrying on past the neighbour cancels the drop; a step
		// against a flat run keeps it in full.
		float carry = max(n1 - n2, 0.0);
		if (drop - carry > RimStep)
			rim = true;
		// SYMMETRIC (Josef's isolation sketch): a break UPWARD is a plane
		// boundary too, so every plank's dome rounds at BOTH edges and
		// clips invisibly into its neighbour instead of piling against
		// the riser. Unbridged (a crack can never fake a rise), with the
		// same slope-continuation cancel so ascending roofs and rock
		// flanks never self-rim.
		float rise = n1 - top;
		float riseCarry = max(n2 - n1, 0.0);
		if (n1 > -50000.0 && rise - riseCarry > RimStep) {
			if (rise < OverheadIgnore) {
				// A nearby plane (stair tread, low ledge): dome boundary.
				rim = true;
			} else {
				// Tall cover ("Ignore Cover Above") is only ignorable if
				// OUR plane actually CONTINUES beneath it - a porch floor
				// running under its roof, read from the next peeled
				// layer's top. A tread ENDING against a wall has nothing
				// of itself beyond the edge: the cover is "not there",
				// and neither is anything else - that edge is a
				// silhouette and must roll (the lifted-shelf bug).
				float under = InC[uint2(p)];
				if (under < -50000.0 || abs(under - top) > RimStep)
					rim = true;
			}
		}
	}

	OutA[dtid.xy] = rim ? 0.0 : seed;
}

// InA = depth field. OutA = one repose iteration at ConeStep. ConeCS cannot be
// reused here: its terrain clamp belongs to an absolute-height field and would
// pin a depth field to world Z.
[numthreads(8, 8, 1)] void ObjectConeCS(uint3 dtid
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

	OutA[dtid.xy] = max(h, 0.0);
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

// P3 (edge-research study): per-column SKY OPENNESS baked from the layer-1
// tops. InA = the full-res top raster; OutA = openness at HALF resolution
// (a soft field - half res quarters the bake). 1 = open sky. The skin
// scales its depth by this, so open tops carry full snow and columns shaded
// by tall neighbours thin toward a dusting; the vertical under-cover half
// (a surface below a higher top in its OWN column) stays per-vertex in
// ApplySkinLift, since it depends on the surface's height, not the column.
// Elevation-angle test per direction: a neighbour must stand meaningfully
// above the column (6-unit pad ignores kerbs and treads) and steeply
// (tan 0.35..1.3 ~ 19..52 degrees) to shade it; two radii per direction so
// both a close wall and a taller ridge further out register.
[numthreads(8, 8, 1)] void ObjectSkyOpenCS(uint3 dtid
										   : SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (dtid.x >= dims.x || dtid.y >= dims.y)
		return;
	uint2 inDims;
	InA.GetDimensions(inDims.x, inDims.y);
	int2 inMax = int2(inDims) - 1;
	int2 p = int2(dtid.xy) * 2;
	// Own surface = MAX over the 2x2 block this output texel covers, so a
	// column is never read as shaded by its own quantization.
	float h = max(max(InA[uint2(min(p, inMax))], InA[uint2(min(p + int2(1, 0), inMax))]),
		max(InA[uint2(min(p + int2(0, 1), inMax))], InA[uint2(min(p + int2(1, 1), inMax))]));
	if (h < -50000.0) {
		OutA[dtid.xy] = 1.0;
		return;
	}
	static const int2 kOpenDirs[8] = {
		int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1),
		int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1)
	};
	// Input texels are 4 world units (kHeightTexel); radii 6 and 14 = 24
	// and 56 units.
	static const float kOpenRadii[2] = { 6.0, 14.0 };
	float occ = 0.0;
	[unroll] for (uint d = 0; d < 8; d++)
	{
		float o = 0.0;
		[unroll] for (uint r = 0; r < 2; r++)
		{
			int2 q = clamp(p + kOpenDirs[d] * (int)kOpenRadii[r], int2(0, 0), inMax);
			float t = InA[uint2(q)];
			float distW = kOpenRadii[r] * 4.0 * length(float2(kOpenDirs[d]));
			[flatten] if (t > -50000.0)
				o = max(o, smoothstep(0.35, 1.3, ((t - h) - 6.0) / distW));
		}
		occ += o;
	}
	OutA[dtid.xy] = 1.0 - occ * (1.0 / 8.0);
}

// P4 (edge-research study): SETTLING - one Jacobi diffusion iteration over a
// cone DEPTH field. Every method in the accumulation literature carries a
// diffusion/blur step; this pipeline never had one. On a depth field it is
// safe by the Bridging law (absolute-height fields are structurally noisy;
// per-plane depth fields are immune): it rounds the rims' knees, pulls the
// dip between two near-touching shells partway up so their domes arch toward
// each other instead of meeting in a black slit, and denoises raster jitter
// - which also feeds the crest-freeze tap ring a smoother field. The cone
// has no sentinels (0 at rims and off-footprint), so plain averaging needs
// no guards; lambda <= 0.5 is unconditionally stable for this stencil.
[numthreads(8, 8, 1)] void ObjectConeDiffuseCS(uint3 dtid
											   : SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;
	float h = InA[dtid.xy];
	int2 c = int2(dtid.xy);
	int2 mx = int2(dims) - 1;
	float avg = (InA[uint2(clamp(c + int2(1, 0), int2(0, 0), mx))] +
					InA[uint2(clamp(c - int2(1, 0), int2(0, 0), mx))] +
					InA[uint2(clamp(c + int2(0, 1), int2(0, 0), mx))] +
					InA[uint2(clamp(c - int2(0, 1), int2(0, 0), mx))]) *
	            0.25;
	OutA[dtid.xy] = lerp(h, avg, saturate(DiffuseLambda));
}

// ---------------------------------------------------------------------------
// BLOB SNOW SHELL - placement (Spike 1).
//
// One thread per cell of a world-space grid at BlobSpacing; the cell is
// anchored to ABSOLUTE world coordinates (not to the scrolling window), so a
// blob's position and size are a pure function of where it is in the world
// and never crawl as the camera moves. Per peeled layer the cell samples the
// layer's top and its placement mask - the projected-snow weight rasterised
// by the capture - and appends one sphere where the mask passes. Radius and
// how far the sphere sits proud of the surface are hashed per cell; a partial
// mask thins the sphere rather than dropping it, so a Snow Fill below 100%
// reads as a soft edge, not a cliff.
//
// Output is an instance list plus DrawIndexedInstancedIndirect args; the
// instance count is bumped atomically here and capped at kBlobCap by BOTH the
// writer and the vertex shader, so an overflow can only lose blobs, never
// read past the buffer.
// ---------------------------------------------------------------------------
cbuffer BlobCB : register(b1)
{
	float BlobSpacing;
	float BlobSize;
	float BlobSizeNoise;
	float BlobJut;
	float BlobJutNoise;
	float BlobMaskThreshold;
	float BlobSeed;
	float BlobLayers;
	float BlobEdgesOnly;
	float BlobEdgeBand;
	float BlobEdgeDrop;
	float BlobRadius;
	float BlobRefZ;
	float padBlobB1;
	float padBlobB2;
	float padBlobB3;
}

Texture2D<float2> BlobMask1 : register(t4);
Texture2D<float2> BlobMask2 : register(t5);
Texture2D<float2> BlobMask3 : register(t6);
// Layers 4-6 (Spike 1b): per-frame peels and their masks.
Texture2D<float> BlobTop4 : register(t7);
Texture2D<float> BlobTop5 : register(t8);
Texture2D<float> BlobTop6 : register(t9);
Texture2D<float2> BlobMask4 : register(t10);
Texture2D<float2> BlobMask5 : register(t11);
Texture2D<float2> BlobMask6 : register(t12);
// Two float4 per blob: [centre.xyz, radius], [surface top z, layer, mask, 0].
RWStructuredBuffer<float4> OutBlobs : register(u3);
// DrawIndexedInstancedIndirect args; dword 1 is the instance count.
RWByteAddressBuffer OutBlobArgs : register(u4);
// Mirror of SnowDeformation.h kBlobCap and SnowStaticsShell.hlsl kBlobCap.
static const uint kBlobCap = 524288;

float BlobHash(int2 cell, uint salt)
{
	uint h = (uint)cell.x * 0x8da6b343u ^ (uint)cell.y * 0xd8163841u ^ salt * 0xcb1ab31fu;
	h ^= h >> 13;
	h *= 0x5bd1e995u;
	h ^= h >> 15;
	return float(h & 0x00FFFFFFu) / 16777216.0;
}

float BlobLayerTop(uint L, int2 p)
{
	switch (L) {
	case 0: return InA.Load(int3(p, 0));
	case 1: return InB.Load(int3(p, 0));
	case 2: return InC.Load(int3(p, 0));
	case 3: return BlobTop4.Load(int3(p, 0));
	case 4: return BlobTop5.Load(int3(p, 0));
	default: return BlobTop6.Load(int3(p, 0));
	}
}

// .x = placement mask, .y = fresh-top code (0 = no fragment this frame).
float2 BlobLayerMask(uint L, int2 p)
{
	switch (L) {
	case 0: return BlobMask1.Load(int3(p, 0));
	case 1: return BlobMask2.Load(int3(p, 0));
	case 2: return BlobMask3.Load(int3(p, 0));
	case 3: return BlobMask4.Load(int3(p, 0));
	case 4: return BlobMask5.Load(int3(p, 0));
	default: return BlobMask6.Load(int3(p, 0));
	}
}

static const int2 kBlobDirs[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

// Edge test: in some direction, no layer of the neighbour texel carries this
// plane on within BlobEdgeDrop of its height. A drop and a wall rising both
// count; the plane continuing at ANY layer index does not, so a shelf under
// a roof keeps its edge and loses the roof's.
bool BlobIsEdge(int2 t, int2 dims, float h, uint layers, uint band)
{
	// Flag, not an early return: a return inside an unrolled loop reads as
	// "potentially uninitialized" to fxc (X4000, the patch gate's old trap).
	bool edge = false;
	const uint half = max(band >> 1, 1u);
	const uint rings = (half == band) ? 1u : 2u;
	for (uint r = 0; r < rings && !edge; r++)
	{
		const int step = int(r == 0 ? band : half);
		for (uint d = 0; d < 8 && !edge; d++)
		{
			const int2 n = t + kBlobDirs[d] * step;
			if (any(n < 0) || any(n >= dims))
				continue;
			float best = 1e9;
			for (uint L = 0; L < layers; L++)
			{
				const float tn = BlobLayerTop(L, n);
				if (tn > -50000.0)
					best = min(best, abs(tn - h));
			}
			edge = best > BlobEdgeDrop;
		}
	}
	return edge;
}

[numthreads(8, 8, 1)] void BlobPlaceCS(uint3 dtid
									   : SV_DispatchThreadID)
{
	uint2 dims;
	InA.GetDimensions(dims.x, dims.y);
	const float spacing = max(BlobSpacing, 1.0);
	const float radius = clamp(BlobRadius, 64.0, HeightHalfExtent - 8.0);
	const uint cells = (uint)ceil(radius * 2.0 / spacing);
	if (dtid.x >= cells || dtid.y >= cells)
		return;

	// Absolute world cell: the placement square's lower-left corner floors
	// onto the spacing lattice, then the thread offsets from there.
	const int2 cellW = int2(floor((HeightWindowCenter - radius.xx) / spacing)) + int2(dtid.xy);
	const uint salt = (uint)max(BlobSeed, 0.0);
	const float2 jitter = float2(BlobHash(cellW, salt + 1u), BlobHash(cellW, salt + 2u));
	const float2 xy = (float2(cellW) + 0.5 + (jitter - 0.5) * 0.9) * spacing;

	const float2 local = xy - HeightWindowCenter;
	const float dist = length(local);
	if (dist >= radius)
		return;
	if (abs(local.x) >= HeightHalfExtent - 1.0 || abs(local.y) >= HeightHalfExtent - 1.0)
		return;
	// The outer half thins toward nothing, so the cap is never filled by far
	// cells before near ones get their turn.
	const float keep = saturate((radius - dist) / (radius * 0.5));
	if (keep < 1.0 && BlobHash(cellW, salt + 5u) > keep)
		return;
	// Same world->texel mapping as TexelWorldXY (v mirrors world +Y).
	const float texel = HeightHalfExtent * 2.0 / dims.x;
	const int2 t = int2(
		clamp(int((local.x + HeightHalfExtent) / texel), 0, int(dims.x) - 1),
		clamp(int((HeightHalfExtent - local.y) / texel), 0, int(dims.y) - 1));

	const float hSize = BlobHash(cellW, salt + 3u);
	const float hJut = BlobHash(cellW, salt + 4u);
	const uint layers = (uint)clamp(BlobLayers, 1.0, 6.0);
	const float threshold = saturate(BlobMaskThreshold);
	const uint band = max(1u, (uint)round(BlobEdgeBand / texel));
	const bool edgesOnly = BlobEdgesOnly > 0.5;

	for (uint L = 0; L < layers; L++)
	{
		// Height from the mask target's fresh channel, never the accumulated
		// top: that one decays 0.5/frame while the game culls the surface's
		// draw, and the sphere would ride the ghost down.
		const float2 mk = BlobLayerMask(L, t);
		const float m = mk.x;
		if (m < threshold || mk.y <= 0.0)
			continue;
		const float top = mk.y * 4096.0 - 2048.0 + BlobRefZ;
		if (edgesOnly && !BlobIsEdge(t, int2(dims), top, layers, band))
			continue;
		// Partial mask (the Snow Fill's soft edge, thin authored paint) thins
		// the sphere toward 60% rather than switching it off.
		const float maskT = saturate((m - threshold) / max(1.0 - threshold, 1e-3));
		const float r = max(BlobSize * (1.0 + BlobSizeNoise * (hSize * 2.0 - 1.0)) * lerp(0.6, 1.0, maskT), 0.25);
		// Jut 0.5 = centre on the surface (a hemisphere shows); 1 = the whole
		// sphere resting on top; toward 0 it sinks in.
		const float jut = saturate(BlobJut * (1.0 + BlobJutNoise * (hJut * 2.0 - 1.0)));
		const float centreZ = top + r * (2.0 * jut - 1.0);

		uint idx;
		OutBlobArgs.InterlockedAdd(4, 1u, idx);
		if (idx < kBlobCap)
		{
			OutBlobs[idx * 2] = float4(xy, centreZ, r);
			OutBlobs[idx * 2 + 1] = float4(top, float(L), m, 0.0);
		}
	}
}
