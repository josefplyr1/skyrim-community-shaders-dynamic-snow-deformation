// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Undulation field bake. The dune field is a pure function of world XY and
// the Spacing slider, yet both shells evaluated it live - one eval per
// vertex/march tap and FOUR per pixel for the shading gradient. This bakes
// height and that gradient into a camera-snapped window once, rebaked only
// on recenter or a Spacing change; amplitude stays a live multiplier.
//
// x = UndulationNorm (amp-free height in [-1, 1])
// yz = its +-12-unit central-difference gradient - the SAME operator both
//      shells' pixel paths used live, so shading is reproduced, not the
//      (sharper) analytic gradient.

#include "SnowDeformation/SnowNoise.hlsli"

cbuffer UndulationFieldCB : register(b0)
{
	// World XY of texel (0,0)'s centre; texels are FieldTexel apart.
	float2 FieldOriginWorld;
	float FieldTexel;
	// UndulationScale (the Spacing slider), part of the bake.
	float FieldScale;
}

RWTexture2D<float4> UndulationField : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	float2 w = FieldOriginWorld + float2(id.xy) * FieldTexel;
	const float uStep = 12.0;
	float h = UndulationNorm(w, FieldScale);
	float2 g = float2(
					UndulationNorm(w + float2(uStep, 0.0), FieldScale) - UndulationNorm(w - float2(uStep, 0.0), FieldScale),
					UndulationNorm(w + float2(0.0, uStep), FieldScale) - UndulationNorm(w - float2(0.0, uStep), FieldScale)) /
	           (2.0 * uStep);
	UndulationField[id.xy] = float4(h, g, 0.0);
}
