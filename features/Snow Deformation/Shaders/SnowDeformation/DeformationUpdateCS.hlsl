// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Persistent snow deformation map update, split into three passes over a
// TOROIDAL store.
//
// A square world-space window following the camera in whole-texel steps.
// Texels never move: the map is addressed physically as
// (logical + MapOrigin) & (dim - 1), and a scroll advances MapOrigin instead
// of copying 4M texels through a ping-pong.
//
//   RingCS   - writes the ring of texels whose world assignment changed
//              (tile-store inject or pristine zero). Dispatch sized to the
//              ring, so a walking-speed scroll costs a few thousand threads.
//   EvolveCS - the world acting on the map: wind-biased refill, melt/crust/
//              deposit decay, unsupported-snow slump. The only pass that
//              reads NEIGHBOUR texels, so it reads a snapshot copy taken
//              after RingCS and writes the map in place.
//   StampCS  - actors acting on the map: stamp capsules and bow-wave
//              deposits, read-modify-write on the texel EvolveCS just wrote.
//              Per-texel only, so it can later be tile-dispatched over the
//              stamps' bounding boxes.
//
// Splitting at the stamp boundary is safe because every stamp/wave term is a
// function of the texel's own value; the one storage round between the passes
// only touches texels a stamp is about to overwrite anyway.
//
// ClearMap rides in the CB for layout stability but the CPU expresses it as
// a full-map ring rect; no pass branches on it.
//
// Stamp classes, selected per stamp by StampEnds[i].z:
//   CARVE (0) - instantaneous depth, max-blended, so standing in a trench
//               does not deepen it.
//   MELT  (1) - additive and dt-scaled, so dwell time deepens the bowl.
//   PIT   (2) - instantaneous like CARVE, and scorches. Scorched snow is
//               displaced snow and keeps its berm; melted snow has none.
//   CRUST (3) - sustained, approaching a target at a rate. Moves no snow.
// Adding STAMP_MODE_CONE reinterprets the same three fields as a wedge: apex
// at the segment start, axis to its end, radius = the half-width at the far
// end. Shouts only. A row of discs cannot stand in for it - each stamp holds
// full depth across the inner tenth of its radius, so they read as craters.
//
// Melted ground refills SLOWER rather than carrying extra depth: depth is
// capped at 1.0, and banking persistence as over-depth flattens the bowl into
// a walled pit once consumers saturate.
//
// Channels:
//   .x  total depression depth (0 = untouched, 1 = ground).
//   .y  SIGNED surface state - the two are mutually exclusive.
//         > 0  the portion of .x that was melted; the berm field subtracts it.
//         < 0  scorch, which keeps its berm and darkens the shell.
//       Berm reads x - max(y, 0); scorch reads max(-y, 0).
//   .z  crust: refrozen snow, resists carving and shades as ice.
//   .w  deposit: snow standing above the untouched surface. MAXed in every
//       frame at the crest's current position, so shouldered ground stays
//       shouldered after the walker leaves. Decays on BowWaveSettle and to
//       the refill. The ImGui map preview blends by alpha and so reads
//       deposit as transparency.

// Bow-wave crests to deposit this frame. Mirrors kMaxBowWaves in
// SnowDeformation.h and MAX_BOW_WAVES in SnowShell.hlsl - the SHAPE is
// evaluated identically in both places, so what the shell draws is what the
// map remembers.
#define MAX_DEPOSIT_WAVES 16

#define MAX_STAMPS 256

// Added to a stamp's mode to mark it a cone. Mirrored by kStampModeCone in
// SnowDeformation.h.
#define STAMP_MODE_CONE 10.0
// Added to a stamp's mode to give a carve a BOWL cross-section instead of the
// flat floor and standing walls a trench has. It reuses the melt bowl's own
// floor setting, because that is already the shape of a hollow scooped out
// rather than cut. Mirrored by kStampModeBowl in SnowDeformation.h.
#define STAMP_MODE_BOWL 20.0
// Where a bowl carve stops holding full depth, as a fraction of its radius.
// Zero: a scoured hollow has no floor at all, it curves from the middle. Its
// own number rather than the melt bowl's, because that one is a FIRE knob and
// a cyclone's gouge has no business changing shape when fire is retuned.
#define CARVE_BOWL_FLOOR 0.0
// A cone's width is a fixed SLOPE from its apex, never a fraction of how far it
// has got so far. That distinction is the whole of why it can grow: both the
// stamp's radius and its axis scale together as the front advances, so the
// slope between them is constant and ground once covered never changes shape
// again. Anything here that keyed off the CURRENT length instead - a mouth
// width, a fade over the last stretch - reached back and re-shaped snow the
// front had already passed, which is what made the near berms crawl outward
// as the shout ran on.

// Upwind supply sample distance for wind-biased refill, in texels.
#define DRIFT_FETCH_TEXELS 3.0
// Melt bowls hold full strength across the inner third and then rise for a
// long way, matching the campfire basins in SnowExclusions.hlsli. Carve uses
// the much steeper StampFalloffStart instead: a trench has walls, a melt bowl
// has shoulders.
// Melt edge irregularity runs on these world-unit cells. Deliberately coarse:
// a melt basin has a wandering OUTLINE and a smooth CROSS-SECTION, so the
// noise must move the rim without chipping the surface. The trail noise is a
// third of the finer cell here, which is what churns a footprint edge.
#define MELT_NOISE_COARSE 80.0
#define MELT_NOISE_FINE 24.0
// Arc branches thrown off a strike. Lightning does not dig a hole, it forks,
// so the mark is a small core with a handful of thin legs radiating out.
#define PIT_LOBES 5
// Lobe geometry as fractions of the stamp radius.
#define PIT_LOBE_MIN 0.45
#define PIT_LOBE_MAX 1.15
#define PIT_LOBE_WIDTH 0.16
// The pock mask cuts a pit into fragments. Its cell is a FRACTION of the pit
// rather than a fixed size: at a fixed size a small strike spans barely two
// cells and the mask can erase the whole mark, which is exactly how a bolt
// ends up not marking at all most of the time it lands.
#define PIT_POCK_CELLS_ACROSS 4.5
#define PIT_POCK_CELL_MIN 6.0
// Refill multiplier at full supply and full wind; interior texels with a
// carved upwind neighbor stall, so the average fill rate stays near uniform.
#define DRIFT_GAIN 2.0

