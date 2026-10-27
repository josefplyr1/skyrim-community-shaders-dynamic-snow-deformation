// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Object height-window processing.
//
// ScrollCS   persistence: carries the accumulated raw top/bottom maps into
//            the current window position (whole-texel offsets). The maps
//            must not depend on what the camera renders this frame; the
//            capture list is frustum-culled, and rebuilding from it alone
//            makes object heights vanish behind the camera. Runs AFTER the
//            capture rasterizes into the cleared maps, and fills only what
//            nothing drew into - see the ghost-merge note on the entry
//            point.
// CombineCS  builds the shelter mask: a structure floating well above the
//            ground shelters what is under it, as a soft melt to a dusting
//            rather than a coverage kill. Doors add to the mask; fires write
//            NEGATIVE mask values, a melt fraction that thins depth toward a
//            floor, so fire pits keep a thin floor instead of sinking below
//            terrain.
// ObjectConeSeedCS / ObjectConeCS  angle of repose over the object top
//            raster: an iterative min-plus cone transform, so no point of the
//            depth field rises steeper than SlopePerUnit from its rims.
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

	// Undersides between ShelterMaxHeight and this shelter only where the
	// spot is enclosed.
	float ShelterBandHeight;
	// P4 "Snow Settling": per-iteration Jacobi blend toward the 4-neighbour
	// average, applied to the finished cone depth fields (0 = off).
	float DiffuseLambda;
	float ShelterMaxHeight;  // underside height above ground past which a structure stops sheltering
	float ShelterConeTan;  // tan(drift angle from vertical), > 0: sizes the enclosure disc

	uint HeightProcessKeeper;  // layout keeper, 0
	uint DeadGhostTiles;  // ScrollCS: dead ghosts leave their tile's flag alone
	uint2 HeightProcessPad;
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
// Enclosure: tap radius cap (texels) and the covered share of the disc at
// which the band starts / fully shelters. Open along one whole side = 0.5.
#define SHELTER_CONE_MAX_TEXELS 160.0
#define SHELTER_CONE_LO 0.5
#define SHELTER_CONE_HI 0.85


// ScrollCS only (b2 is unbound for every other entry point here). Mirror of
// HeightGhostCB in SnowDeformation.h.
cbuffer GhostCB : register(b2)
{
	row_major float4x4 GhostViewProj;
	float4 GhostCameraPosAdjust;
}

#include "SnowDeformation/SnowExclusions.hlsli"

Texture2D<float> InA : register(t0);
Texture2D<float> InB : register(t1);
Texture2D<float4> TerrainWindow : register(t2);
// ObjectConeSeedCS only: the NEXT peeled layer's top map, for the
// continuation test behind tall cover (see the rise rim below).
Texture2D<float> InC : register(t3);
RWTexture2D<float> OutA : register(u0);
RWTexture2D<float> OutB : register(u1);
// WaterCoverCS's near field: squared texels, 10 for none in reach.
RWTexture2D<uint> WaterNearOut : register(u1);
// CombineCS only: the shelter mask, TWO independent channels (R = door
// suppression, G = melt fraction). A single winner-takes-all channel
// discarded melt wherever a door's faint influence tail reached - a ring
// of full-depth plateau snow around every sheltered door.
RWTexture2D<float2> OutMask : register(u2);
// Per 8x8 tile of the layer-1 raw maps, 1 where no texel holds a top above
// and a bottom below the sentinels: every ShelterTap there returns +0.
// Written by the bottoms ScrollCS of the layer-1 merge, read by CombineCS
// (unbound reads 0, which never skips).
RWTexture2D<uint> TileEmptyOut : register(u3);
Texture2D<uint> TileEmpty : register(t4);
// CombineCS's tile cull: ExclusionCB's PosRadius then DirExtType arrays. Each
// thread reads a different entry, which a cbuffer serializes across the warp.
StructuredBuffer<float4> ExclusionList : register(t5);
#ifdef SCROLL_RASTER_RG
// Layer 1's raster (SnowHeightCapture): x = top, y = the negated bottom.
Texture2D<float2> RasterIn : register(t6);
#endif

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

