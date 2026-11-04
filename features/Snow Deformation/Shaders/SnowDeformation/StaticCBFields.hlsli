// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;
	float2 HeightWindowCenter;
	float HeightHalfExtent;

	float HasSmoothedNormals;
	float RoundedDepth;
	float VertexCountF;
	float HasObjectTop;

	float DebugSkinId;
	float LegacySkin;
	float padMoundSteepness;
	float AlphaTested;

	float FullCoat;
	float FadeExempt;
	float RoadField;
	float ProjThreshold;
	float ProjNoiseWeight;
	float ParallaxFollow;
	float ClassOverride;
	float ProjNoiseScale;
	float SteepThin;
	float ProjNoiseTiling;
	float ProjPixelEnable;
	float padSkinNormalCopy;
	float padShellMinNz;
	float PeelTol;
	float SkinSssSeen;
	float padMeldSk;

	float FarWindowCenterX;
	float FarWindowCenterY;
	float FarHalfExtent;
	float FarRoadDepth;
	float DecalMode;

	float HasSkinMasksCopy;
	float HasPreDecalMasks;
	float EdgeFlankWidth;
	float EdgeCoat;
	float FineHalfExtent;
	float LODBatch;
	float TexturedSnow;