// Unsupported-snow slump (TRENCH-REALISM-PLAN.md Stage 3b): a support test,
// not a blur. Per axis at radius R, support = min(carve at +R, carve at -R);
// the settle target is the max over axes and radii. A strip dug on both sides
// settles toward its neighbours' floor; a trench wall, carved on one side
// only, reads ~0 and never moves. Fixed point by construction, and open snow
// never starts, so the collapse cannot creep outward.
//
// Radii are world units via the live TexelSize. An axis contributes nothing
// without SLUMP_MIN_SUPPORT inside the two GATE radii - that bound is what
// confines the effect to trenches. The third, longer radius never gates; it
// only reads the flanking floor depth for texels that already qualified.
//
// Support is displaced, unscorched depth (x - |y|), never raw x: counting
// spell marks would let a campfire settle the snow around it. The berm field's
// Displaced() keeps scorch instead - a berm is spoil thrown, this is ground
// bearing weight.
#define SLUMP_RADII 3
// Radii up to this index gate; beyond it they only deepen.
#define SLUMP_GATE_RADII 2
static const float kSlumpRadius[SLUMP_RADII] = { 16.0, 32.0, 64.0 };
static const float kSlumpReach[SLUMP_RADII] = { 1.0, 0.85, 0.7 };
// A strip settles PARTWAY toward its neighbors' floor, not onto it - about
// half: the fin survives as a low bump inside the channel rather than
// melting to the bottom.
#define SLUMP_SETTLE 0.5
// Least min-support inside the gate radii that engages an axis. Well above
// refill remnants and trench shoulders, well below a walked trail's floor.
#define SLUMP_MIN_SUPPORT 0.25
// Eight axes 22.5 degrees apart (taps go both ways, so 180 covers the
// circle). Four showed up as a cross pattern on diagonal fins.
#define SLUMP_AXES 8
static const float2 kSlumpAxis[SLUMP_AXES] = {
	float2(1.0, 0.0), float2(0.9239, 0.3827), float2(0.7071, 0.7071), float2(0.3827, 0.9239),
	float2(0.0, 1.0), float2(-0.3827, 0.9239), float2(-0.7071, 0.7071), float2(-0.9239, 0.3827)
};
// Depth-fraction per second at slider 1: a fin sinks over a second or two
// after the second walker passes, settling rather than popping.
#define SLUMP_SPEED 0.5
// The settled floor is UNEVEN on purpose - low bumps, not a plane. The
// target wobbles on the coarse melt cells, which are already the scale of
// a wandering outline rather than a chipped surface.
#define SLUMP_FLOOR_NOISE 0.45

cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	// Toroidal store: where logical texel (0,0) sits physically. A scroll
	// advances this instead of copying the map; only the reassigned ring of
	// texels is rewritten (RingCS).
	int2 MapOrigin;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	// Lower smoothstep edge of the stamp falloff (fraction of radius):
	// higher = steeper trench walls.
	float StampFalloffStart;
	// Retired TrailIrregularity slot; layout kept.
	float padTrail;
	// Unit wind direction (world XY, blowing toward) times wind strength
	// 0-1; zero = uniform refill.
	float2 WindBias;

	// Seconds this frame; melt accumulates per second, not per frame.
	float DeltaTime;
	// How much slower melted ground refills, 0-1. 0 = it recovers exactly as
	// fast as a footprint.
	float MeltPersistence;
	// Fraction of the radius held at full melt before the flank starts.
	// 0 = a pure bowl curving from the centre; high = a flat floor with walls.
	float MeltFloorStart;
	// Fraction the melt radius wobbles by, on the coarse cells above.
	float MeltEdgeNoise;

	// Depth a boot still prints on fully crusted snow, as a fraction of what it
	// would print on loose snow. NOT zero: actors stand on the terrain while
	// the shell floats above them, so a crust that refuses to take a print at
	// all puts feet inside apparently solid ice.
	float CrustPrintDepth;
	// Crust lost per second regardless of weather. Ice gives way to
	// temperature, not to snowfall, so this is deliberately NOT folded into
	// the refill - which stops entirely in clear weather and would otherwise
	// leave a glaze standing for ever.
	float CrustThaw;
	// How completely carving through a crust destroys it, against how deep the
	// cut went. Anything that cuts snow has broken the skin over it.
	float CrustBreakOnCarve;
	// Unsupported-snow slump speed, 0-1; 0 disables the pass entirely.
	float SlumpRate;

	// 1 = InjectDepth holds the tile store's memory of the texels arriving
	// from outside the window. Its own row: Stamps must start 16-byte aligned.
	uint InjectValid;
	// This frame's span in GAME time, in the same seconds DeltaTime uses. Equal
	// to DeltaTime in ordinary play; a wait or a sleep passes hours without
	// rendering them, and the world's own clocks must not sit those hours out.
	float GameDeltaTime;
	// 1 = paint the per-texel activity view (u2).
	uint DebugActivityView;
	uint InjectPad;

	// Ring of texels whose world assignment changed this frame, in LOGICAL
	// texel space (x0, y0, w, h). Two rects at most: the leading band per
	// scrolled axis, or one full-map rect on a clear. RingCS covers their
	// union by a flat texel index.
	int4 RingRects[2];
	uint RingRectCount;
	uint RingTotalTexels;
	// 1 = the tile scans list every tile (the "my trenches froze" one-click
	// cross-check). Claimed a RingPad slot, layout unchanged.
	uint ForceAllDirty;
	uint RingPad;

	float4 Stamps[MAX_STAMPS];   // xy: world pos, z: depth (carve) or strength (melt), w: radius
	float4 StampEnds[MAX_STAMPS];  // xy: previous world pos (capsule start), z: 0 carve / 1 melt, w: melt rate (depth per second)

	// x = live count, y = reach scale, z = forward bias, w = settle seconds
	// (how long a deposit lying INSIDE a trench holds; out on untouched snow
	// a deposit is permanent until refill buries it).
	float4 DepositParams;
	float4 DepositPosDir[MAX_DEPOSIT_WAVES];   // xy world pos (the foot), zw unit travel direction
	float4 DepositShape[MAX_DEPOSIT_WAVES];    // x push radius, y strength, zw previous foot position
}

// EvolveCS's snapshot of the map, copied on the CPU just before the pass so
// its neighbour reads (slump support, upwind supply) see one consistent
// frame. Physical layout, like the map it was copied from.
Texture2D<float4> PreviousDeformation : register(t0);
// THE map - single texture, physical (toroidal) layout. RingCS writes the
// reassigned band, EvolveCS rewrites in place from the snapshot, StampCS
// read-modify-writes (typed UAV load - the codebase-wide assumption
// GrassCollision's CollisionUpdateCS already relies on for the same RGBA16F
// format).
RWTexture2D<float4> CurrentDeformation : register(u0);
// Tile-store depth for the arriving ring, resampled on the CPU. LOGICAL
// layout - RingCS translates when it writes the map.
Texture2D<float> InjectDepth : register(t1);
// PHYSICAL tiles (x | y << 16, 8x8 texels each) the stamp pass must visit -
// built on the CPU from the stamp capsules' and waves' bounding boxes, so
// the dispatch covers actors' surroundings and nothing else.
StructuredBuffer<uint> StampTiles : register(t2);