// Would a surface at this world point have been submitted this frame? Inside
// the camera frustum the game draws it, so the capture saw it; outside, the
// capture list cannot speak for it at all.
bool GhostInView(float2 worldXY, float worldZ)
{
	float3 rel = float3(worldXY, worldZ) - GhostCameraPosAdjust.xyz;
	float4 clip = mul(GhostViewProj, float4(rel, 1.0));
	return clip.w > 0.0 && all(abs(clip.xy) <= clip.w) && clip.z >= 0.0 && clip.z <= clip.w;
}

// Runs after the capture rasterizes into maps CLEARED to the sentinels, so
// what it finds is exactly this frame's truth wherever anything drew. The
// ghost fills the rest.
//
// The old order - scroll the accumulated map in, then MAX the captures over
// it - could only ever RAISE a texel. A reference swapping LOD meshes kept
// the taller silhouette of the mesh it replaced until GhostDecay ate the
// difference at half a unit per frame, which is seconds of skins shaped to a
// mesh that is no longer drawn (and, where the stale top cleared the
// shelter band, seconds of bare rock under it).
//
// A live raster only overrules the ghost where the ghost is IN VIEW: a
// surface the camera can see and nothing re-rasterized is gone. Out of view
// it stands, decayed, which is what keeps the roof over a walkway sheltering
// its floor while the camera looks straight down at the boards.
// SCROLL_TOP_ONLY: the peeled layers' merge, whose bottoms nothing reads.
#ifdef SCROLL_TOP_ONLY
static const bool kScrollBottoms = false;
#else
static const bool kScrollBottoms = true;
#endif

uint ScrollTexel(uint2 texel, uint2 dims);
#ifndef SCROLL_TOP_ONLY
groupshared uint scrollElevated;
#endif

[numthreads(8, 8, 1)] void ScrollCS(uint3 dtid
									: SV_DispatchThreadID, uint3 gid
									: SV_GroupID, uint gi
									: SV_GroupIndex) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
#ifndef SCROLL_TOP_ONLY
	if (gi == 0)
		scrollElevated = 0;
	GroupMemoryBarrierWithGroupSync();
	[branch] if (all(dtid.xy < dims))
		InterlockedOr(scrollElevated, ScrollTexel(dtid.xy, dims));
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
		TileEmptyOut[gid.xy] = scrollElevated == 0 ? 1u : 0u;
#else
	if (all(dtid.xy < dims))
		ScrollTexel(dtid.xy, dims);
#endif
}

// One texel of the merge; returns 1 where the result floats a structure
// ShelterTap can see (a top and a bottom inside the sentinels).
uint ScrollTexel(uint2 texel, uint2 dims)
{
	uint3 dtid = uint3(texel, 0);
#ifdef SCROLL_RASTER_RG
	float2 raster = RasterIn[dtid.xy];
	float rasterTop = raster.x;
	float rasterBottom = -raster.y;
#else
	float rasterTop = OutA[dtid.xy];
	float rasterBottom = kScrollBottoms ? OutB[dtid.xy] : 100000.0;
#endif
	bool haveTop = rasterTop > -50000.0;
	bool haveBottom = kScrollBottoms && rasterBottom < 50000.0;

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
			if (kScrollBottoms)
				bottom = InB[uint2(src)] + GhostDecay;
		}
	}

	// The view test costs a clip transform, so it is asked only where the
	// ghost actually disagrees with a live raster - a small minority of
	// texels. Everywhere else the old MAX/MIN answer already holds.
	[branch] if (haveTop)
	{
		if (top <= rasterTop || GhostInView(TexelWorldXY(dtid.xy, dims), top))
			top = rasterTop;
	}
	[branch] if (haveBottom)
	{
		if (bottom >= rasterBottom || GhostInView(TexelWorldXY(dtid.xy, dims), bottom))
			bottom = rasterBottom;
	}

	OutA[dtid.xy] = top;
	if (kScrollBottoms)
		OutB[dtid.xy] = bottom;
	// A ghost's top sinks and its bottom rises. Once the bottom stands
	// ShelterBandHeight + 60 over the top, ShelterTap is +0 under every
	// centre's terrain (floating needs top - terrain > 40, the band needs
	// clearance < ShelterBandHeight + 100); +1 covers the rounding.
	bool dead = DeadGhostTiles != 0 && bottom - top > ShelterBandHeight + 61.0;
	return (top > -50000.0 && bottom < 50000.0 && !dead) ? 1u : 0u;
}

