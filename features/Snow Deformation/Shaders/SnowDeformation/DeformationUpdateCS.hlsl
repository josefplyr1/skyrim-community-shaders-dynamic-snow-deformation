// Persistent snow deformation map update.
//
// The map is a square world-space window following the camera in whole-texel
// steps. Each frame the previous map is re-read at a scrolled offset, snow
// refill is applied, and this frame's stamps are blended in.
// Texel value = normalized depression depth (0 = untouched, 1 = ground).
//
// Three stamp classes share the buffer, selected per stamp by StampEnds[i].z:
//   CARVE (0) - a shape displaces snow. Instantaneous depth, max-blended:
//               standing in a trench does not deepen it.
//   MELT  (1) - a heat source removes snow while it stands there. Additive
//               and dt-scaled, so DWELL TIME is what deepens the bowl.
//   PIT   (2) - a discharge throws snow aside. Follows CARVE, not melt: the
//               displacement is instantaneous, so a lightning cloak standing
//               over one spot pocks it once rather than boring downward. It
//               also SCORCHES, and scorched snow is displaced snow - it keeps
//               its berm, where melted snow has none.
// A stamp may also be a CONE rather than a capsule, flagged by adding
// STAMP_MODE_CONE to its mode. It needs no extra fields: a capsule is already
// two points and a radius, and a cone is the same three read differently -
// apex at the segment start, axis to the segment end, radius = the half-width
// it has opened to by the far end. Only shouts use it, because a shout is the
// one source whose footprint is a wedge, and a row of overlapping discs cannot
// stand in for one: every stamp holds full depth only across the inner tenth
// of its radius, so ten discs read as ten craters with shallow gaps.
//
//   CRUST (3) - frost refreezes the surface. Sustained, so it follows MELT:
//               approaches a target at a rate, and a wall glazing for ten
//               seconds sets harder than one that flickered. It moves no snow
//               at all - depth is untouched - it only hardens what is there.
// Melted ground stays bare longer than trampled ground, because the ground
// under a fire is warm and wet after the flame is gone. That is applied as a
// SLOWER REFILL on melted texels, not as extra depth: depth is capped at 1.0
// so the bowl profile below survives intact. Banking the persistence as
// over-depth instead would flatten it - every consumer saturates at 1.0, so
// the whole over-melted core collapses onto one plateau and the bowl becomes
// a flat-floored pit with walls.
//
// Channels:
//   .x  total depression depth.
//   .y  SIGNED surface state, because the two things it records are mutually
//       exclusive - snow that melted away cannot also be scorched solid:
//         > 0  the portion of .x that was MELTED rather than displaced. Melted
//              snow leaves no spoil, so the berm field subtracts it.
//         < 0  SCORCH, from a shock discharge. Displaced, so it keeps its full
//              berm, and its magnitude darkens the shell.
//       Berm reads x - max(y, 0); scorch reads max(-y, 0).
//   .z  CRUST: refrozen snow. Resists being carved and shades as ice. Frost
//       neither removes snow nor throws it, so it is neither of the above and
//       needed a channel of its own.
//   .w  unused. Kept deliberately: BLOOD-DESIGN.md needs exactly this kind of
//       per-texel surface value, and widening again later would cost another
//       33 MB for one field.

#define MAX_STAMPS 256

// Added to a stamp's mode to mark it a cone. Mirrored by kStampModeCone in
// SnowDeformation.h.
#define STAMP_MODE_CONE 10.0
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

cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	int2 ScrollDelta;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	// Lower smoothstep edge of the stamp falloff (fraction of radius):
	// higher = steeper trench walls.
	float StampFalloffStart;
	// Fraction-of-radius noise wobbling each stamp's edge.
	float StampNoiseAmp;
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
	float perFramePad;

	float4 Stamps[MAX_STAMPS];     // xy: world pos, z: depth (carve) or strength (melt), w: radius
	float4 StampEnds[MAX_STAMPS];  // xy: previous world pos (capsule start), z: 0 carve / 1 melt, w: melt rate (depth per second)
}

Texture2D<float4> PreviousDeformation : register(t0);
RWTexture2D<float4> CurrentDeformation : register(u0);

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

[numthreads(8, 8, 1)] void main(uint3 DTid
								: SV_DispatchThreadID) {
	uint2 pixel = DTid.xy;

	float deformation = 0.0;
	// Melted portion of `deformation`, carried so the shells can tell a
	// melt basin from a dug trench.
	float melted = 0.0;
	float crust = 0.0;

	if (!ClearMap) {
		int2 sourcePixel = int2(pixel) + ScrollDelta;

		uint2 dims;
		PreviousDeformation.GetDimensions(dims.x, dims.y);

		[branch] if (all(sourcePixel >= 0) && all(sourcePixel < int2(dims)))
		{
			float4 previous = PreviousDeformation[uint2(sourcePixel)];
			deformation = previous.x;
			melted = previous.y;
			crust = previous.z;
		}

		// Wind-biased refill: recovery scales with the intact snow a few
		// texels upwind (the drift supply), so carved areas fill from their
		// upwind edge and the fill front marches downwind. Calm weather
		// falls back to uniform refill.
		float refill = RefillAmount;
		float windStrength = length(WindBias);
		[branch] if (refill > 0.0 && windStrength > 0.001)
		{
			int2 upwindPixel = sourcePixel - int2(round(WindBias / windStrength * DRIFT_FETCH_TEXELS));
			float upwindDeformation = deformation;
			[branch] if (all(upwindPixel >= 0) && all(upwindPixel < int2(dims)))
			{
				upwindDeformation = PreviousDeformation[uint2(upwindPixel)].x;
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
		crust = max(crust - refill - CrustThaw * DeltaTime, 0.0);
	}

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
		bool isCone = modeRaw > STAMP_MODE_CONE - 0.5;
		float mode = isCone ? modeRaw - STAMP_MODE_CONE : modeRaw;

		// Either edge treatment can push the falloff outward, so the gate
		// widens by whichever reaches further.
		// Pits reach furthest of all - their arc legs run past the radius.
		float gateRadius = radius * max(PIT_LOBE_MAX + PIT_LOBE_WIDTH,
									 1.0 + max(0.5 * StampNoiseAmp, MeltEdgeNoise));
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
				// Carve: high-frequency noise ON the falloff distance, which
				// churns the edge the way a boot breaks snow.
				float edgeDist = edgeCoord;
				[branch] if (StampNoiseAmp > 0.001)
				{
					edgeDist += (StampNoise(worldPos * 0.125) - 0.5) * StampNoiseAmp;
				}
				// Falloff from StampFalloffStart of the radius: low values keep a
				// wide edge band coarser consumers of the map can still represent,
				// high values hold full depth almost to the edge.
				float falloff = 1.0 - smoothstep(StampFalloffStart, 1.0, edgeDist);

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

	// Alpha is written as 1, not 0. Nothing reads it yet - it is being kept for
	// blood - but the ImGui debug preview blends the map with its alpha, and a
	// zero there renders the whole thing invisible. Whatever claims .w later
	// needs its own debug view rather than this one.
	CurrentDeformation[pixel] = float4(total,
		meltedNow > 0.0 ? meltedNow : -min(scorch, 1.0), crustNow, 1.0);
}
