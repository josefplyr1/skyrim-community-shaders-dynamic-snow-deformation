// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// DeferredCompositeCS.hlsl's seam shield. SSGI reads the snow shell's edge
// cliff as an occluder and prints a dark AO ring on the ground just beyond the
// snow border, which makes the shell read as hovering. The shell cannot shield
// those pixels itself (it discarded there), so the AO is lifted inside the
// shell's analytic contact fringe. The mask (SeamShieldCS) is the window's
// fringe band gated to real snow nearby: bare ground far from any border is
// not fringe.

#ifndef __SNOW_SEAM_SHIELD_HLSLI__
#define __SNOW_SEAM_SHIELD_HLSLI__

Texture2D<float> SnowSeamMask : register(t16);

cbuffer SnowSeamCB : register(b7)
{
	float2 SnowSeamWindowOffset;  // absolute worldXY + offset -> window texels
	float SnowSeamTexelSize;
	float SnowSeamDim;
	float4 SnowSeamParams;  // x = lift strength, w > 0.5 = active this frame
};

float SnowSeamShield(float2 worldXY)
{
	float2 t = clamp((worldXY + SnowSeamWindowOffset) / SnowSeamTexelSize, 0.0, SnowSeamDim - 1.001);
	int2 tBase = (int2)t;
	float2 f = t - float2(tBase);
	int2 tNext = min(tBase + 1, int2((int)SnowSeamDim - 1, (int)SnowSeamDim - 1));
	float m00 = SnowSeamMask.Load(int3(tBase.x, tBase.y, 0));
	float m10 = SnowSeamMask.Load(int3(tNext.x, tBase.y, 0));
	float m01 = SnowSeamMask.Load(int3(tBase.x, tNext.y, 0));
	float m11 = SnowSeamMask.Load(int3(tNext.x, tNext.y, 0));
	return lerp(lerp(m00, m10, f.x), lerp(m01, m11, f.x), f.y);
}

// Lifts SSGI AO inside the shell's contact fringe.
float SnowSeamLiftAO(float ssgiAo, float2 worldXYRel)
{
	[branch] if (SnowSeamParams.w > 0.5)
	{
		float2 seamWorldXY = worldXYRel + FrameBuffer::CameraPosAdjust.xy;
		ssgiAo = lerp(ssgiAo, 1.0, SnowSeamShield(seamWorldXY) * SnowSeamParams.x);
	}
	return ssgiAo;
}

#endif  // __SNOW_SEAM_SHIELD_HLSLI__
