// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#ifndef __SNOW_NOISE_DEPENDENCY_HLSL__
#define __SNOW_NOISE_DEPENDENCY_HLSL__

// Constant-free noise core, split out of SnowFields.hlsli so the undulation
// bake CS evaluates the EXACT functions the shells do without pulling in
// their ShellCB context. Both shells reach these through SnowFields.hlsli.

// World-anchored value noise, shared by the border domain warp, the dune
// undulation and the churn (and any other organic-edge shaping).
float ShapeNoiseHash(float2 cell)
{
	float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float ShapeNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(ShapeNoiseHash(i), ShapeNoiseHash(i + float2(1, 0)), f.x),
		lerp(ShapeNoiseHash(i + float2(0, 1)), ShapeNoiseHash(i + float2(1, 1)), f.x), f.y);
}

// The dune field's amp-free core: two octaves in [-1, 1]. The shells apply
// UndulationAmp live (so the strength slider needs no rebake) and the bake
// CS stores exactly this.
float UndulationNorm(float2 worldXY, float scale)
{
	float2 p = worldXY / max(scale, 0.05);
	float n = ShapeNoise(p / 340.0) * 0.72 + ShapeNoise(p / 110.0) * 0.28;
	return (n - 0.5) * 2.0;
}

#endif  // __SNOW_NOISE_DEPENDENCY_HLSL__
