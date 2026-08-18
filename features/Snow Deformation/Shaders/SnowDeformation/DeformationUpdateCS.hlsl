// Persistent snow deformation map update.
//
// The map is a square world-space window following the camera in whole-texel
// steps. Each frame the previous map is re-read at a scrolled offset, snow
// refill is applied, and this frame's stamps are blended in.
// Texel value = normalized depression depth (0 = untouched, 1 = ground).
//
// Two stamp classes share the buffer, selected per stamp by StampEnds[i].z:
//   CARVE (0) - a shape displaces snow. Instantaneous depth, max-blended:
//               standing in a trench does not deepen it.
//   MELT  (1) - a heat source removes snow while it stands there. Additive
//               and dt-scaled, so DWELL TIME is what deepens the bowl.
// Melt may drive a texel past 1.0 into MeltCeiling headroom. Every consumer
// saturates, so that excess is invisible depth which must decay through the
// refill before ground starts covering again - melted ground stays clear
// longer than a footprint, and heat lingers after the source is gone.

#define MAX_STAMPS 256

// Upwind supply sample distance for wind-biased refill, in texels.
#define DRIFT_FETCH_TEXELS 3.0
// Melt bowls hold full strength across the inner third and then rise for a
// long way, matching the campfire basins in SnowExclusions.hlsli. Carve uses
// the much steeper StampFalloffStart instead: a trench has walls, a melt bowl
// has shoulders.
#define MELT_FALLOFF_START 0.35
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
	// Ceiling on the accumulated value, 1.0 + headroom. Exactly 1.0 disables
	// the headroom and melt then behaves like a saturating carve.
	float MeltCeiling;
	float2 perFramePad;

	float4 Stamps[MAX_STAMPS];     // xy: world pos, z: depth (carve) or strength (melt), w: radius
	float4 StampEnds[MAX_STAMPS];  // xy: previous world pos (capsule start), z: 0 carve / 1 melt, w: melt rate (depth per second)
}

Texture2D<float> PreviousDeformation : register(t0);
RWTexture2D<float> CurrentDeformation : register(u0);

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

	if (!ClearMap) {
		int2 sourcePixel = int2(pixel) + ScrollDelta;

		uint2 dims;
		PreviousDeformation.GetDimensions(dims.x, dims.y);

		[branch] if (all(sourcePixel >= 0) && all(sourcePixel < int2(dims)))
		{
			deformation = PreviousDeformation[uint2(sourcePixel)];
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
				upwindDeformation = PreviousDeformation[uint2(upwindPixel)];
			}
			refill *= lerp(1.0, (1.0 - upwindDeformation) * DRIFT_GAIN, windStrength);
		}
		deformation = max(deformation - refill, 0.0);
	}

	float2 worldPos = WindowOrigin + (float2(pixel) + 0.5) * TexelSize;

	// Carve and melt accumulate separately so the result cannot depend on the
	// order stamps happen to sit in the buffer.
	float carve = deformation;
	float melt = 0.0;

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

		// Edge noise can push the falloff outward, so the gate widens with it.
		float gateRadius = radius * (1.0 + 0.5 * StampNoiseAmp);
		[branch] if (distSq < gateRadius * gateRadius)
		{
			float edgeDist = sqrt(distSq) / radius;
			[branch] if (StampNoiseAmp > 0.001)
			{
				edgeDist += (StampNoise(worldPos * 0.125) - 0.5) * StampNoiseAmp;
			}
			[branch] if (StampEnds[i].z < 0.5)
			{
				// Falloff from StampFalloffStart of the radius: low values keep a
				// wide edge band coarser consumers of the map can still represent,
				// high values hold full depth almost to the edge.
				float falloff = 1.0 - smoothstep(StampFalloffStart, 1.0, edgeDist);
				carve = max(carve, Stamps[i].z * falloff);
			}
			else
			{
				float falloff = 1.0 - smoothstep(MELT_FALLOFF_START, 1.0, edgeDist);
				melt += Stamps[i].z * StampEnds[i].w * DeltaTime * falloff;
			}
		}
	}

	CurrentDeformation[pixel] = min(carve + melt, MeltCeiling);
}
