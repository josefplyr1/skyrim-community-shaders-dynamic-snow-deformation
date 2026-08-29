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
	// >0.5: this seed/iterate dispatch builds an ABSOLUTE reposed snow
	// SURFACE (Snow Bridging) instead of a per-plane depth field: seeds are
	// top+depth (top alone at silhouette rims, +100000 where empty - a
	// no-op under min-plus), internal steps bury themselves under the
	// slope limit with no rim test, and the iterate skips the >=0 clamp
	// (absolute world z may be negative). Per dispatch, not per frame:
	// roads/patch/PS keep reading the classic depth cone.
	float BridgeMode;
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
		// Bridged: empty columns must not constrain the min-plus surface
		// (and near seeds they pick up propagated values, which is what
		// keeps edge bilinear taps sane). Classic: empty = rim.
		OutA[dtid.xy] = BridgeMode > 0.5 ? 100000.0 : 0.0;
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
	// as ONE plane and the fillet arced across the treads. BRIDGED one
	// texel out as before: the cracks between walkway boards are single
	// empty texels at this raster's 4-unit resolution, and treating each
	// as a rim pinched every board into its own pillow with holes between;
	// a real silhouette is empty for many texels and still rims. The
	// threshold is the "Plane Split Step" knob.
	bool rim = false;
	[unroll] for (int i = 0; i < 4; i++)
	{
		int2 offs = int2(i == 0 ? 1 : (i == 1 ? -1 : 0), i == 2 ? 1 : (i == 3 ? -1 : 0));
		int2 p = int2(dtid.xy) + offs;
		if (any(p < 0) || any(p >= int2(dims)))
			continue;
		float n1 = InA[uint2(p)];
		float n2 = -100000.0;
		int2 p2 = int2(dtid.xy) + offs * 2;
		[flatten] if (all(p2 >= 0) && all(p2 < int2(dims)))
			n2 = InA[uint2(p2)];
		float n = max(n1, n2);
		if (n < -50000.0) {
			rim = true;
			continue;
		}
		if (BridgeMode > 0.5) {
			// Bridged: small steps need no rims (the surface buries them
			// under the slope limit by itself), but a ledge the
			// neighbour's snow column can never climb - deeper than its
			// full depth plus the repose rise across the bridge span - is
			// a separate structure, and without a bare-top rim its edge
			// would stand as an open shell wall. The threshold is
			// physics, not a knob.
			if (top - n > seed + SlopePerUnit * 8.0)
				rim = true;
			continue;
		}
		float drop = top - n;
		// Slope carrying on past the neighbour cancels the drop; a step
		// against a flat run keeps it in full.
		float carry = max(n1 - n2, 0.0);
		if (drop - carry > RimStep)
			rim = true;
	}

	[branch] if (BridgeMode > 0.5)
		OutA[dtid.xy] = rim ? top : top + seed;
	else
		OutA[dtid.xy] = rim ? 0.0 : seed;
}

// InA = depth field. OutA = one repose iteration at ConeStep. ConeCS cannot be
// reused here: its terrain clamp belongs to an absolute-height field and would
// pin a depth field to world Z. Bridge mode additionally reads InB = the
// layer's TOP raster for the connectivity test.
[numthreads(8, 8, 1)] void ObjectConeCS(uint3 dtid
										: SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float texel = HeightHalfExtent * 2.0 / dims.x;
	float h = InA[dtid.xy];
	// Bridge mode: this column's own floor. Snow can only avalanche onto a
	// surface it physically reaches, so a neighbour's drift constrains this
	// column ONLY if its level plus the repose rise lands ABOVE the floor;
	// a lower drift with air between must not cut a porch, roof or post to
	// its level - that cut is what erased every shell in town on the first
	// bridged build. Empty columns (floor -100000) accept everything, which
	// is what propagates real values into cracks and edge-bilinear texels.
	float topHere = InB[dtid.xy];

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
			float cand = InA[uint2(p)] + SlopePerUnit * dist;
			[flatten] if (BridgeMode > 0.5 && cand < topHere)
				continue;
			h = min(h, cand);
		}
	}

	// The bridged surface is absolute world z, which may be negative; the
	// >=0 clamp belongs to the depth field only.
	OutA[dtid.xy] = BridgeMode > 0.5 ? h : max(h, 0.0);
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
