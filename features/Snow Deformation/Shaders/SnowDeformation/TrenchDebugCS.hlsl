// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

Texture2D<float4> DeformationMap : register(t0);
RWTexture2D<float> DebugDepth : register(u0);

cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	int2 MapOrigin;
}

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	uint2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	const uint2 phys = (dtid.xy + uint2(MapOrigin)) & (dims - 1);
	const float4 texel = DeformationMap[phys];
	DebugDepth[dtid.xy] = saturate(texel.x - max(texel.y, 0.0));
}
