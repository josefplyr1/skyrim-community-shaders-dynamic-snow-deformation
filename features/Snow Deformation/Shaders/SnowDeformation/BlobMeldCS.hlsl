// Screen-space snow shell, passes 2-4: from the seed field (x = |view z|,
// y = thickness; HeightMapProcessCS BlobSeedCS) roll a ball of each seed's
// own thickness toward the camera - the interior lifts by the thickness,
// silhouettes grow a rounded lip, gaps narrower than the balls bridge - then
// smooth, then keep only pixels that float in front of the scene. The
// composite (SnowStaticsShell MELD) shades the result once per pixel.
// Depth blur as the meld was tried and rejected: it flattened the shape.

// Mirror of SnowDeformation.h MeldCB and SnowStaticsShell.hlsl MeldCB.
cbuffer MeldCB : register(b0)
{
	row_major float4x4 MeldProj;
	row_major float4x4 MeldProjInverse;
	row_major float4x4 MeldViewInverse;
	float2 MeldDims;  // dynamic-resolution pixel dims
	float2 MeldDir;   // (1,0) or (0,1)
	float MeldDepthRange;   // dilation: a surface further along the view is another surface
	float MeldSmoothRange;  // smoothing: bilateral depth range
	float MeldMaxRadiusPx;  // pixel cap on every kernel
	float MeldDebug;
	float MeldSmoothing;  // smoothing radius, world units (0 = off)
	float MeldAnchor;     // unused (kept for layout)
	float MeldFootBias;   // sheet must float this far in front of the scene
	float MeldSeed;       // 1 on the first dilation (scene anchors are read)
	float MeldVerticalRange;  // surfaces further apart in world height do not meld (0 = off)
	float padMeld1;
	float padMeld2;
	float padMeld3;
}

Texture2D<float2> InField : register(t0);
Texture2D<float> SceneDepth : register(t1);
RWTexture2D<float2> OutField : register(u0);

static const float kMeldEmpty = 1e30;

// World size of one pixel at depth z, along the current pass direction.
float MeldPixelWorld(float z)
{
	const float scale = MeldDir.x > 0.5 ? (MeldProj[0][0] * MeldDims.x) : (MeldProj[1][1] * MeldDims.y);
	return z * 2.0 / max(abs(scale), 1e-4);
}

// The scene's |view z| at a pixel, through the same inverse projection the
// composite uses; sky and the far plane read as empty.
float MeldSceneZ(int2 p)
{
	const float d = SceneDepth.Load(int3(p, 0));
	const float2 ndc = float2((float(p.x) + 0.5) / MeldDims.x * 2.0 - 1.0, 1.0 - (float(p.y) + 0.5) / MeldDims.y * 2.0);
	const float4 v = mul(MeldProjInverse, float4(ndc, d, 1.0));
	const float z = abs(v.z / max(abs(v.w), 1e-8));
	return (z > 1e7 || isnan(z)) ? kMeldEmpty : z;
}

// World height (camera-relative) of a pixel at depth z, for the vertical gate.
float MeldWorldZ(int2 p, float z)
{
	const float2 ndc = float2((float(p.x) + 0.5) / MeldDims.x * 2.0 - 1.0, 1.0 - (float(p.y) + 0.5) / MeldDims.y * 2.0);
	float4 v = mul(MeldProjInverse, float4(ndc, 1.0, 1.0));
	v.xyz /= v.w;
	const float3 viewPos = v.xyz * (z / max(abs(v.z), 1e-5));
	return mul(MeldViewInverse, float4(viewPos, 1.0)).z;
}