// Per-tile (8x8) occupancy of the PHYSICAL map: 1 = some texel holds a
// non-zero channel. Maintained by the passes themselves - an evolve or
// stamp group writes the truth for its whole tile, the ring sets it where
// inject lands - so the evolve scan needs no CPU readback and a stale mark
// self-heals on the next visit. Seeded all-1 on map (re)creation: the safe
// default is dirty.
RWTexture2D<uint> Occupancy : register(u3);
Texture2D<uint> OccupancyIn : register(t4);
// Evolve tile list: [0] = count, then packed tiles (x | y<<16). Appended by
// ScanEvolveCS, sized into indirect args by TileArgsCS, consumed by
// EvolveCS - count and list never touch the CPU.
RWStructuredBuffer<uint> EvolveTiles : register(u4);
StructuredBuffer<uint> EvolveTilesIn : register(t3);
// TileArgsCS: whatever list is bound at t5 becomes indirect args at u5.
StructuredBuffer<uint> TileListIn : register(t5);
RWByteAddressBuffer TileArgs : register(u5);

// Per-tile "the map changed here this frame" - the berm bake's dirty set,
// marked by every writer (the ring unconditionally: departing ground zeroed
// is as much a berm-input change as arriving ground injected). Accumulates
// until a berm rebuild consumes it (the CPU clears it after), so a berm
// A/B toggle re-enabling picks up exactly what it missed.
RWTexture2D<uint> BermDirty : register(u6);
Texture2D<uint> BermDirtyIn : register(t6);
// Berm tile list, same shape as the evolve list.
RWStructuredBuffer<uint> BermTiles : register(u7);

// Logical -> physical texel. The dim is a power of two (1024/2048/4096), so
// the wrap is a mask. Callers clamp in LOGICAL space first - the map border
// is the window's world border; the physical seam is meaningless to the
// simulation and must never see a neighbour read across it.
int2 TorusPhys(int2 logical, int2 dims)
{
	return (logical + MapOrigin) & (dims - 1);
}

// Idle-skip activity flag: ORed to 1 when any texel's STORED value moved this
// frame. Compared at the R16 map's own precision - a half quantum - or a
// sub-quantum decay (slump parked a hair off its clamp, a thaw tail) would
// read as activity for ever and the skip would never engage. An epsilon, not
// f32tof16 bits: that intrinsic truncates while texture storage rounds to
// nearest, so a result the stored map rounds straight back to compared as
// "changed" every frame.
// Raw layout mirrored in SnowDeformation.cpp: [0] flag, [4] count,
// [8]/[12] complemented min X/Y (min as InterlockedMax of 65535-coord, so an
// all-zero clear initializes every field), [16]/[20] max X/Y, [24]/[28]/[32]
// per-channel counts (depth, melt/scorch, crust/deposit), [36] delta sum,
// [40] evolve-only flag (EvolveCS changed something - its idle gate's own
// verdict, meaningful only on frames evolve ran), [44] evolve tile count and
// [48] berm tile count (the dispatch census). Both passes OR into the same
// buffer; the CPU clears it once per executed frame.
RWByteAddressBuffer ActivityFlag : register(u1);
groupshared uint gActivity;
groupshared uint gCountR;
groupshared uint gCountG;
groupshared uint gCountB;
groupshared uint gCMinX;
groupshared uint gCMinY;
groupshared uint gMaxX;
groupshared uint gMaxY;
// Sum of the largest per-texel delta, fixed-point 1e6. The MEAN delta is a
// term's fingerprint: the slump step is SlumpRate x 0.5 x dt and scales with
// the slider; storage-precision creep is an order smaller and scales with
// nothing.
groupshared uint gDeltaSum;
// Any texel of this group's tile non-zero after the pass - the occupancy
// truth an aligned 8x8 group can write absolutely.
groupshared uint gNonZero;

// Debug: per-texel activity, painted only while the menu view is open.
// R = depth, G = melt/scorch, B = crust or deposit; brightness = how far past
// stored precision the change is. Max-composed: the CPU clears it once per
// frame and each executed pass folds its own delta in.
RWTexture2D<float4> ActivityView : register(u2);

// Per-channel excess beyond what the R16 map's storage precision can express;
// zero everywhere = this pass rewrote the stored map byte-identically.
float4 StoredDelta(float4 a, float4 b)
{
	float4 d = abs(a - b);
	float4 tol = max(abs(a), abs(b)) * exp2(-11.0) + 1e-6;
	return max(d - tol, 0.0);
}

void ActivityReset(uint GIdx)
{
	if (GIdx == 0) {
		gActivity = 0;
		gCountR = 0;
		gCountG = 0;
		gCountB = 0;
		gCMinX = 0;
		gCMinY = 0;
		gMaxX = 0;
		gMaxY = 0;
		gDeltaSum = 0;
		gNonZero = 0;
	}
	GroupMemoryBarrierWithGroupSync();
}

// Flag, counts and bbox via one groupshared reduction - 4M threads hammering
// a single address serializes on the atomic unit.
void ActivityAccumulate(float4 delta, uint2 pixel)
{
	if (any(delta > 0.0)) {
		InterlockedAdd(gActivity, 1u);
		if (delta.x > 0.0)
			InterlockedAdd(gCountR, 1u);
		if (delta.y > 0.0)
			InterlockedAdd(gCountG, 1u);
		if (max(delta.z, delta.w) > 0.0)
			InterlockedAdd(gCountB, 1u);
		InterlockedMax(gCMinX, 65535u - pixel.x);
		InterlockedMax(gCMinY, 65535u - pixel.y);
		InterlockedMax(gMaxX, pixel.x);
		InterlockedMax(gMaxY, pixel.y);
		float maxDelta = max(max(delta.x, delta.y), max(delta.z, delta.w));
		InterlockedAdd(gDeltaSum, min((uint)(maxDelta * 1e6 + 0.5), 1000000u));
	}
	[branch] if (DebugActivityView)
		ActivityView[pixel] = max(ActivityView[pixel],
			float4(saturate(delta.x * 512.0), saturate(delta.y * 512.0),
				saturate(max(delta.z, delta.w) * 512.0), 1.0));
}

void ActivityPublish(uint GIdx)
{
	GroupMemoryBarrierWithGroupSync();
	if (GIdx == 0 && gActivity != 0) {
		ActivityFlag.InterlockedOr(0, 1u);
		ActivityFlag.InterlockedAdd(4, gActivity);
		uint unused;
		ActivityFlag.InterlockedMax(8, gCMinX, unused);
		ActivityFlag.InterlockedMax(12, gCMinY, unused);
		ActivityFlag.InterlockedMax(16, gMaxX, unused);
		ActivityFlag.InterlockedMax(20, gMaxY, unused);
		ActivityFlag.InterlockedAdd(24, gCountR);
		ActivityFlag.InterlockedAdd(28, gCountG);
		ActivityFlag.InterlockedAdd(32, gCountB);
		ActivityFlag.InterlockedAdd(36, gDeltaSum);
	}
}

