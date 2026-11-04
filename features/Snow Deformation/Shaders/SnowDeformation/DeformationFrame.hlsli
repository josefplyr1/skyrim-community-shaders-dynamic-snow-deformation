// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

	float2 WindowOrigin;
	int2 MapOrigin;

	float TexelSize;
	uint StampCount;
	float RefillAmount;
	uint ClearMap;

	float StampFalloffStart;
	float BermReach;
	float2 WindBias;

	float DeltaTime;
	float MeltPersistence;
	float MeltFloorStart;
	float MeltEdgeNoise;

	float CrustPrintDepth;
	float CrustThaw;
	float CrustBreakOnCarve;
	float SlumpRate;

	uint InjectValid;
	float GameDeltaTime;
	uint DebugActivityView;
	float AgeingClock;
