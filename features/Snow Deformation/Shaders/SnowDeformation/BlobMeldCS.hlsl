// Blob Snow Shell - screen-space meld, pass 2 of 3: separable bilateral blur
// of the nearest-sphere depth (|view z|, 1e30 = no sphere). The kernel is a
// world-space radius projected to pixels, so the meld looks the same at any
// distance; the depth-range weight keeps a foreground sphere from bleeding
// into a background one. Pass 1 (sphere depth) and pass 3 (composite) live
// in SnowStaticsShell.hlsl (BLOB_DEPTH / MELD).

// Mirror of SnowDeformation.h MeldCB and SnowStaticsShell.hlsl MeldCB.
cbuffer MeldCB : register(b0)
{
	row_major float4x4 MeldProj;
	row_major float4x4 MeldProjInverse;
	row_major float4x4 MeldViewInverse;
	float2 MeldDims;  // dynamic-resolution pixel dims
	float2 MeldDir;   // (1,0) or (0,1)
	float MeldRadiusWorld;
	float MeldDepthRange;
	float MeldMaxRadiusPx;
	float MeldDebug;
}

Texture2D<float> InDepth : register(t0);
RWTexture2D<float> OutDepth : register(u0);

[numthreads(8, 8, 1)] void MeldBlurCS(uint3 dtid
									  : SV_DispatchThreadID)
{
	const int2 dims = int2(MeldDims);
	if (dtid.x >= (uint)dims.x || dtid.y >= (uint)dims.y)
		return;
	const int2 p = int2(dtid.xy);
	const float c = InDepth[p];
	// Empty stays empty: the blur rounds the union, it never grows it.
	if (c > 1e29)
	{
		OutDepth[p] = c;
		return;
	}
	// World radius -> pixels at this depth (projection y scale, half height).
	const float rpx = clamp(MeldRadiusWorld * MeldProj[1][1] * MeldDims.y * 0.5 / max(c, 1.0), 1.0, MeldMaxRadiusPx);
	const int r = (int)ceil(rpx);
	const float invSigma2 = 1.0 / (2.0 * (rpx * 0.5) * (rpx * 0.5));
	const float invRange2 = 1.0 / max(MeldDepthRange * MeldDepthRange, 1e-3);
	const int2 dir = int2(MeldDir);
	float sum = c;
	float wsum = 1.0;
	for (int i = -r; i <= r; i++)
	{
		if (i == 0)
			continue;
		const int2 q = clamp(p + dir * i, int2(0, 0), dims - 1);
		const float d = InDepth[q];
		if (d > 1e29)
			continue;
		const float dz = d - c;
		const float w = exp(-float(i * i) * invSigma2) * exp(-dz * dz * invRange2);
		sum += d * w;
		wsum += w;
	}
	OutDepth[p] = sum / wsum;
}
