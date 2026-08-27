// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Snow deformation sampling for the lighting pixel shader.
//
// The deformation map is a world-space window (absolute coordinates) around
// the camera, produced each frame by DeformationUpdateCS. Texel value is
// normalized depression depth: 0 = untouched snow, 1 = compressed to ground.

namespace SnowDeformation
{
	Texture2D<float4> DeformationMap : register(t101);
	// Shell snow albedo + tangent normals for the horizon LOD-terrain recolor.
	Texture2D<float4> HorizonSnowAlbedo : register(t102);
	Texture2D<float4> HorizonSnowNormal : register(t103);

	// Must match kSnowUVTile in SnowShell.hlsl: identical world tiling on the
	// shell and the recolored LOD is what makes the handoff invisible.
	static const float SnowUVTile = 4096.0 / 24.0;

	// Snow classification of a baked LOD terrain texel: bright and
	// desaturated (gamma-space input). Must match ClassifyLODSnow in
	// TerrainWindowFillCS.hlsl so the shell's far coverage and the horizon
	// recolor agree on where snow is.
	float ClassifyLODSnow(float3 gammaColor)
	{
		float luminance = dot(gammaColor, float3(0.2126, 0.7152, 0.0722));
		float saturation = max(gammaColor.r, max(gammaColor.g, gammaColor.b)) - min(gammaColor.r, min(gammaColor.g, gammaColor.b));
		float lumLo = 0.62 - 0.64 * saturate(SharedData::snowDeformationSettings.LODSnowSensitivity);
		return smoothstep(lumLo, lumLo + 0.12, luminance) * (1.0 - smoothstep(0.10, 0.22, saturation));
	}

	float2 GetDeformationUV(float2 absWorldXY)
	{
		return (absWorldXY - SharedData::snowDeformationSettings.WindowOrigin) * SharedData::snowDeformationSettings.InvWorldSize;
	}

	// Bilinear at fractional LOGICAL texel coordinates. Load-based on
	// purpose: the map is toroidal, and a hardware sampler would bilinear
	// across the physical wrap seam, mixing two unrelated world locations.
	// Clamp logically first (the map border is the window border), then mask
	// each tap to physical.
	float DeformBilinear(float2 t, float2 mapDim)
	{
		t = clamp(t, 0.0, mapDim - 1.001);
		int2 t0 = (int2)t;
		float2 f = t - t0;
		int2 t1 = min(t0 + 1, int2(mapDim) - 1);

		const int2 mask = int2(mapDim) - 1;
		const int2 origin = SharedData::snowDeformationSettings.DeformMapOrigin;
		int2 q0 = (t0 + origin) & mask;
		int2 q1 = (t1 + origin) & mask;

		float s00 = DeformationMap.Load(int3(q0.x, q0.y, 0)).x;
		float s10 = DeformationMap.Load(int3(q1.x, q0.y, 0)).x;
		float s01 = DeformationMap.Load(int3(q0.x, q1.y, 0)).x;
		float s11 = DeformationMap.Load(int3(q1.x, q1.y, 0)).x;

		// Saturated per tap: melt writes past 1.0 into the refill headroom
		// and only the visible 0-1 range shades; the bicubic weights below
		// are convex, so this bounds the result too.
		return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
	}

	float GetDeformation(float2 absWorldXY)
	{
		float2 uv = GetDeformationUV(absWorldXY);

		// Fade near the window border so the effect never pops at the edge.
		// LOGICAL uv: the physical rotation never reaches this math.
		float2 edge = min(uv, 1.0 - uv);
		float border = saturate(min(edge.x, edge.y) * 16.0);

		float deformation = 0.0;
		[branch] if (border > 0.0)
		{
			// B-spline bicubic via 4 bilinear taps: value- and gradient-
			// continuous, so normals derived from this field do not band
			// per texel. Dimensions from the texture, not a constant: the map
			// resolution is runtime (Debugging Options).
			float2 mapDim;
			DeformationMap.GetDimensions(mapDim.x, mapDim.y);
			float2 t = uv * mapDim - 0.5;
			float2 i = floor(t);
			float2 f = t - i;
			float2 f2 = f * f;
			float2 f3 = f2 * f;

			float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
			float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
			float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
			float2 w3 = f3 / 6.0;

			float2 g0 = w0 + w1;
			float2 g1 = w2 + w3;
			float2 h0 = i - 1.0 + w1 / g0;
			float2 h1 = i + 1.0 + w3 / g1;

			float s00 = DeformBilinear(float2(h0.x, h0.y), mapDim);
			float s10 = DeformBilinear(float2(h1.x, h0.y), mapDim);
			float s01 = DeformBilinear(float2(h0.x, h1.y), mapDim);
			float s11 = DeformBilinear(float2(h1.x, h1.y), mapDim);

			deformation = (g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11)) * border;
		}
		return deformation;
	}
}