// World-anchored value noise (8-unit cells at the call site) wobbling each
// stamp's falloff distance, so trail edges read as churned snow instead of
// swept circles.
float StampNoiseHash(float2 cell)
{
	// Wrap the lattice so the hash's frac() math stays well inside float
	// precision at world-scale inputs; the 512-cell repeat is invisible in
	// edge wobble.
	cell -= 512.0 * floor(cell / 512.0);
	float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float StampNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(StampNoiseHash(i), StampNoiseHash(i + float2(1, 0)), f.x),
		lerp(StampNoiseHash(i + float2(0, 1)), StampNoiseHash(i + float2(1, 1)), f.x), f.y);
}

// Support depth of a snapshot texel for the slump test: displaced,
// unscorched carve only (see the SLUMP_* block). Takes a LOGICAL texel;
// outside the window counts as PRISTINE, not as carved: a border texel then
// has one untouched side and stands, which errs toward doing nothing at the
// edge.
float SlumpTap(int2 p, int2 dims)
{
	if (any(p < 0) || any(p >= dims))
		return 0.0;
	float4 t = PreviousDeformation[uint2(TorusPhys(p, dims))];
	return saturate(t.x - abs(t.y));
}

// The scroll made free: texels whose world assignment changed this frame get
// their stored memory (or pristine zero) written directly; every other texel
// is simply left where it physically stands. Flat index over the rect union,
// so the dispatch is sized to the ring and not the map.
[numthreads(64, 1, 1)] void RingCS(uint3 DTid
								   : SV_DispatchThreadID) {
	uint id = DTid.x;
	if (id >= RingTotalTexels || RingRectCount == 0)
		return;

	uint2 dims;
	CurrentDeformation.GetDimensions(dims.x, dims.y);

	int4 rect = RingRects[0];
	const uint rect0Texels = (uint)(rect.z * rect.w);
	if (id >= rect0Texels) {
		id -= rect0Texels;
		rect = RingRects[1];
	}
	const int2 logical = int2(rect.x + (int)(id % (uint)rect.z), rect.y + (int)(id / (uint)rect.z));

	// Arriving ground is not pristine: the tile store remembers what was dug
	// there. Everything else - melt, crust, deposit - is not stored, exactly
	// as the old scroll path's out-of-window seed behaved.
	float depth = 0.0;
	[branch] if (InjectValid)
		depth = InjectDepth[logical];
	const uint2 phys = uint2(TorusPhys(logical, int2(dims)));
	CurrentDeformation[phys] = float4(depth, 0.0, 0.0, 0.0);
	// Injected ground occupies its tile; zeroed ground leaves any stale mark
	// for the next evolve visit to heal (writes here are unordered across the
	// band, so an absolute 0 could clobber a neighbour texel's 1).
	if (depth > 0.0)
		Occupancy[phys >> 3] = 1u;
	// The berm bake's inputs changed either way.
	BermDirty[phys >> 3] = 1u;
}

// The world acting on the map: refill, decay, slump. The neighbour reads
// (slump support, upwind refill supply) all go to the previous-frame
// snapshot, so tile edges cannot see half-updated texels. No scroll: texels
// never move, RingCS already rewrote the reassigned band. Returns whether
// the texel holds anything, for the group's occupancy write.
bool EvolveTexel(uint2 phys)
{
	uint2 dims;
	PreviousDeformation.GetDimensions(dims.x, dims.y);
	// Inverse of TorusPhys: world position, border tests and the activity
	// bookkeeping live in logical space.
	const uint2 pixel = uint2((int2(phys) - MapOrigin) & (int2(dims) - 1));

	float2 worldPos = WindowOrigin + (float2(pixel) + 0.5) * TexelSize;

	// What this texel would hold if the pass had not run - the activity
	// comparison baseline.
	const float4 carried = PreviousDeformation[phys];
	float deformation = carried.x;
	// Melted portion of `deformation`, carried so the shells can tell a
	// melt basin from a dug trench.
	float melted = carried.y;
	float crust = carried.z;
	float deposit = carried.w;

	{
		// Wind-biased refill: recovery scales with the intact snow a few
		// texels upwind (the drift supply), so carved areas fill from their
		// upwind edge and the fill front marches downwind. Calm weather
		// falls back to uniform refill.
		float refill = RefillAmount;
		float windStrength = length(WindBias);
		[branch] if (refill > 0.0 && windStrength > 0.001)
		{
			int2 upwindPixel = int2(pixel) - int2(round(WindBias / windStrength * DRIFT_FETCH_TEXELS));
			float upwindDeformation = deformation;
			[branch] if (all(upwindPixel >= 0) && all(upwindPixel < int2(dims)))
			{
				upwindDeformation = PreviousDeformation[uint2(TorusPhys(upwindPixel, int2(dims)))].x;
			}
			refill *= lerp(1.0, (1.0 - upwindDeformation) * DRIFT_GAIN, windStrength);
		}
		// Warm wet ground takes its time. Scaled by how much of this texel's
		// depression was melted rather than dug, so a boot print through a
		// melt basin still recovers at the boot print rate. Scorch (negative)
		// gets none of this: burnt snow is still snow, and refills normally.
		refill *= lerp(1.0, 1.0 - saturate(MeltPersistence), saturate(max(melted, 0.0) / max(deformation, 1e-4)));

		deformation = max(deformation - refill, 0.0);
		// Both states fade with the snow they describe: the melted portion can
		// never exceed the depression it is part of, and scorch fades toward
		// zero from the other side.
		melted = melted >= 0.0 ? min(max(melted - refill, 0.0), deformation) :
		                         min(melted + refill, 0.0);
		// Fresh snow buries a crust as readily as it fills a trench, and ice
		// gives way to temperature besides - so a glaze fades even under a
		// clear sky, where the refill has stopped entirely.
		// GAME time: this is the one decay that keeps running under a clear sky,
		// so it is also the one a night spent waiting would otherwise skip
		// entirely - the glaze was still there in the morning.
		crust = max(crust - refill - CrustThaw * GameDeltaTime, 0.0);
		// SETTLE IS A TRENCH CLOCK, NOT A WAVE CLOCK.
		// Snow shouldered onto untouched cover has been MOVED, and moving
		// itself back is not something snow does - so out there the only
		// thing that takes a deposit away is refill, i.e. fresh snowfall
		// burying it, exactly as it buries the trench beside it. That is why
		// the wave now stays when the walker stops.
		//
		// Inside the trench the deposit is spoil lying on ground that has
		// ALREADY been carved, and that is what was thickening the trench's
		// own spiky edges; there the settle clock clears it fast. One field,
		// two lifetimes, told apart by whether the ground under it was dug.
		// Fixed 0.35 s: the Settle slider is RETIRED.
		const float dugHere = saturate(deformation * 3.0);
		deposit = max(deposit - refill - dugHere * DeltaTime / 0.35, 0.0);

		// Unsupported-snow slump: see the SLUMP_* block up top for the
		// design. Runs on the snapshot, like the drift fetch above, and
		// RAISES deformation toward the settle target at a rate - so it
		// composes with the refill (which is pulling the other way on both
		// the strip and its neighbors) and with this frame's stamps, which
		// max-blend over it in StampCS.
		// The receiving texel is skipped outright while it carries any melt
		// or scorch of its own: those marks are spell-authored shapes, and
		// deepening one - even toward a correct neighbor floor - redraws it.
		[branch] if (SlumpRate > 0.001 && deformation < 0.999 && abs(melted) < 0.005)
		{
			float slumpTarget = 0.0;
			[unroll] for (uint axis = 0; axis < SLUMP_AXES; axis++)
			{
				float gate = 0.0;
				float axisTarget = 0.0;
				[unroll] for (uint r = 0; r < SLUMP_RADII; r++)
				{
					int2 off = int2(round(kSlumpAxis[axis] * (kSlumpRadius[r] / max(TexelSize, 1e-4))));
					float support = min(SlumpTap(int2(pixel) + off, int2(dims)),
						SlumpTap(int2(pixel) - off, int2(dims)));
					if (r < SLUMP_GATE_RADII)
						gate = max(gate, support);
					axisTarget = max(axisTarget, support * kSlumpReach[r]);
				}
				if (gate > SLUMP_MIN_SUPPORT)
					slumpTarget = max(slumpTarget, axisTarget);
			}
			// Low bumps, not a plane: half-depth settle, wobbled on the
			// coarse cells so the floor keeps an uneven remainder.
			slumpTarget *= SLUMP_SETTLE * (1.0 - SLUMP_FLOOR_NOISE * StampNoise(worldPos / MELT_NOISE_COARSE));
			// Crusted snow is frozen solid and holds its shape. The melted
			// channel is left alone: raising depth only loosens its clamp,
			// so the added depth reads as DISPLACED - which also sheds the
			// strip's berm through the (1 - deformation) mask for free.
			// Dead-zone at the R16 map's own resolution: chasing a
			// sub-quantum gap cannot change what the stored map renders, but
			// it holds the activity verdict hot - the settle front then
			// crawls one ULP a frame for minutes and the idle skip never
			// engages. One quantum short of target is pixel-identical.
			[branch] if (slumpTarget - deformation > max(slumpTarget, deformation) * exp2(-10.0) + 1e-6)
			{
				// GAME time, like the refill and the thaw: settling is
				// something the world does to itself over hours, and it is
				// clamped to the target, so a night waited through arrives at
				// the same place standing there for it would have.
				deformation = min(deformation + SlumpRate * SLUMP_SPEED * (1.0 - crust) * GameDeltaTime,
					slumpTarget);
			}
		}
	}

	// Alpha is written as 1, not 0, on the debug side only. Nothing reads the
	// map's .w as alpha except the ImGui preview, which the deposit channel
	// claimed; see TrenchDebugCS for the honest view.
	float4 result = float4(deformation, melted, crust, deposit);
	CurrentDeformation[phys] = result;

	ActivityAccumulate(StoredDelta(result, carried), pixel);
	return any(result != 0.0);
}

