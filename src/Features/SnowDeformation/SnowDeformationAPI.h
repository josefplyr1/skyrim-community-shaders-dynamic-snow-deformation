#pragma once
#include <stdint.h>

#define SNOWDEFORMATION_API_VERSION 2u

struct SnowDeformationAPI_V1
{
	uint32_t version;
	bool (*IsActive)();
	float (*GetSnowDepthAt)(float worldX, float worldY);
	float (*GetSnowDepthAtRef)(void* tesObjectREFR);
	float (*GetSnowAccumulation)();
};

struct SnowDeformationAPI_V2
{
	uint32_t version;
	bool (*IsActive)();
	float (*GetSnowDepthAt)(float worldX, float worldY);
	float (*GetSnowDepthAtRef)(void* tesObjectREFR);
	float (*GetSnowAccumulation)();
	void (*DepositBlood)(float worldX, float worldY, float worldZ, float radius, float r, float g, float b, float amount);
};

typedef const void* (*SnowDeformation_GetAPI_t)(uint32_t requestedVersion);
