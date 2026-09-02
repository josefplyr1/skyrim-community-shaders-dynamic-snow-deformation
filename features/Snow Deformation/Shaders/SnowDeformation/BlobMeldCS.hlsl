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
	float MeldVerticalRange;  // surfaces further apart in world height do not meld (0 = off)
	float MeldFeather;        // sheet edge fillets onto a surface close behind over this many world units
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
	// The centre's own surface height, for the vertical gate: a surface much
	// higher or lower than this one may not push it (a rail over a plank).
	float2 cf = InField[p];
	float zc = cf.x;
	[flatten] if (MeldSeed > 0.5 && zc > 1e29 && MeldAnchor > 0.5)
		zc = MeldSceneZ(p);
	const bool gate = MeldVerticalRange > 0.0 && zc < 1e29;
	const float wzc = gate ? MeldWorldZ(p, zc) : 0.0;
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
		if (gate && i != 0 && abs(MeldWorldZ(q, zq) - wzc) > MeldVerticalRange)
			continue;
		// Depth gate: a surface further along the view than Meld Depth Range
		// is another surface; its ball neither pushes this one nor is pushed.
		if (zc < 1e29 && i != 0 && abs(zq - zc) > MeldDepthRange)
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
	const bool gate = MeldVerticalRange > 0.0;
	const float wzc = gate ? MeldWorldZ(p, c.x) : 0.0;
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
		if (hole)
			break;
		if (gate && i != 0 && abs(MeldWorldZ(q, f.x) - wzc) > MeldVerticalRange)
			continue;
		if (i != 0 && abs(f.x - c.x) > MeldDepthRange)
			continue;
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

// Feather, pass 1 of 2 (horizontal): for each sheet pixel, the distance in
// pixels to the nearest non-sheet pixel along the row (F + 1 = none within
// reach); non-sheet pixels carry -1.
[numthreads(8, 8, 1)] void MeldFeatherHCS(uint3 dtid
										  : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	if (c.y < 0.5 || c.x > 1e29)
	{
		OutField[p] = float2(c.x, -1.0);
		return;
	}
	const float pw = MeldPixelWorld(c.x);
	const int F = clamp((int)ceil(MeldFeather / max(pw, 1e-4)), 0, (int)MeldMaxRadiusPx);
	float d = float(F + 1);
	for (int i = 1; i <= F; i++)
	{
		const float a = InField[clamp(p + int2(i, 0), int2(0, 0), dims - 1)].y;
		const float b = InField[clamp(p - int2(i, 0), int2(0, 0), dims - 1)].y;
		if (a < 0.5 || b < 0.5)
		{
			d = float(i);
			break;
		}
	}
	OutField[p] = float2(c.x, d);
}

// Feather, pass 2 of 2 (vertical): Chebyshev distance to the sheet edge from
// the row distances, then the edge ramps down onto a surface close behind it
// (the landscape shell, the plank) so the two meet in a fillet instead of a
// step. Against a far background the edge stays where it is.
[numthreads(8, 8, 1)] void MeldFeatherVCS(uint3 dtid
										  : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float2 c = InField[p];
	if (c.y < 0.0 || c.x > 1e29)
	{
		OutField[p] = float2(kMeldEmpty, 0.0);
		return;
	}
	const float pw = MeldPixelWorld(c.x);
	const int F = clamp((int)ceil(MeldFeather / max(pw, 1e-4)), 0, (int)MeldMaxRadiusPx);
	float d = c.y;
	for (int j = 1; j <= F; j++)
	{
		const float u = InField[clamp(p + int2(0, j), int2(0, 0), dims - 1)].y;
		const float v = InField[clamp(p - int2(0, j), int2(0, 0), dims - 1)].y;
		const float du = u < 0.0 ? float(j) : max(float(j), u);
		const float dv = v < 0.0 ? float(j) : max(float(j), v);
		d = min(d, min(du, dv));
	}
	float z = c.x;
	float sheet = 1.0;
	[branch] if (F > 0 && d <= float(F))
	{
		const float sz = MeldSceneZ(p);
		[branch] if (sz < 1e29 && (sz - z) < MeldDepthRange)
		{
			const float t = smoothstep(0.0, 1.0, saturate((d - 0.5) / float(F)));
			z = lerp(sz, z, t);
			sheet = (z < sz - MeldFootBias) ? 1.0 : 0.0;
		}
	}
	OutField[p] = float2(z, sheet);
}