// Indirect over ScanEvolveCS's list: occupied tiles plus the slump-reach
// halo (a pristine fin BETWEEN two carved trails is the receiving texel, so
// zero tiles beside occupied ones must still be visited). Each group owns
// one whole tile and writes its occupancy back absolutely - which is also
// how a stale mark heals.
[numthreads(8, 8, 1)] void EvolveCS(uint3 GTid
									: SV_GroupThreadID, uint3 Gid
									: SV_GroupID, uint GIdx
									: SV_GroupIndex) {
	// Uniform per group, so the early-out never splits a barrier.
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= EvolveTilesIn[0])
		return;

	ActivityReset(GIdx);
	const uint packed = EvolveTilesIn[1 + listIndex];
	const uint2 tile = uint2(packed & 0xFFFFu, packed >> 16u);
	if (EvolveTexel(tile * 8u + GTid.xy))
		InterlockedOr(gNonZero, 1u);
	ActivityPublish(GIdx);
	if (GIdx == 0) {
		Occupancy[tile] = gNonZero;
		if (gActivity != 0) {
			BermDirty[tile] = 1u;
			// Evolve's own word: a run that changed nothing lets the CPU
			// idle this pass until an external write (ring, stamps) or
			// refill re-arms it.
			ActivityFlag.InterlockedOr(40, 1u);
		}
	}
}

// Lists the tiles evolve must visit: any occupancy within the slump reach,
// tested in wrapped physical space (logical adjacency survives the seam;
// the extra cross-border pairs are harmless over-inclusion). At extreme
// range/resolution combos the halo grows to a few hundred taps per tile -
// accepted, the scan is R8 reads against a 4M-texel pass saved.
[numthreads(8, 8, 1)] void ScanEvolveCS(uint3 DTid
										: SV_DispatchThreadID) {
	uint2 dims;
	// The SRV, not the UAV: the grid reaches this pass as OccupancyIn (u3 is
	// deliberately unbound), and an unbound view's GetDimensions returns 0x0
	// - which silently killed every scan thread at the bounds guard and
	// dispatched an evolve of zero tiles. The census is what caught it.
	OccupancyIn.GetDimensions(dims.x, dims.y);
	if (any(DTid.xy >= dims))
		return;

	bool need = ForceAllDirty != 0;
	if (!need) {
		const int halo = (int)ceil((64.0 / max(TexelSize, 1e-3) + 1.0) / 8.0);
		for (int dy = -halo; dy <= halo && !need; dy++)
			for (int dx = -halo; dx <= halo && !need; dx++) {
				const uint2 t = (DTid.xy + uint2(int2(dx, dy) + int2(dims))) & (dims - 1);
				need = OccupancyIn[t] != 0;
			}
	}
	if (!need)
		return;

	uint idx;
	InterlockedAdd(EvolveTiles[0], 1u, idx);
	EvolveTiles[1 + idx] = DTid.x | (DTid.y << 16);
	// Census, riding the activity readback.
	ActivityFlag.InterlockedAdd(44, 1u);
}