// How strongly the raster at p reads as a floating structure above the
// given terrain height (walkway, roof, bridge, tent canvas). CONTINUOUS:
// a binary test quantized the ring average into visible melt terraces
// under eaves - stairs descending toward the wall.
// x = cut off at ShelterMaxHeight, y = cut off at ShelterBandHeight.
float2 ShelterTap(int2 p, int2 dims, float terrain)
{
	p = clamp(p, int2(0, 0), dims - 1);
	float2 result = 0.0;
	float top = InA[p];
	[branch] if (top > -50000.0)
	{
		float bottom = InB[p];
		// Too high to shelter: a wide archway or a tall bridge leaves the
		// ground under it open to the sky.
		float clearance = bottom - terrain;
		float floating = smoothstep(20.0, 60.0, clearance) * smoothstep(40.0, 80.0, top - terrain);
		result = floating * (1.0 - smoothstep(float2(ShelterMaxHeight, ShelterBandHeight), float2(ShelterMaxHeight, ShelterBandHeight) + 100.0, clearance));
	}
	return result;
}

// Covered share of a fixed disc (radius = ShelterBandHeight * tan(drift
// angle)), remapped. Offsets are the same for every texel, so the result
// is a convolution of the raster and cannot sweep as ghosts decay. Rings
// are rotated against each other so a straight roofline crosses one tap
// at a time.
float ShelterEnclosure(int2 texel, int2 dims, float terrain)
{
	static const float2 kEnclosureTaps[32] = {
		float2(1.000, 0.000), float2(0.707, 0.707), float2(0.000, 1.000), float2(-0.707, 0.707),
		float2(-1.000, 0.000), float2(-0.707, -0.707), float2(0.000, -1.000), float2(0.707, -0.707),
		float2(0.736, 0.146), float2(0.417, 0.624), float2(-0.146, 0.736), float2(-0.624, 0.417),
		float2(-0.736, -0.146), float2(-0.417, -0.624), float2(0.146, -0.736), float2(0.624, -0.417),
		float2(0.462, 0.191), float2(0.191, 0.462), float2(-0.191, 0.462), float2(-0.462, 0.191),
		float2(-0.462, -0.191), float2(-0.191, -0.462), float2(0.191, -0.462), float2(0.462, -0.191),
		float2(0.208, 0.139), float2(0.049, 0.245), float2(-0.139, 0.208), float2(-0.245, 0.049),
		float2(-0.208, -0.139), float2(-0.049, -0.245), float2(0.139, -0.208), float2(0.245, -0.049)
	};
	float texelSize = HeightHalfExtent * 2.0 / dims.x;
	float radius = clamp(ShelterBandHeight * ShelterConeTan / texelSize, SHELTER_RING_TEXELS, SHELTER_CONE_MAX_TEXELS);

	float covered = ShelterTap(texel, dims, terrain).y * 2.0;
	[loop] for (uint tapI = 0; tapI < 32; tapI++)
		covered += ShelterTap(texel + int2(round(kEnclosureTaps[tapI] * radius)), dims, terrain).y;
	return smoothstep(SHELTER_CONE_LO, SHELTER_CONE_HI, covered / 34.0);
}

groupshared uint combineTileCount;
groupshared uint combineTileList[MAX_EXCLUSIONS];
groupshared uint combineElevated;

