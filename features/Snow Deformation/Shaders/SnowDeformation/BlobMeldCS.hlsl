// Blob Snow Shell - screen-space meld, the field passes between the sphere
// depth (pass 1, SnowStaticsShell BLOB_DEPTH) and the composite (SnowStaticsShell
// MELD). The field is float2: x = |view z| (1e30 = nothing), y = coverage.
//
// The meld is a morphological CLOSING with a ball of MeldRadiusWorld: dilate
// (every surface point sprouts a ball, nearest front wins) then erode (roll
// the ball back). Valleys narrower than the ball fill, every bump keeps its
// own curvature, nothing grows past its silhouette. A Gaussian blur of depth
// was tried first: it flattens the spheres' own curvature with the necks, the
// normals turn camera-facing, and the surface reads as a bright flat void.
// With MeldAnchor the scene depth seeds the field too, so the sheet runs down
// from a sphere onto the shell or the plank face beneath it; coverage keeps
// the composite within ball reach of a real sphere, and the sheet pass keeps
// only pixels where the closed surface actually floats in front of the scene.

// Mirror of SnowDeformation.h MeldCB and SnowStaticsShell.hlsl MeldCB.
cbuffer MeldCB : register(b0)
{
	row_major float4x4 MeldProj;
	row_major float4x4 MeldProjInverse;
	row_major float4x4 MeldViewInverse;
	float2 MeldDims;  // dynamic-resolution pixel dims
	float2 MeldDir;   // (1,0) or (0,1)
	float MeldRadiusWorld;  // closing ball radius, world units
	float MeldDepthRange;   // smoothing: bilateral depth range
	float MeldMaxRadiusPx;  // pixel cap on every kernel
	float MeldDebug;
	float MeldSmoothing;  // smoothing radius, world units (0 = off)
	float MeldAnchor;     // 1 = scene depth seeds the field
	float MeldFootBias;   // sheet must float this far in front of the scene
	float MeldSeed;       // 1 on the first dilation: seed coverage and anchors
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

[numthreads(8, 8, 1)] void MeldDilateCS(uint3 dtid
										: SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const int2 dir = int2(MeldDir);
	const float R = max(MeldRadiusWorld, 0.01);
	const int cap = (int)MeldMaxRadiusPx;
	float best = kMeldEmpty;
	float cov = 0.0;
	for (int i = -cap; i <= cap; i++)
	{
		const int2 q = clamp(p + dir * i, int2(0, 0), dims - 1);
		const float2 f = InField[q];
		float zq = f.x;
		float cq = f.y;
		[branch] if (MeldSeed > 0.5)
		{
			cq = zq < 1e29 ? 1.0 : 0.0;
			[flatten] if (zq > 1e29 && MeldAnchor > 0.5)
				zq = MeldSceneZ(q);
		}
		if (zq > 1e29)
			continue;
		const float s = abs(float(i)) * MeldPixelWorld(zq);
		if (s >= R)
			continue;
		best = min(best, zq - sqrt(R * R - s * s));
		cov = max(cov, cq);
	}
	OutField[p] = float2(best, cov);
}

[numthreads(8, 8, 1)] void MeldErodeCS(uint3 dtid
									   : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	if (c.x > 1e29)
	{
		OutField[p] = float2(kMeldEmpty, 0.0);
		return;
	}
	const int2 dir = int2(MeldDir);
	const float R = max(MeldRadiusWorld, 0.01);
	const float pw = MeldPixelWorld(c.x);
	const int r = min((int)MeldMaxRadiusPx, (int)ceil(R / max(pw, 1e-4)));
	float best = -kMeldEmpty;
	float cov = 1.0;
	bool hole = false;
	for (int i = -r; i <= r && !hole; i++)
	{
		const int2 q = clamp(p + dir * i, int2(0, 0), dims - 1);
		const float s = abs(float(i)) * pw;
		if (s >= R)
			continue;
		const float2 f = InField[q];
		hole = f.x > 1e29;
		best = max(best, f.x + sqrt(R * R - s * s));
		cov = min(cov, f.y);
	}
	OutField[p] = hole ? float2(kMeldEmpty, 0.0) : float2(best, cov);
}

// Optional small bilateral smoothing of the closed field (x only).
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
	const float invRange2 = 1.0 / max(MeldDepthRange * MeldDepthRange, 1e-3);
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

// Final: y = 1 only where the closed surface floats in front of the scene
// inside coverage. A sphere pixel passed the scene depth test in pass 1, so
// it qualifies; an anchor pixel the ball did not lift does not.
[numthreads(8, 8, 1)] void MeldSheetCS(uint3 dtid
									   : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	float sheet = 0.0;
	[branch] if (c.x < 1e29 && c.y > 0.5)
	{
		const float sz = MeldSceneZ(p);
		sheet = (c.x < sz - MeldFootBias) ? 1.0 : 0.0;
	}
	OutField[p] = float2(c.x, sheet);
}