// Lists the tiles the berm bake must rebuild: the map changed within the
// 40-unit tap reach. Same wrapped-space dilation as the evolve scan.
[numthreads(8, 8, 1)] void ScanBermCS(uint3 DTid
									  : SV_DispatchThreadID) {
	uint2 dims;
	BermDirtyIn.GetDimensions(dims.x, dims.y);
	if (any(DTid.xy >= dims))
		return;

	bool need = ForceAllDirty != 0;
	if (!need) {
		const int halo = (int)ceil((40.0 / max(TexelSize, 1e-3) + 2.0) / 8.0);
		for (int dy = -halo; dy <= halo && !need; dy++)
			for (int dx = -halo; dx <= halo && !need; dx++) {
				const uint2 t = (DTid.xy + uint2(int2(dx, dy) + int2(dims))) & (dims - 1);
				need = BermDirtyIn[t] != 0;
			}
	}
	if (!need)
		return;

	uint idx;
	InterlockedAdd(BermTiles[0], 1u, idx);
	BermTiles[1 + idx] = DTid.x | (DTid.y << 16);
	ActivityFlag.InterlockedAdd(48, 1u);
}

// A list's count becomes indirect dispatch args (x capped at 1024, the
// remainder in y - 4096-dim full-dirty is 262144 tiles, past the 65535
// one-dimension cap), so the CPU never reads a count back.
[numthreads(1, 1, 1)] void TileArgsCS(uint3 DTid
									  : SV_DispatchThreadID) {
	const uint count = TileListIn[0];
	TileArgs.Store3(0, uint3(min(count, 1024u), (count + 1023u) / 1024u, 1u));
}