// Dilation with each seed's own radius (its thickness). Separable: the
// horizontal pass carries the winning radius in y for the vertical pass.
[numthreads(8, 8, 1)] void MeldDilateCS(uint3 dtid
										: SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const int2 dir = int2(MeldDir);
	const int cap = (int)MeldMaxRadiusPx;
	// The centre's reference depth for the gates: its own seed, else (first
	// pass, anchored) the scene behind it.
	const float2 cf = InField[p];
	const float zc = cf.x;
	const bool hasC = zc < 1e29;
	const bool vgate = MeldVerticalRange > 0.0 && hasC;
	const float wzc = vgate ? MeldWorldZ(p, zc) : 0.0;
	float best = kMeldEmpty;
	float bestR = 0.0;
	float bestZ = 0.0;
	for (int i = -cap; i <= cap; i++)
	{
		const int2 q = clamp(p + dir * i, int2(0, 0), dims - 1);
		const float2 f = InField[q];
		const float zq = f.x;
		const float rq = f.y;
		if (zq > 1e29 || rq <= 0.0)
			continue;
		// One-directional depth gate: a surface BEHIND this one by more than
		// the range is another surface and may not push it; a nearer one is
		// in front and occludes, as it should.
		if (hasC && i != 0 && (zq - zc) > MeldDepthRange)
			continue;
		if (vgate && i != 0 && abs(MeldWorldZ(q, zq) - wzc) > MeldVerticalRange)
			continue;
		const float s = abs(float(i)) * MeldPixelWorld(zq);
		if (s >= rq)
			continue;
		const float cand = zq - sqrt(rq * rq - s * s);
		[flatten] if (cand < best)
		{
			best = cand;
			bestR = rq;
			bestZ = zq;
		}
	}
	// Lip hug: where a lip reaches a pixel with no seed of its own and the
	// visible surface there sits at or behind the lip's apex but within the
	// range, the lip follows that surface just in front of it, so the sheet
	// runs onto the landscape shell or a plank face instead of stopping in
	// mid-air above it. A surface in FRONT of the apex is an occluder and is
	// left alone.
	[branch] if (!hasC && best < 1e29)
	{
		const float sz = MeldSceneZ(p);
		[flatten] if (sz < 1e29 && sz >= bestZ - bestR && (sz - bestZ) < MeldDepthRange && best > sz - 2.0 * MeldFootBias)
			best = sz - 2.0 * MeldFootBias;
	}
	OutField[p] = float2(best, bestR);
}

// Optional bilateral smoothing of the field (x only; y carried).
[numthreads(8, 8, 1)] void MeldSmoothCS(uint3 dtid
										: SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	if (c.x > 1e29)
	{
		OutField[p] = c;
		return;
	}
	const float rpx = clamp(MeldSmoothing / max(MeldPixelWorld(c.x), 1e-4), 1.0, MeldMaxRadiusPx);
	const int r = (int)ceil(rpx);
	const float invSigma2 = 1.0 / (2.0 * (rpx * 0.5) * (rpx * 0.5));
	const float invRange2 = 1.0 / max(MeldSmoothRange * MeldSmoothRange, 1e-3);
	const int2 dir = int2(MeldDir);
	float sum = c.x;
	float wsum = 1.0;
	for (int i = -r; i <= r; i++)
	{
		if (i == 0)
			continue;
		const int2 q = clamp(p + dir * i, int2(0, 0), dims - 1);
		const float d = InField[q].x;
		if (d > 1e29)
			continue;
		const float dz = d - c.x;
		const float w = exp(-float(i * i) * invSigma2) * exp(-dz * dz * invRange2);
		sum += d * w;
		wsum += w;
	}
	OutField[p] = float2(sum / wsum, c.y);
}

// Final: y = 1 only where the field floats in front of the scene.
[numthreads(8, 8, 1)] void MeldSheetCS(uint3 dtid
									   : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	float sheet = 0.0;
	[branch] if (c.x < 1e29 && c.y > 0.0)
	{
		const float sz = MeldSceneZ(p);
		sheet = (c.x < sz - MeldFootBias) ? 1.0 : 0.0;
	}
	OutField[p] = float2(c.x, sheet);
}
