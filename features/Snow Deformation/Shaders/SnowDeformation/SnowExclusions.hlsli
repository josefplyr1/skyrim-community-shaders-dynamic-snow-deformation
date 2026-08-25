// Exclusion zone evaluation, shared by the near mask and the wide field.
//
// Exclusions are the places snow should NOT lie: doors, fires, worked
// stations, bedding, sealed containers. Each is nothing but a position, a
// radius, a direction and a type, so
// the answer at a point is pure arithmetic over the constant buffer - no
// geometry involved. That is why this can be evaluated at any range, unlike
// the SHELTER term (roofs, tents, walkways), which needs a top-down render of
// real geometry and so stays inside the near window.
//
// CombineCS (HeightMapProcessCS) evaluates this at the near window's
// resolution; ExclusionFieldCS bakes it over a far wider, coarser window. Both
// call the same function so the two can never disagree about a bowl's shape.
// ONE exception, and it is a deliberate one: sealed containers (type 3) are
// too small for the wide field's 32-unit texel to represent, so that pass
// skips them and the near mask alone owns them. See ExclusionFieldCS.

#ifndef SNOW_EXCLUSIONS_HLSLI
#define SNOW_EXCLUSIONS_HLSLI

// Must match kMaxExclusions in src/Features/SnowDeformation.h. On overflow the
// gather keeps the nearest sources (distance sort CPU-side).
#define MAX_EXCLUSIONS 256

cbuffer ExclusionCB : register(b1)
{
	float4 ExclusionPosRadius[MAX_EXCLUSIONS];   // xyz = position, w = radius (rects: half-extent ACROSS the facing)
	float4 ExclusionDirExtType[MAX_EXCLUSIONS];  // doors (w=0): xy = facing, z = forward extent. Fires (w=1 noisy, w=2 smooth): xy = elongation axis x (aspect-1), z = melt strength. Sealed containers (w=3): xy = unit facing, z = half-extent ALONG it
	uint ExclusionCount;
	float3 exclusionPad;
}

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

struct ExclusionResult
{
	// Coverage fades to bare ground (doors).
	float Suppress;
	// Depth thins toward a floor that never vanishes (fires, stations, bedding).
	float Melt;
	// How far the height field is pulled back to the terrain. Accumulated
	// multiplicatively, matching the successive lerps this replaced: two
	// overlapping clearings flatten more than either alone, where a plain max
	// would let the weaker one be swallowed.
	float Flatten;
};

// Influence of ONE exclusion at worldXY, given the terrain height under it.
// Z-gated at 300 units so an upper-floor door does not clear ground snow far
// below, while sunken cave entrances still qualify. Exposed separately so the
// field bake can drive it from a per-tile culled list.
//
// Sealed containers use a TIGHTER band. The band is not a tolerance here, it
// is the condition itself: the shell rides the terrain, so it can only push
// up inside a coffin that sits ON the ground. One standing on a ruin's raised
// floor is out of the shell's reach already, and a 300-unit gate would answer
// it by clearing a rectangle of ground far below instead.
ExclusionResult EvaluateExclusionAt(uint index, float2 worldXY, float terrain)
{
	ExclusionResult result;
	result.Suppress = 0.0;
	result.Melt = 0.0;
	result.Flatten = 0.0;

	float3 center = ExclusionPosRadius[index].xyz;
	float radius = ExclusionPosRadius[index].w;
	float4 dirExtType = ExclusionDirExtType[index];
	float zBand = dirExtType.w > 2.5 ? 120.0 : 300.0;
	[branch] if (abs(center.z - terrain) < zBand)
	{
		float2 d = worldXY - center.xy;
		float influence = 0.0;
		[branch] if (dirExtType.w > 2.5)
		{
			// Sealed container (draugr sarcophagi): an oriented RECTANGLE
			// that kills coverage outright rather than thinning depth. A
			// coffin shut for centuries holds no snow, and melting toward a
			// floor would still leave a white sheet lying in the open box.
			//
			// The rectangle is the reference's own footprint and the edge
			// fades across its outer 15%, so the falloff band lands under
			// the stone walls. An exclusion that reached its full strength
			// at the silhouette would ring every coffin with bare ground.
			float u = dot(d, dirExtType.xy);
			float v = dot(d, float2(-dirExtType.y, dirExtType.x));
			float e = max(abs(u) / max(dirExtType.z, 1.0), abs(v) / max(radius, 1.0));
			influence = 1.0 - smoothstep(0.85, 1.0, e);
			result.Suppress = influence;
		}
		else if (dirExtType.w < 0.5)
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
			result.Suppress = influence;
		}
		else
		{
			// Melt bowl - full melt in the core, then a long gradual rise.
			// Type 1 (fires): two noise octaves, fine wobble plus large-scale
			// shape irregularity, so no two bowls read as stamped circles.
			// Type 2 (bedding): smooth edge, so a bedroll's bowl joins a
			// tent's shelter sink cleanly. dirExtType.xy = elongation axis x
			// (aspect-1): compressing the along-axis distance stretches the
			// bowl into an oval centered on the object. dirExtType.z = melt
			// strength.
			float2 dEff = d;
			float axisLen = length(dirExtType.xy);
			[branch] if (axisLen > 0.001)
			{
				float2 axisDir = dirExtType.xy / axisLen;
				dEff -= axisDir * dot(d, axisDir) * (axisLen / (1.0 + axisLen));
			}
			float noisy = 1.0;
			[branch] if (dirExtType.w < 1.5)
			{
				noisy = 0.7 + 0.35 * ExclusionNoise(worldXY) + 0.35 * ExclusionNoise(worldXY * 0.3);
			}
			float noisyRadius = radius * noisy;
			float dist = length(dEff);
			influence = (1.0 - smoothstep(noisyRadius * 0.35, noisyRadius, dist)) * dirExtType.z;
			result.Melt = influence;
		}
		result.Flatten = influence;
	}

	return result;
}

// Every exclusion in the buffer, combined.
ExclusionResult EvaluateExclusions(float2 worldXY, float terrain)
{
	ExclusionResult result;
	result.Suppress = 0.0;
	result.Melt = 0.0;
	result.Flatten = 0.0;

	for (uint exclusionI = 0; exclusionI < ExclusionCount; exclusionI++) {
		ExclusionResult one = EvaluateExclusionAt(exclusionI, worldXY, terrain);
		result.Suppress = max(result.Suppress, one.Suppress);
		result.Melt = max(result.Melt, one.Melt);
		result.Flatten += one.Flatten * (1.0 - result.Flatten);
	}

	return result;
}

#endif