// Actors acting on the map: stamp capsules and bow-wave deposits, RMW on the
// texel EvolveCS wrote this frame (or the standing map on frames where the
// world had nothing to do). Every term is a function of the texel's own
// value - no neighbour reads - which is what lets this pass run in place and
// be dispatched over the stamps' bounding tiles only. Returns whether the
// texel holds anything, for the group's occupancy write.
bool StampTexel(uint2 phys)
{
	uint2 dims;
	CurrentDeformation.GetDimensions(dims.x, dims.y);
	// Inverse of TorusPhys: the world position and the activity bookkeeping
	// (bbox, view) live in logical space.
	const uint2 pixel = uint2((int2(phys) - MapOrigin) & (int2(dims) - 1));

	const float4 carried = CurrentDeformation[phys];
	float deformation = carried.x;
	float melted = carried.y;
	float crust = carried.z;
	float deposit = carried.w;

	float2 worldPos = WindowOrigin + (float2(pixel) + 0.5) * TexelSize;

	// Carve, melt and scorch accumulate separately so the result cannot depend
	// on the order stamps happen to sit in the buffer.
	//
	// Melt follows the campfire basins in SnowExclusions.hlsli, where the
	// falloff IS the depth at a point rather than a speed toward one. That
	// distinction is the whole shape: scaling the RATE lets every texel under
	// the stamp keep deepening until it saturates - the rim merely arrives
	// late - so a source left standing converges on a flat-floored cylinder.
	// Scaling the TARGET gives each texel its own ceiling, so the bowl is
	// permanent and a hotter source reaches the same bowl sooner instead of
	// digging a deeper one.
	float carve = deformation;
	float meltTarget = 0.0;
	float meltRate = 0.0;
	float scorch = max(-melted, 0.0);
	float crustTarget = 0.0;
	float crustRate = 0.0;
	// How much of the standing crust gets smashed through this frame.
	float crustBreak = 0.0;
	// Crust as it stands BEFORE this frame's stamps, which is what a boot
	// landing on it has to get through.
	const float standingCrust = crust;

	for (uint i = 0; i < StampCount; i++) {
		// Capsule stamp: distance to the segment from the actor's previous
		// position, so trails are continuous regardless of movement speed.
		float2 p0 = StampEnds[i].xy;
		float2 p1 = Stamps[i].xy;
		float2 seg = p1 - p0;
		float segLenSq = dot(seg, seg);
		float t = segLenSq > 1e-4 ? saturate(dot(worldPos - p0, seg) / segLenSq) : 0.0;
		float2 delta = worldPos - (p0 + seg * t);
		float distSq = dot(delta, delta);
		float radius = Stamps[i].w;

		// A cone is contained in the capsule of the same radius about the same
		// axis, so the gate below needs no change at all - only the falloff
		// coordinate inside it does.
		float modeRaw = StampEnds[i].z;
		bool isBowl = modeRaw > STAMP_MODE_BOWL - 0.5;
		float modeNoBowl = isBowl ? modeRaw - STAMP_MODE_BOWL : modeRaw;
		bool isCone = modeNoBowl > STAMP_MODE_CONE - 0.5;
		float mode = isCone ? modeNoBowl - STAMP_MODE_CONE : modeNoBowl;

		// Melt edge noise can push the falloff outward, so the gate widens
		// by it. Pits reach furthest of all - their arc legs run past the
		// radius.
		float gateRadius = radius * max(PIT_LOBE_MAX + PIT_LOBE_WIDTH,
									 1.0 + MeltEdgeNoise);
		[branch] if (distSq < gateRadius * gateRadius)
		{
			float dist = sqrt(distSq);

			// Normalised distance to the edge of the shape: 0 on the axis, 1
			// at the rim. Everything below shapes its falloff from this, so a
			// cone and a capsule share every profile they have.
			float edgeCoord = dist / max(radius, 1e-3);
			[branch] if (isCone)
			{
				float axisLen = sqrt(segLenSq);
				float2 axis = axisLen > 1e-3 ? seg / axisLen : float2(1.0, 0.0);
				float2 rel = worldPos - p0;
				float along = dot(rel, axis);
				// Perpendicular offset, via the 2D cross product.
				float perp = abs(rel.x * axis.y - rel.y * axis.x);
				float axisT = along / max(axisLen, 1e-3);
				// One slope, straight from the apex. radius and axisLen scale
				// together while the front travels, so this is the same wedge
				// at every extension - only longer.
				float halfWidth = max(radius * axisT, 1e-3);
				edgeCoord = perp / halfWidth;
				// Behind the apex, or past the front, is outside the shape.
				// The far end is a clean edge on purpose: it is where the
				// shockwave got to, not somewhere it faded away.
				edgeCoord = (along < 0.0 || along > axisLen) ? 1e6 : edgeCoord;
			}

			[branch] if (mode < 0.5)
			{
				float edgeDist = edgeCoord;
				// Falloff from StampFalloffStart of the radius: low values keep a
				// wide edge band coarser consumers of the map can still represent,
				// high values hold full depth almost to the edge. A BOWL carve
				// takes the melt bowl's floor instead, which begins falling away
				// almost at the centre - so the cross-section curves the whole
				// way rather than standing walls up around a flat floor.
				float falloff = 1.0 - smoothstep(isBowl ? CARVE_BOWL_FLOOR : StampFalloffStart, 1.0, edgeDist);

				// Crusted snow bears weight. A print on it is shallow rather
				// than absent, and something heavy enough breaks through and
				// takes the crust with it - StampEnds[i].w carries how much
				// weight this shape puts through, unused by carves otherwise.
				float force = saturate(StampEnds[i].w);
				float resist = standingCrust * (1.0 - force);
				float printed = Stamps[i].z * falloff * lerp(1.0, CrustPrintDepth, resist);
				carve = max(carve, printed);

				// Anything that CUTS a crust has broken the skin over it, not
				// merely dented it - so the cut takes the glaze with it, in
				// proportion to how deep it went. Without this a trench
				// through ice keeps its polish all the way down and reads as
				// a groove ploughed through ice cream.
				crustBreak = max(crustBreak, saturate(max(force, printed * CrustBreakOnCarve) * falloff));
			}
			else if (mode > 2.5)
			{
				// Crust: frost hardens the surface without moving any snow, so
				// depth is left entirely alone. Sustained, so it approaches a
				// target at a rate exactly as melt does - a wall glazing for
				// ten seconds sets harder than one that flickered.
				float glazeWobble = 1.0;
				[branch] if (MeltEdgeNoise > 0.001)
				{
					float wobble = 0.5 * StampNoise(worldPos / MELT_NOISE_FINE) +
					               0.5 * StampNoise(worldPos / MELT_NOISE_COARSE);
					glazeWobble = 1.0 + (wobble - 0.5) * 2.0 * MeltEdgeNoise;
				}
				float falloff = 1.0 - smoothstep(MeltFloorStart, 1.0, edgeCoord / max(glazeWobble, 1e-3));
				crustTarget = max(crustTarget, Stamps[i].z * falloff);
				crustRate += Stamps[i].z * StampEnds[i].w * falloff;
			}
			else if (mode > 1.5)
			{
				// Pit: a core with arc legs, all of it pocked. Everything is
				// seeded from the stamp's own centre rather than from time, so
				// a strike keeps the same shape frame after frame instead of
				// boiling, and two strikes never share one.
				float2 centre = Stamps[i].xy;
				float ringFrac = StampEnds[i].w;

				// A cloak pocks a RING at its reach rather than a bowl at its
				// feet, so the core distance folds around the ring radius.
				float coreDist = ringFrac > 0.001 ?
				                     abs(dist - radius * ringFrac) / max(radius * (1.0 - ringFrac), 1e-3) :
				                     edgeCoord;
				float shape = 1.0 - smoothstep(0.15, 1.0, coreDist);

				// Arc legs fork from a POINT of discharge, so they mean nothing
				// on a wedge - and their seed is the stamp centre, which for a
				// cone is its far end rather than anywhere it struck. Branched
				// around rather than given a loop count of zero: an unrolled
				// loop needs a bound the compiler can see.
				[branch] if (!isCone)
				[unroll] for (int lobe = 0; lobe < PIT_LOBES; lobe++) {
					float2 seed = centre * 0.05 + float2(lobe * 7.3, lobe * 3.1);
					float angle = StampNoiseHash(floor(seed)) * 6.2831853;
					float legLength = radius * lerp(PIT_LOBE_MIN, PIT_LOBE_MAX,
						StampNoiseHash(floor(seed + 31.7)));
					float2 tip = centre + float2(cos(angle), sin(angle)) * legLength;

					// Distance to the leg, which tapers to nothing at the tip.
					float2 legVec = tip - centre;
					float legLenSq = max(dot(legVec, legVec), 1e-4);
					float legT = saturate(dot(worldPos - centre, legVec) / legLenSq);
					float2 legDelta = worldPos - (centre + legVec * legT);
					float legWidth = radius * PIT_LOBE_WIDTH * (1.0 - 0.75 * legT);
					shape = max(shape, 1.0 - smoothstep(0.2, 1.0, length(legDelta) / max(legWidth, 1e-3)));
				}

				// Pocked, not scored: the mask cuts the shape into fragments so
				// a strike reads as snow blasted apart rather than a drawn star.
				float pockCell = max(radius / PIT_POCK_CELLS_ACROSS, PIT_POCK_CELL_MIN);
				float pock = StampNoise(worldPos / pockCell);
				// Floored well below 1 so the mask always leaves something: a
				// strike that lands must mark, even where the pocking thins.
				shape *= max(smoothstep(0.30, 0.62, pock), 0.45);

				// CARVE's model, not melt's: the throw is instantaneous, so it
				// is max-blended to an instant depth. Accumulating instead
				// would let a standing lightning cloak bore a shaft.
				float pitDepth = Stamps[i].z * shape;
				carve = max(carve, pitDepth);
				scorch = max(scorch, pitDepth);
			}
			else
			{
				// Melt: the noise scales the RADIUS instead, so the rim
				// wanders while the profile stays a smooth bowl. Perturbing
				// the falloff distance here instead would pit the floor,
				// because noise inside the flat core drags samples past the
				// start of the flank.
				float meltWobble = 1.0;
				[branch] if (MeltEdgeNoise > 0.001)
				{
					float wobble = 0.5 * StampNoise(worldPos / MELT_NOISE_FINE) +
					               0.5 * StampNoise(worldPos / MELT_NOISE_COARSE);
					meltWobble = 1.0 + (wobble - 0.5) * 2.0 * MeltEdgeNoise;
				}
				float falloff = 1.0 - smoothstep(MeltFloorStart, 1.0, edgeCoord / max(meltWobble, 1e-3));
				// Depth this texel melts TO, and how fast it gets there. The
				// rate carries the same falloff, so the whole basin reaches
				// its profile together and the bowl is visible from the first
				// second instead of opening outward from the middle.
				meltTarget = max(meltTarget, Stamps[i].z * falloff);
				meltRate += Stamps[i].z * StampEnds[i].w * falloff;
			}
		}
	}

	float total = carve;
	[flatten] if (meltTarget > total)
		total = min(total + meltRate * DeltaTime, meltTarget);
	total = min(total, 1.0);

	// Melt-origin depth is recorded positive so the berm field can subtract it;
	// scorch is recorded negative so it keeps its berm and can darken the
	// shell. Melt wins the channel where both somehow land, since snow that
	// has gone cannot also be burnt.
	float meltedNow = min(melted + max(total - carve, 0.0), total);

	// What was smashed this frame comes off the crust that was ALREADY there,
	// and only then does this frame's frost set on what is left. The order is
	// what lets a cold heavy thing trench snow and leave the trench frozen
	// behind it - a frost atronach walking, which nothing else in the design
	// produces, since fire removes snow and force displaces it. Growing first
	// and breaking afterwards destroyed a glaze that had not existed when the
	// foot landed, so the two cancelled and the trail came out bare.
	//
	// Carving still breaks a standing crust exactly as before: the break is
	// applied to `crust`, which is the glaze the boot actually met.
	float crustNow = saturate(crust * (1.0 - crustBreak));
	[flatten] if (crustTarget > crustNow)
		crustNow = min(crustNow + crustRate * DeltaTime, crustTarget);
	crustNow = saturate(crustNow);

	// Bow-wave deposit. The SAME crescent the shell draws, MAXed in at
	// wherever the crest stands this frame: max, not accumulate, so passing
	// twice does not build a wall, and so the field records the high-water
	// mark of what was shouldered rather than a running total.
	[branch] if (DepositParams.x > 0.5)
	{
		float crest = 0.0;
		// How much of the OLD deposit this frame's crest region owns. The
		// crest is the current state of the snow it is pushing, so wherever
		// it reaches, whatever was written there before is stale and must go
		// before the new value is laid down. This is what tells a WAVE from a
		// TRENCH: ground the crest has already swept over is
		// walked ground - trench - and gets cleared, while ground it has not
		// reached yet keeps the pile that was pushed onto it. Without it the
		// crest wrote its ring at every step and never took any of it back,
		// so the whole corridor kept a thin deposit and the chunk noise stood
		// that up as a row of icicles along the path.
		float wipe = 0.0;
		const uint waveCount = (uint)DepositParams.x;
		[loop] for (uint w = 0; w < waveCount; w++)
		{
			// CAPSULE, not a point: the crest hugs the swept path
			// the foot actually took this frame - the same segStart -> tip
			// capsule the trench stamps use.
			const float2 tipPos = DepositPosDir[w].xy;
			const float2 prevPos = DepositShape[w].zw;
			const float2 fwd = DepositPosDir[w].zw;
			const float2 sideDir = float2(-fwd.y, fwd.x);
			const float2 seg = tipPos - prevPos;
			const float segLenSq = dot(seg, seg);
			const float segT = segLenSq > 1e-4 ? saturate(dot(worldPos - prevPos, seg) / segLenSq) : 0.0;
			const float2 onPath = prevPos + seg * segT;
			const float radius = max(DepositShape[w].x, 1e-3);

			// ANCHORED AT THE FOOT, STRETCHED FORWARD: the
			// wave starts where the trench ends, always. Offsetting the lobe's
			// centre forward instead detaches the mound from the trench mouth
			// as Reach goes up. The shape's near edge stays
			// pinned at the foot and Reach only DIVIDES the forward axis, so
			// turning it up extends the hill onward from the same start
			// instead of moving it away.
			const float reach = max(DepositParams.y, 0.25);
			const float2 rel = worldPos - onPath;
			const float along = dot(rel, fwd);
			const float across = dot(rel, sideDir);
			// Forward distances are compressed by Reach - the lobe covers
			// [0 .. ~1.45 x radius x reach] ahead; behind is left at true
			// scale and the angular term kills it anyway.
			const float2 shaped = float2(along > 0.0 ? along / reach : along, across);
			const float d = length(shaped);
			[branch] if (d > radius * 1.45)
				continue;
			const float t = d / radius;
			// Zero AT the foot (the walker stands in the trench they just
			// cut), rising to the crest ahead.
			const float radial = smoothstep(0.25, 0.85, t) * (1.0 - smoothstep(0.95, 1.45, t));
			const float forward = d > 1e-3 ? shaped.x / d : 1.0;
			const float halfAngle = saturate(forward * 0.5 + 0.5);
			const float angular = pow(halfAngle, lerp(0.35, 3.0, DepositParams.z));
			const float shape = radial * angular;
			crest = max(crest, shape * DepositShape[w].y);

			// THE WIPE NEVER REACHES THE PILE (the final melt fix).
			// The wipe is a per-frame MULTIPLICATIVE cut, so any leak onto
			// the pile compounds at frame rate. A claim mask keyed to the crest
			// shape being exactly 1 protects almost nowhere, and the flanks melt
			// within a second of the fade.
			// No shape algebra survives that; geometry does: the pile lies
			// AHEAD of the foot by construction, so the wipe is confined to
			// ground at or BEHIND the leading edge, plus the corridor's
			// sides. Ground ahead is structurally unreachable by it. When
			// the walker advances, yesterday's pile falls behind the new
			// foot and is cleared; when the walker stops, the pile is ahead
			// of a foot that never comes, and stands for good.
			const float behind = 1.0 - smoothstep(0.05 * radius, 0.35 * radius, along);
			const float lateral = 1.0 - smoothstep(1.4, 2.1, abs(across) / radius);
			wipe = max(wipe, behind * lateral * DepositShape[w].y);
		}
		// Clear what the crest region owns, THEN lay this frame's crest into
		// it. Order matters: the pile ahead of the walker is written after
		// the wipe, so it survives; the pile the walker has drawn level with
		// is wiped and not rewritten (the forward lobe no longer covers it),
		// which is how the corridor behind comes out clean. When the walker
		// stops, wave strength decays, no crest is emitted, nothing wipes -
		// and the last pile pushed stands there for good.
		deposit *= saturate(1.0 - wipe);
		// Only on snow that is still standing: a crest cannot pile up out of
		// ground that has already been dug away.
		deposit = max(deposit, crest * saturate(1.0 - total));
	}

	float4 result = float4(total,
		meltedNow > 0.0 ? meltedNow : -min(scorch, 1.0), crustNow, deposit);
	CurrentDeformation[phys] = result;

	ActivityAccumulate(StoredDelta(result, carried), pixel);
	return any(result != 0.0);
}

