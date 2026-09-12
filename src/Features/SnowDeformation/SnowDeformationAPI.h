#pragma once
// Community Shaders - Snow Deformation: public C API for SKSE plugins.
// Copy this header into your plugin. No dependency on Community Shaders'
// own headers or on CommonLib.
//
//   auto getApi = (SnowDeformation_GetAPI_t)GetProcAddress(
//       GetModuleHandleA("CommunityShaders.dll"), "SnowDeformation_GetAPI");
//   auto api = getApi ? (const SnowDeformationAPI_V1*)getApi(1) : nullptr;
//
// Resolve it after kPostPostLoad (every plugin is loaded by then). A null
// result means Community Shaders is absent, too old, or does not carry the
// feature. Every function is safe to call at any time and returns 0 / false
// while the feature is off or no land is baked.
#include <stdint.h>

#define SNOWDEFORMATION_API_VERSION 1u

struct SnowDeformationAPI_V1
{
	uint32_t version;  // SNOWDEFORMATION_API_VERSION of the struct returned
	// True when Snow Deformation is loaded and switched on.
	bool (*IsActive)();
	// Untrampled terrain snow depth at a world XY in world units, scaled by
	// the current accumulation. 0 on bare ground, unbaked land, or when off.
	// Existing trenches are not subtracted: an actor walking carves its own
	// trench, so this is the depth it wades through.
	float (*GetSnowDepthAt)(float worldX, float worldY);
	// GetSnowDepthAt under a RE::TESObjectREFR*, 0 when the reference stands
	// more than 70 units above the land (bridge, walkway, roof).
	float (*GetSnowDepthAtRef)(void* tesObjectREFR);
	// The accumulation scalar, 0..1 (0 when accumulation is off).
	float (*GetSnowAccumulation)();
};

typedef const void* (*SnowDeformation_GetAPI_t)(uint32_t requestedVersion);
