// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Terrain-window far fill.
//
// Runs once per window rebuild, after the CPU upload of baked cell data:
// texels that no baked cell covers (sentinel height) get their height from
// the xLODGen worldspace heightmap (the Terrain Shadows data source) and
// their snow coverage from the game's own LOD terrain diffuse — where the
// baked LOD texture is snow, our snow is placed, so the far shell follows
// the hand-painted snow instead of an elevation guess. The snow-line
// heuristic survives only as the fallback for texels without an LOD tile.
// Texel .w records provenance for the debug view: 1 = snow-line fallback,
// 2 + score = LOD-classified.

RWTexture2D<float4> TerrainWindow : register(u0);
Texture2D<float> HeightMap : register(t0);
// 2x2 block of level-32 LOD terrain diffuse tiles covering the window.
Texture2D<float4> LODTile00 : register(t1);
Texture2D<float4> LODTile10 : register(t2);
Texture2D<float4> LODTile01 : register(t3);
Texture2D<float4> LODTile11 : register(t4);
SamplerState LinearSampler : register(s0);

cbuffer WindowFillCB : register(b0)
{
	// World XY of window texel (0,0); texels are vertex-aligned (no half-texel).
	float2 WindowOriginWorld;
	float TexelSize;
	uint WindowDim;

	// world.xy * Scale + Offset = heightmap UV (Terrain Shadows convention).
	float2 HeightMapScale;
	float2 HeightMapOffset;

	// Decoded height = lerp(HeightRange.x, HeightRange.y, sample), where
	// HeightRange is the metadata's pos0.z/pos1.z — the range the file's
	// values are normalized over (+-32767*8 for xLODGen exports). This is
	// the convention ShadowUpdate.cs.hlsl decodes with; the metadata's
	// zRange is the actual min/max CONTENT height and is a much narrower
	// band, so decoding with it flattens the world toward its midpoint.
	float2 HeightRange;
	// Worldspace south/north Y bounds for the latitude term.
	float2 WorldYRange;

	float SnowLineZ;
	float SnowNorthDrop;
	float SnowLineFade;
	// Snow depth (world units) a fully covered heightmap texel carries.
	float SnowDepthUnits;

	// World XY of the tile block's SW corner and one tile's world span
	// (32 cells). Tiles are loaded ignoring sRGB, so classification runs on
	// the stored gamma-space values.
	float2 LODTileBase;
	float LODTileSpan;
	// Shifts the luminance threshold: 0 = only bright white counts as snow,
	// 1 = pale gray already counts.
	float LODSnowSensitivity;

	float4 LODTileValid;
}

// Snow classification of a baked LOD diffuse texel: bright and desaturated.
// LOD bakes fold every landscape texture down, so snow arrives as near-white
// with a slight blue cast while rock/dirt/grass stay darker or saturated.
// Must match ClassifyLODSnow in SnowDeformation.hlsli (the horizon recolor).
float ClassifyLODSnow(float3 color)
{
	float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
	float saturation = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
	float lumLo = 0.62 - 0.64 * saturate(LODSnowSensitivity);
	return smoothstep(lumLo, lumLo + 0.12, luminance) * (1.0 - smoothstep(0.10, 0.22, saturation));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= WindowDim || id.y >= WindowDim)
		return;
	// Baked cell data always wins; only sentinel texels are filled.
	if (TerrainWindow[id.xy].x > -50000.0)
		return;

	float2 worldXY = WindowOriginWorld + float2(id.xy) * TexelSize;
	float2 uv = worldXY * HeightMapScale + HeightMapOffset;
	// Outside the worldspace: stay sentinel (the shell edge-fades there).
	if (any(uv < 0.0) || any(uv > 1.0))
		return;

	float z = lerp(HeightRange.x, HeightRange.y, HeightMap.SampleLevel(LinearSampler, uv, 0));

	// Coverage from the LOD terrain diffuse when this texel's tile exists.
	// LOD tiles are top-down north-up images: v runs north->south, so flip.
	float2 tileLocal = (worldXY - LODTileBase) / LODTileSpan;
	int2 tileIdx = clamp((int2)floor(tileLocal), 0, 1);
	uint tileFlat = uint(tileIdx.y) * 2u + uint(tileIdx.x);
	float coverage;
	float provenance;
	[branch] if (LODTileValid[tileFlat] > 0.5)
	{
		float2 tileUV = saturate(tileLocal - (float2)tileIdx);
		tileUV.y = 1.0 - tileUV.y;
		float3 lodColor;
		[branch] if (tileFlat == 0u)
			lodColor = LODTile00.SampleLevel(LinearSampler, tileUV, 0).rgb;
		else if (tileFlat == 1u)
			lodColor = LODTile10.SampleLevel(LinearSampler, tileUV, 0).rgb;
		else if (tileFlat == 2u)
			lodColor = LODTile01.SampleLevel(LinearSampler, tileUV, 0).rgb;
		else lodColor = LODTile11.SampleLevel(LinearSampler, tileUV, 0).rgb;
		coverage = ClassifyLODSnow(lodColor);
		provenance = 2.0 + coverage;
	}
	else
	{
		// Snow-line fallback: sinks toward the north edge over the top
		// ~third of the map, so the northern coast reads snowy at sea level
		// while the mid-map plains stay bare at the same elevation.
		float northness = saturate((worldXY.y - WorldYRange.x) / max(WorldYRange.y - WorldYRange.x, 1.0));
		float snowLine = SnowLineZ - SnowNorthDrop * smoothstep(0.55, 0.9, northness);
		coverage = smoothstep(snowLine - SnowLineFade, snowLine + SnowLineFade, z);
		provenance = 1.0;
	}

	TerrainWindow[id.xy] = float4(z, coverage * SnowDepthUnits, coverage, provenance);
}
