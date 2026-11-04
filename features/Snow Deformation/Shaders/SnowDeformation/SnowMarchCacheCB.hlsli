// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

StructuredBuffer<uint> MarchTilesIn : register(t63);
cbuffer MarchCacheCB : register(b2)
{
	uint MarchRowStart;
	uint MarchRowCount;
	uint2 MarchCachePad;
#ifdef SNOW_MARCH_SUN_CS
	float4 MarchSun;
#endif
}
