// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// The snow set's albedo convention and the ice a track glazes by, shared by
// the shells and Lighting's snow recolors so every surface wears the same snow.

#ifndef __SNOW_COLOR_HLSLI__
#define __SNOW_COLOR_HLSLI__

#include "Common/SharedData.hlsli"
#include "Common/ColorManagement.hlsli"

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
namespace SnowColor
{
	// A snow set texel as the ground's own shader reads it: a PBR (linear) set
	// as TruePBR's albedo, a vanilla set as Lighting's diffuse.
	float3 SetAlbedo(float3 texel, bool isLinear)
	{
		return isLinear ? Color::LinearToPBRMaterial(ColorManagement::DiffuseToWorking(texel, true)) : ColorManagement::DiffuseToWorking(texel);
	}
}

// Wet Snow and Ice: a track glazes by the ice that froze after it was cut, the
// same on the shells and the land recolor.
namespace SnowIce
{
	// Mirror kIceHistoryBins and kAgeingClockStart in SnowDeformation.h.
	static const uint kHistoryBins = 128;
	static const float kClockWrap = 1024.0;

	// The climate rows' ice (xyz) when a track was cut at a_date (ageing clock):
	// each entry is the ice when its clock unit ended, so a date reads the end
	// of its own unit and a fresh cut reads now. Older than the history: its
	// oldest entry. The history is written before the frame's update, which
	// moves the dates back by kClockWrap when the clock wraps.
	float3 AtCut(float a_date)
	{
		const float newest = floor(SharedData::snowDeformationSettings.IceClock);
		float age = newest - a_date;
		age -= age > kClockWrap - float(kHistoryBins) ? kClockWrap : 0.0;
		const float t = newest - clamp(age, 0.0, float(kHistoryBins - 1));
		const float t0 = floor(t);
		const uint b0 = uint(t0) & (kHistoryBins - 1);
		const uint b1 = (b0 + 1) & (kHistoryBins - 1);
		return lerp(SharedData::snowDeformationSettings.IceHistory[b0].xyz, SharedData::snowDeformationSettings.IceHistory[b1].xyz, t - t0);
	}
}
#endif

#endif  // __SNOW_COLOR_HLSLI__