// One group per CPU-listed tile: actors' surroundings, nothing else. The
// group owns its whole tile, so it writes occupancy absolutely.
[numthreads(8, 8, 1)] void StampCS(uint3 GTid
								   : SV_GroupThreadID, uint3 Gid
								   : SV_GroupID, uint GIdx
								   : SV_GroupIndex) {
	ActivityReset(GIdx);
	const uint packed = StampTiles[Gid.x];
	const uint2 tile = uint2(packed & 0xFFFFu, packed >> 16u);
	if (StampTexel(tile * 8u + GTid.xy))
		InterlockedOr(gNonZero, 1u);
	ActivityPublish(GIdx);
	if (GIdx == 0) {
		Occupancy[tile] = gNonZero;
		if (gActivity != 0)
			BermDirty[tile] = 1u;
	}
}

// Full-map fallback for a tile list past its cap; also the force-all-dirty
// debug path, so "my trenches froze" has a one-click cross-check.
[numthreads(8, 8, 1)] void StampAllCS(uint3 DTid
									  : SV_DispatchThreadID, uint3 Gid
									  : SV_GroupID, uint GIdx
									  : SV_GroupIndex) {
	ActivityReset(GIdx);
	if (StampTexel(DTid.xy))
		InterlockedOr(gNonZero, 1u);
	ActivityPublish(GIdx);
	if (GIdx == 0) {
		Occupancy[Gid.xy] = gNonZero;
		if (gActivity != 0)
			BermDirty[Gid.xy] = 1u;
	}
}