// InA = raw tops, InB = raw bottoms. OutMask = shelter mask, and sizes the window.
[numthreads(8, 8, 1)] void CombineCS(uint3 dtid
									 : SV_DispatchThreadID, uint3 gid
									 : SV_GroupID, uint gi
									 : SV_GroupIndex) {
	uint2 dims;
	OutMask.GetDimensions(dims.x, dims.y);

	// The exclusions that reach this 8x8 tile. Past its reach an exclusion's
	// influence is exactly zero and the terms combine by max, so the culled
	// list gives the answer the whole buffer would.
	if (gi == 0) {
		combineTileCount = 0;
		combineElevated = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	// The shelter ring reaches 10 texels: tiles -2..+2 of this one, clamped
	// like ShelterTap's own taps.
	if (gi < 25) {
		int2 tile = clamp(int2(gid.xy) + int2(gi % 5u, gi / 5u) - 2, 0, int2(dims / 8u) - 1);
		if (TileEmpty.Load(int3(tile, 0)) == 0)
			combineElevated = 1;
	}
	{
		float2 cornerA = TexelWorldXY(gid.xy * 8, dims);
		float2 cornerB = TexelWorldXY(gid.xy * 8 + 7, dims);
		float2 tileMin = min(cornerA, cornerB);
		float2 tileMax = max(cornerA, cornerB);
		for (uint testI = gi; testI < ExclusionCount; testI += 64) {
			float4 posRadius = ExclusionList[testI];
			float2 center = posRadius.xy;
			float reach = ExclusionReach(posRadius, ExclusionList[MAX_EXCLUSIONS + testI]);
			float2 closest = clamp(center, tileMin, tileMax);
			[branch] if (dot(closest - center, closest - center) <= reach * reach)
			{
				uint slot;
				InterlockedAdd(combineTileCount, 1, slot);
				if (slot < MAX_EXCLUSIONS)
					combineTileList[slot] = testI;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (any(dtid.xy >= dims))
		return;
	// Nothing floats within the ring's reach and no exclusion reaches the tile:
	// every ShelterTap is +0, the enclosure cannot run, melt and suppress are +0.
	[branch] if (combineElevated == 0 && combineTileCount == 0)
	{
		OutMask[dtid.xy] = float2(0.0, 0.0);
		return;
	}

	float2 worldXY = TexelWorldXY(dtid.xy, dims);
	float terrain = SampleTerrainHeight(worldXY);
	float suppress = 0.0;
	float melt = 0.0;

	// Shelter: the raster feeds the floating-structure test. A bottom well
	// clear of the ground with a top high above it is a walkway/roof/bridge,
	// and the ground beneath it is sheltered from snowfall. Consumed as MELT
	// (thin shell floor, snow texture kept - a coverage kill exposed the
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
		float2 ringFrac = ShelterTap(texel, dimsI, terrain) * 2.0;
		[unroll] for (uint ringI = 0; ringI < 8; ringI++)
			ringFrac += ShelterTap(texel + kShelterRing[ringI], dimsI, terrain) +
			            ShelterTap(texel + kShelterRingInner[ringI], dimsI, terrain);
		ringFrac /= 18.0;
		float shelterFrac = ringFrac.x;
		// The band's own shelter, only where there is any and only as far as
		// the spot is enclosed.
		[branch] if (ringFrac.y - ringFrac.x > 0.001)
			shelterFrac = lerp(ringFrac.x, ringFrac.y, ShelterEnclosure(texel, dimsI, terrain));
		// Deliberately no edge noise: roofline sinks read best smooth (fire
		// bowls keep their noisy rims; sheltered snow follows the structure).
		melt = max(melt, SHELTER_MELT * saturate(shelterFrac));
	}

	// Exclusion zones: either suppress snow (doors: coverage fades to bare
	// ground) or melt it (fires: depth thins toward a floor). Shared with the
	// wide field bake so the near and far answers are the same function of the
	// same constant buffer.
	{
		uint tileCount = min(combineTileCount, MAX_EXCLUSIONS);
		for (uint listI = 0; listI < tileCount; listI++) {
			ExclusionResult one = EvaluateExclusionAt(combineTileList[listI], worldXY, terrain);
			suppress = max(suppress, one.Suppress);
			melt = max(melt, one.Melt);
		}
	}

	OutMask[dtid.xy] = float2(suppress, melt);
}

// Object snow cone seed. InA = raw object top raster. OutA = snow DEPTH above
// the local surface, not an absolute height: a field in absolute height cannot
// climb from the terrain to the top of a tall narrow object within its own
// footprint, so stacked stones and pillars would carry no snow at all.
// Zero marks a rim the layer must taper to: off the footprint, or a column
// standing well above a neighbour (an internal step, e.g. one stone on
// another). The cone passes then raise the interior at the angle of repose.
// One texel's seed; ObjectConeSeedCS and ConeFusedCS both take it from here.
float ObjectConeSeedAt(uint2 texel, uint2 dims)
{
	float top = InA[texel];
	if (top < -50000.0)
		return 0.0;

	// Per-texel seed, floored by the class constant. InB is the skin-depth
	// raster: ROADS write their own class there, so the cone over a road
	// interior reaches the road depth and the patch's verge blend can ride
	// it down to the landscape class - seeded at the constant alone, the
	// blend silently capped every road at min(road, landscape), which is how
	// Road Meshes stopped responding above the snow01 depth. The constant
	// floor is what keeps everything else byte-identical: non-carving
	// objects write ZERO depth to the raster, and an unfloored seed would
	// collapse their cone and with it every skin's edge taper.
	float texelDepth = max(InB[texel], 0.0);
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
		int2 p = int2(texel) + offs;
		if (any(p < 0) || any(p >= int2(dims)))
			continue;
		float n1 = InA[uint2(p)];
		// The 2-texel tap feeds the slope-continuation carry of both rim
		// tests below; it was never the meld option's (that was the 3-texel
		// tap and the max across them, retired 2026-09-06). Cling: the shell
		// rolls at its own raster edge whatever sits nearby.
		float n2 = -100000.0;
		int2 p2 = int2(texel) + offs * 2;
		[flatten] if (all(p2 >= 0) && all(p2 < int2(dims)))
			n2 = InA[uint2(p2)];
		float n = n1;
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

	return rim ? 0.0 : seed;
}

[numthreads(8, 8, 1)] void ObjectConeSeedCS(uint3 dtid
											: SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;
	OutA[dtid.xy] = ObjectConeSeedAt(dtid.xy, dims);
}

// InA = depth field. OutA = one repose iteration at ConeStep.
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

// P3 (edge-research study): per-column SKY OPENNESS baked from the layer-1
// tops. InA = the full-res top raster; OutA = openness at HALF resolution
// (a soft field - half res quarters the bake). 1 = open sky. The skin
// scales its depth by this, so open tops carry full snow and columns shaded
// by tall neighbours thin toward a dusting; the vertical under-cover half
// (a surface below a higher top in its OWN column) stays per-vertex in
// ApplySkinLift, since it depends on the surface's height, not the column.
// Elevation-angle test per direction: a neighbour must stand meaningfully
// above the column and steeply (tan 0.35..1.3 ~ 19..52 degrees) to shade
// it; two radii per direction so both a close wall and a taller ridge
// further out register. The pad is what keeps a surface's own parts out:
// thatch cards, stall beams and roof ridges sit 10-40 units over the
// surface beside them and shaded it into a dusting at 6 (Whiterun,
// 2026-09-17); a wall still clears 24.
static const float kOpenPad = 24.0;
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
				o = max(o, smoothstep(0.35, 1.3, ((t - h) - kOpenPad) / distW));
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
// The neighbours come in the order +x, -x, +y, -y.
float ConeSettle(float h, float a_px, float a_nx, float a_py, float a_ny)
{
	float avg = (a_px + a_nx + a_py + a_ny) * 0.25;
	return lerp(h, avg, saturate(DiffuseLambda));
}

[numthreads(8, 8, 1)] void ObjectConeDiffuseCS(uint3 dtid
											   : SV_DispatchThreadID) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;
	int2 c = int2(dtid.xy);
	int2 mx = int2(dims) - 1;
	OutA[dtid.xy] = ConeSettle(InA[dtid.xy],
		InA[uint2(clamp(c + int2(1, 0), int2(0, 0), mx))],
		InA[uint2(clamp(c - int2(1, 0), int2(0, 0), mx))],
		InA[uint2(clamp(c + int2(0, 1), int2(0, 0), mx))],
		InA[uint2(clamp(c - int2(0, 1), int2(0, 0), mx))]);
}

// A cone chain with no repose step (none can move a texel: the deepest seed
// is under one texel's rise at the slope) in ONE dispatch. Each group seeds
// its 16x16 tile and a two-texel ring into groupshared memory, settles twice
// there and writes the tile: the seed pass and both ObjectConeDiffuseCS passes
// without the two full-window round trips between them. Every texel takes
// the same seed function and the same ConeSettle over the same clamped
// neighbours, so the cone is bit-identical. It seeds from the object maps
// (t0, t1, t3).
#define CONE_TILE 16
#define CONE_RING 2
#define CONE_SPAN (CONE_TILE + 2 * CONE_RING)
groupshared float coneSeedShared[CONE_SPAN * CONE_SPAN];
groupshared float coneSettleShared[CONE_SPAN * CONE_SPAN];

// Slot of window texel p in a tile whose ring starts at a_origin.
uint ConeSlot(int2 p, int2 a_origin)
{
	int2 l = p - a_origin;
	return uint(l.y * CONE_SPAN + l.x);
}

[numthreads(CONE_TILE, CONE_TILE, 1)] void ConeFusedCS(uint3 gid
													   : SV_GroupID, uint3 gtid
													   : SV_GroupThreadID, uint gi
													   : SV_GroupIndex) {
	uint2 dims;
	OutA.GetDimensions(dims.x, dims.y);
	const int2 dimsI = int2(dims);
	const int2 mx = dimsI - 1;
	const int2 origin = int2(gid.xy) * CONE_TILE - CONE_RING;

	// Seeds over the tile and its two-texel ring; ring texels past the window
	// stay unwritten, the clamps below never reach them.
	[loop] for (uint seedI = gi; seedI < CONE_SPAN * CONE_SPAN; seedI += CONE_TILE * CONE_TILE)
	{
		const int2 p = origin + int2(seedI % CONE_SPAN, seedI / CONE_SPAN);
		[branch] if (all(p >= 0) && all(p < dimsI))
		{
			coneSeedShared[seedI] = ObjectConeSeedAt(uint2(p), dims);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// The first settle over the tile and a one-texel ring.
	const uint inner = CONE_SPAN - 2;
	[loop] for (uint settleI = gi; settleI < inner * inner; settleI += CONE_TILE * CONE_TILE)
	{
		const int2 p = origin + 1 + int2(settleI % inner, settleI / inner);
		[branch] if (all(p >= 0) && all(p < dimsI))
		{
			coneSettleShared[ConeSlot(p, origin)] = ConeSettle(coneSeedShared[ConeSlot(p, origin)],
				coneSeedShared[ConeSlot(clamp(p + int2(1, 0), int2(0, 0), mx), origin)],
				coneSeedShared[ConeSlot(clamp(p - int2(1, 0), int2(0, 0), mx), origin)],
				coneSeedShared[ConeSlot(clamp(p + int2(0, 1), int2(0, 0), mx), origin)],
				coneSeedShared[ConeSlot(clamp(p - int2(0, 1), int2(0, 0), mx), origin)]);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	// The second over the tile.
	const int2 p = int2(gid.xy) * CONE_TILE + int2(gtid.xy);
	[branch] if (all(p < dimsI))
	{
		OutA[uint2(p)] = ConeSettle(coneSettleShared[ConeSlot(p, origin)],
			coneSettleShared[ConeSlot(clamp(p + int2(1, 0), int2(0, 0), mx), origin)],
			coneSettleShared[ConeSlot(clamp(p - int2(1, 0), int2(0, 0), mx), origin)],
			coneSettleShared[ConeSlot(clamp(p + int2(0, 1), int2(0, 0), mx), origin)],
			coneSettleShared[ConeSlot(clamp(p - int2(0, 1), int2(0, 0), mx), origin)]);
	}
}

// Water the data route reads: the raw water window minus every lone wet
// texel. A texel is wet when its land vertex sits kWaterTexelDepth under
// its water; it keeps its water only when a 4-neighbour is wet too, so a
// single vertex dipping under a cell plane (inside a cliff mesh) cannot
// bare a whole texel. InA = raw water window, rows grow with -Y.
float WaterWetTexel(int2 w)
{
	if (any(w < 0) || any(w >= int2(TerrainDim, TerrainDim)))
		return 0.0;
	float water = InA.Load(int3(w, 0));
	float h = TerrainWindow.Load(int3(w.x, (int)TerrainDim - 1 - w.y, 0)).x;
	return (water > -50000.0 && h > -50000.0 && h < water - 8.0) ? 1.0 : 0.0;
}

// A vertex under its water at all (the cap's skim, 2 units).
float WaterUnderTexel(int2 w)
{
	if (any(w < 0) || any(w >= int2(TerrainDim, TerrainDim)))
		return 0.0;
	float water = InA.Load(int3(w, 0));
	float h = TerrainWindow.Load(int3(w.x, (int)TerrainDim - 1 - w.y, 0)).x;
	return (water > -50000.0 && h > -50000.0 && h < water - 2.0) ? 1.0 : 0.0;
}

static const float kWaterNearMax = 3.0;

// WaterNearOut = texels to the nearest vertex under the water, capped at
// kWaterNearMax: the cap's horizontal term. Runs only when the water raster
// or the terrain window changed.
[numthreads(8, 8, 1)] void WaterCoverCS(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= TerrainDim))
		return;
	int2 w = (int2)dtid.xy;
	float water = InA.Load(int3(w, 0));
	[branch] if (WaterWetTexel(w) > 0.5)
	{
		float nb = WaterWetTexel(w + int2(1, 0)) + WaterWetTexel(w - int2(1, 0)) + WaterWetTexel(w + int2(0, 1)) + WaterWetTexel(w - int2(0, 1));
		[flatten] if (nb < 0.5)
			water = -100000.0;
	}
	OutA[dtid.xy] = water;
	// The squared distance: its sqrt is the length the reader takes, and
	// anything past kWaterNearMax reads as it.
	uint nearSq = 10u;
	[loop] for (int j = -3; j <= 3; j++)
		[loop] for (int i = -3; i <= 3; i++)
			[flatten] if (WaterUnderTexel(w + int2(i, j)) > 0.5)
				nearSq = min(nearSq, (uint)(i * i + j * j));
	WaterNearOut[dtid.xy] = nearSq;
}

#ifdef RASTER_COMPARE
// Debug (the height raster cache's verify mode): counts the texels where a
// kept raster and a fresh draw of the same captures differ, bit for bit, and
// keeps the first 8 after the count: texel y:x, kept .xy, fresh .xy.
Texture2D<float4> CompareKept : register(t8);
Texture2D<float4> CompareFresh : register(t9);
RWByteAddressBuffer CompareCount : register(u4);

[numthreads(8, 8, 1)] void RasterCompareCS(uint3 dtid
										   : SV_DispatchThreadID) {
	uint2 dims;
	CompareKept.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;
	float4 kept = CompareKept[dtid.xy];
	float4 fresh = CompareFresh[dtid.xy];
	if (all(asuint(kept) == asuint(fresh)))
		return;
	uint prior;
	CompareCount.InterlockedAdd(0, 1u, prior);
	if (prior >= 8u)
		return;
	uint base = 16u + prior * 32u;
	CompareCount.Store4(base, uint4((dtid.y << 16) | dtid.x, asuint(kept.x), asuint(kept.y), asuint(fresh.x)));
	CompareCount.Store4(base + 16u, uint4(asuint(fresh.y), 0u, 0u, 0u));
}
#endif
