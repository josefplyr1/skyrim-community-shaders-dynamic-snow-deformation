// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Copies the deformation map's DEPTH channel into a single-channel texture so
// the menu can show it honestly.
//
// The map is RGBA16F and its .w was claimed by the bow wave's deposit field.
// ImGui blends by the texture's alpha, so drawing the map directly renders it
// through deposit: transparent wherever nothing has been pushed, and blank
// entirely right after a load, whatever the depth channel holds. Three rounds
// of a bug hunt were read backwards from that image.
//
// R8_UNORM samples as (depth, 0, 0, 1) - alpha is 1 by construction - so the
// copy cannot lie about what is there.

Texture2D<float4> DeformationMap : register(t0);
RWTexture2D<float> DebugDepth : register(u0);

// Leading rows of DeformationUpdateCS's PerFrame; only MapOrigin is read,
// but the prefix layout must match it exactly.
cbuffer PerFrame : register(b0)
{
	float2 WindowOrigin;
	int2 MapOrigin;
}

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	// Displaced depth, matching what the store keeps and what the shells carve
	// from: melted ground leaves no spoil and is not a trench.
	// De-rotated: the map is toroidal, the debug view stays world-aligned.
	uint2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	const uint2 phys = (dtid.xy + uint2(MapOrigin)) & (dims - 1);
	const float4 texel = DeformationMap[phys];
	DebugDepth[dtid.xy] = saturate(texel.x - max(texel.y, 0.0));
}
