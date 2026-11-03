// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

Texture2D<float> MainDepth : register(t0);
Texture2D<float> ShellRasterDepth : register(t1);
Texture2D<float> PreSnowDepth : register(t2);
RWTexture2D<float> BlendedDepth : register(u0);
RWTexture2D<float> BlendedDepth16 : register(u1);

cbuffer DepthSyncCB : register(b2)
{
	uint2 SyncOrigin;
	uint2 SyncExtent;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;

	float src = MainDepth[p];
	if (ShellRasterDepth[p] <= 0.0 && src >= PreSnowDepth[p])
		return;
	float blended = min(BlendedDepth[p], src);
	BlendedDepth[p] = blended;
	BlendedDepth16[p] = blended;
}

RWTexture2D<float> PreSnowDepthOut : register(u2);

[numthreads(8, 8, 1)] void PreSnowCopyCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;
	PreSnowDepthOut[p] = MainDepth[p];
}

RWTexture2D<float> SavedDepth : register(u1);

[numthreads(8, 8, 1)] void SssSyncCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	ShellRasterDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;
	const float raster = ShellRasterDepth[p];
	if (raster == 0.0)
		return;
	const float blended = BlendedDepth[p];
	SavedDepth[p] = blended;
	BlendedDepth[p] = min(blended, abs(raster));
}

[numthreads(8, 8, 1)] void SssRestoreCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	ShellRasterDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;
	if (ShellRasterDepth[p] != 0.0)
		BlendedDepth[p] = SavedDepth[p];
}

bool SkinSssPixel(uint2 p, out float a_depth)
{
	a_depth = MainDepth[p];
	return ShellRasterDepth[p] > 0.0 || a_depth < PreSnowDepth[p];
}

[numthreads(8, 8, 1)] void SkinSssSyncCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;
	float depth;
	if (!SkinSssPixel(p, depth))
		return;
	const float blended = BlendedDepth[p];
	SavedDepth[p] = blended;
	BlendedDepth[p] = min(blended, depth);
}

#ifdef SKIN_SSS_MARK
Texture2D<float> FirstMarchDepth : register(t3);
RWTexture2D<uint> ChangedTiles : register(u2);
groupshared uint gDepthChanged;

[numthreads(8, 8, 1)] void SkinSssSyncMarkCS(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	if (gi == 0)
		gDepthChanged = 0;
	GroupMemoryBarrierWithGroupSync();
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	[branch] if (all(dtid.xy < SyncExtent) && all(p < dims))
	{
		float blended = BlendedDepth[p];
		float depth;
		[branch] if (SkinSssPixel(p, depth))
		{
			SavedDepth[p] = blended;
			blended = min(blended, depth);
			BlendedDepth[p] = blended;
		}
		if (blended != FirstMarchDepth[p])
			InterlockedOr(gDepthChanged, 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	[branch] if (gDepthChanged != 0 && gi < 16)
	{
		uint2 tiles;
		ChangedTiles.GetDimensions(tiles.x, tiles.y);
		const int2 t = int2((gid.xy * 8 + SyncOrigin) >> 3) - 1 + int2(gi & 3, gi >> 2);
		if (all(t >= 0) && all(t < int2(tiles)))
			ChangedTiles[uint2(t)] = 1u;
	}
}
#endif

[numthreads(8, 8, 1)] void SkinSssRestoreCS(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	const uint2 p = dtid.xy + SyncOrigin;
	if (any(dtid.xy >= SyncExtent) || any(p >= dims))
		return;
	float depth;
	if (SkinSssPixel(p, depth))
		BlendedDepth[p] = SavedDepth[p];
}

Texture2D<float> FillSource : register(t9);

float4 FillVS(uint vertexID : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint FootprintKey(float f)
{
	const uint u = asuint(f);
	return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

float FootprintValue(uint k)
{
	return asfloat((k & 0x80000000u) ? (k & 0x7FFFFFFFu) : ~k);
}

ByteAddressBuffer FillFootprint : register(t62);

float4 FillRectVS(uint vertexID : SV_VertexID) : SV_Position
{
	static const uint2 kCorner[6] = { uint2(0, 1), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 0) };
	const uint4 k = FillFootprint.Load4(0);
	if (k.z == 0)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float2 lo = float2(FootprintValue(~k.x), FootprintValue(~k.y));
	const float2 hi = float2(FootprintValue(k.z), FootprintValue(k.w));
	const uint2 c = kCorner[vertexID];
	return float4(c.x ? hi.x : lo.x, c.y ? hi.y : lo.y, 0.0, 1.0);
}

#ifdef FILL_PRE_SNOW
struct FillPreSnowOut
{
	float depth : SV_Depth;
	float preSnow : SV_Target0;
};

FillPreSnowOut FillPS(float4 pos : SV_Position)
{
	FillPreSnowOut o;
	o.depth = FillSource.Load(int3(pos.xy, 0));
	o.preSnow = o.depth;
	return o;
}
#elif defined(FILL_CLAIM)
float FillPS(float4 pos : SV_Position) : SV_Depth
{
	const float d = FillSource.Load(int3(pos.xy, 0));
	if (d <= 0.0)
		discard;
	return d;
}
#else
float FillPS(float4 pos : SV_Position) : SV_Depth
{
	return FillSource.Load(int3(pos.xy, 0));
}
#endif

cbuffer SkinCullCB : register(b0)
{
	row_major float4x4 CullViewProj;
	float4 CullCameraPosAdjust;
	float4 CullViewport;
	float4 CullDepth;
	uint CullSkinCount;
	float CullLevel;
	float2 cullPad;
}

Texture2D<float> HiZSource : register(t1);
RWTexture2D<float> HiZDest : register(u2);
RWTexture2D<float> HiZPrev : register(u6);

float HiZTap(uint2 p)
{
#ifdef HIZ_FROM_UAV
	return HiZPrev[p];
#else
	float d = HiZSource[p];
	[branch] if (CullLevel < 0.5)
	{
		float2 pf = float2(p);
		if (any(pf < CullViewport.xy) || any(pf >= CullViewport.xy + CullViewport.zw))
			d = 0.0;
	}
	return d;
#endif
}

[numthreads(8, 8, 1)] void HiZBuildCS(uint3 id : SV_DispatchThreadID)
{
	uint2 dstDims;
	HiZDest.GetDimensions(dstDims.x, dstDims.y);
	if (any(id.xy >= dstDims))
		return;
	uint2 srcDims;
#ifdef HIZ_FROM_UAV
	HiZPrev.GetDimensions(srcDims.x, srcDims.y);
#else
	HiZSource.GetDimensions(srcDims.x, srcDims.y);
#endif
	uint2 p0 = id.xy * 2;
	uint2 p1 = min(p0 + 1, srcDims - 1);
	uint2 p2 = min(p0 + 2, srcDims - 1);
	bool2 tail = (id.xy == dstDims - 1) & ((srcDims & 1) != 0);
	float d = max(max(HiZTap(uint2(p0.x, p0.y)), HiZTap(uint2(p1.x, p0.y))),
		max(HiZTap(uint2(p0.x, p1.y)), HiZTap(uint2(p1.x, p1.y))));
	if (tail.x)
		d = max(d, max(HiZTap(uint2(p2.x, p0.y)), HiZTap(uint2(p2.x, p1.y))));
	if (tail.y)
		d = max(d, max(HiZTap(uint2(p0.x, p2.y)), HiZTap(uint2(p1.x, p2.y))));
	if (tail.x && tail.y)
		d = max(d, HiZTap(uint2(p2.x, p2.y)));
	HiZDest[id.xy] = d;
}

struct SkinBound
{
	float3 Center;
	float Radius;
	uint IndexCount;
	uint BoundsSlot;
	uint HasBounds;
	float LiftMargin;
	uint ClusterOffset;
	uint ClusterCount;
	uint IndexPoolOffset;
	uint ScratchBase;
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
	float FadeEnd;
	float3 pad;
};
StructuredBuffer<SkinBound> SkinBounds : register(t2);
Texture2D<float> HiZ : register(t3);
StructuredBuffer<float4> MeshBounds : register(t4);
RWByteAddressBuffer SkinArgs : register(u3);
RWByteAddressBuffer SkinFootprint : register(u7);

uint4 SkinFootprintKeys(float2 lo, float2 hi)
{
	const float2 pad = 2.0 / CullViewport.zw;
	lo = clamp(lo - pad, -1.0, 1.0);
	hi = clamp(hi + pad, -1.0, 1.0);
	return uint4(~FootprintKey(lo.x), ~FootprintKey(lo.y), FootprintKey(hi.x), FootprintKey(hi.y));
}

void AddFootprint(uint4 keys)
{
	SkinFootprint.InterlockedMax(0, keys.x);
	SkinFootprint.InterlockedMax(4, keys.y);
	SkinFootprint.InterlockedMax(8, keys.z);
	SkinFootprint.InterlockedMax(12, keys.w);
}

bool BoxPastFadeEnd(float3 bmin, float3 bmax, float4 r0, float4 r1, float fadeEnd)
{
	float3 c = (bmin + bmax) * 0.5;
	float3 e = (bmax - bmin) * 0.5;
	float2 wc = float2(dot(r0.xyz, c) + r0.w, dot(r1.xyz, c) + r1.w);
	float2 we = float2(dot(abs(r0.xyz), e), dot(abs(r1.xyz), e));
	return fadeEnd > 0.0 && length(max(abs(wc - CullCameraPosAdjust.xy) - we, 0.0)) > fadeEnd;
}
float4 LocalBoxCornerClip(uint i, float3 bmin, float3 bmax, float4 r0, float4 r1, float4 r2)
{
	float3 l = float3((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
	float3 wp = float3(dot(r0.xyz, l) + r0.w, dot(r1.xyz, l) + r1.w, dot(r2.xyz, l) + r2.w);
	return mul(CullViewProj, float4(wp - CullCameraPosAdjust.xyz, 1.0));
}

bool ProjectLocalBox(float3 bmin, float3 bmax, float4 r0, float4 r1, float4 r2,
	out float2 lo, out float2 hi, out float zn)
{
	lo = 1e9;
	hi = -1e9;
	zn = 1e9;
	bool behind = false;
	[unroll] for (uint i = 0; i < 8; i++)
	{
		float4 clip = LocalBoxCornerClip(i, bmin, bmax, r0, r1, r2);
		if (clip.w <= 1e-3)
			behind = true;
		else
		{
			float2 n = clip.xy / clip.w;
			lo = min(lo, n);
			hi = max(hi, n);
			zn = min(zn, clip.z / clip.w);
		}
	}
	return !behind;
}

bool ProjectLocalBoxNear(float3 bmin, float3 bmax, float4 r0, float4 r1, float4 r2, out float2 lo, out float2 hi)
{
	float4 clip[8];
	[unroll] for (uint i = 0; i < 8; i++)
		clip[i] = LocalBoxCornerClip(i, bmin, bmax, r0, r1, r2);
	lo = 1e9;
	hi = -1e9;
	bool front = false;
	[unroll] for (uint c = 0; c < 8; c++)
	{
		[flatten] if (clip[c].z >= 0.0)
		{
			lo = min(lo, clip[c].xy / clip[c].w);
			hi = max(hi, clip[c].xy / clip[c].w);
			front = true;
		}
		[unroll] for (uint axis = 1; axis < 8; axis <<= 1)
		{
			const uint d = c | axis;
			[flatten] if (d != c && (clip[c].z >= 0.0) != (clip[d].z >= 0.0))
			{
				const float4 p = lerp(clip[c], clip[d], clip[c].z / (clip[c].z - clip[d].z));
				lo = min(lo, p.xy / p.w);
				hi = max(hi, p.xy / p.w);
			}
		}
	}
	return front;
}

uint TestProjectedBox(float2 lo, float2 hi, float zn)
{
	if (hi.x < -1.0 || lo.x > 1.0 || hi.y < -1.0 || lo.y > 1.0)
		return 3;
	if (zn > 1.0)
		return 4;
	float depthNear = CullDepth.x + saturate(zn) * (CullDepth.y - CullDepth.x) - CullDepth.z;
	float2 vpMin = CullViewport.xy;
	float2 vpMax = CullViewport.xy + CullViewport.zw - 1.0;
	float2 pxMin = clamp(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * CullViewport.zw + CullViewport.xy - 1.0, vpMin, vpMax);
	float2 pxMax = clamp(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * CullViewport.zw + CullViewport.xy + 1.0, vpMin, vpMax);
	float span = max(max(pxMax.x - pxMin.x, pxMax.y - pxMin.y), 1.0);
	uint level = (uint)clamp(ceil(log2(span)) - 1.0, 0.0, CullDepth.w - 1.0);
	uint shift = level + 1;
	uint w, h, levels;
	HiZ.GetDimensions(level, w, h, levels);
	uint2 t0 = min(uint2(pxMin) >> shift, uint2(w, h) - 1);
	uint2 t1 = min(uint2(pxMax) >> shift, uint2(w, h) - 1);
	float occluder = max(max(HiZ.Load(int3(t0.x, t0.y, level)), HiZ.Load(int3(t1.x, t0.y, level))),
		max(HiZ.Load(int3(t0.x, t1.y, level)), HiZ.Load(int3(t1.x, t1.y, level))));
	if (occluder <= 0.0)
		return 2;
	return depthNear > occluder ? 5 : 0;
}

bool ProjectSphere(float3 center, float r, out float2 lo, out float2 hi, out float zn)
{
	lo = 1e9;
	hi = -1e9;
	zn = 0.0;
	float3 rel = center - CullCameraPosAdjust.xyz;
	float4 rowW = CullViewProj[3];
	float fLen = max(length(rowW.xyz), 1e-6);
	float3 fh = rowW.xyz / fLen;
	float wC = dot(rowW.xyz, rel) + rowW.w;
	float wNear = wC - r * fLen;
	if (wNear <= 1e-3)
		return false;
	float z = wC / fLen;
	[unroll] for (uint axis = 0; axis < 2; axis++)
	{
		float4 row = CullViewProj[axis];
		float uf = dot(row.xyz, fh);
		float3 up = row.xyz - uf * fh;
		float upLen = max(length(up), 1e-6);
		float a = (dot(up, rel) + row.w - uf * rowW.w / fLen) / upLen;
		float t = sqrt(max(a * a + z * z - r * r, 0.0));
		float k = upLen / fLen;
		float c = uf / fLen;
		float b0 = k * (a * t - z * r) / max(z * t + a * r, 1e-6) + c;
		float b1 = k * (a * t + z * r) / max(z * t - a * r, 1e-6) + c;
		lo[axis] = min(b0, b1);
		hi[axis] = max(b0, b1);
	}
	float4 rowZ = CullViewProj[2];
	zn = (dot(rowZ.xyz, rel - r * fh) + rowZ.w) / wNear;
	return true;
}

[numthreads(64, 1, 1)] void SkinCullCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= CullSkinCount)
		return;
	SkinBound b = SkinBounds[id.x];
	uint draw = 1;
	uint reason = 0;
	float2 lo = 1e9;
	float2 hi = -1e9;
	float zn = 0.0;
	bool bounded = false;
	bool boxed = false;
	bool pastRange = false;

	[branch] if (b.HasBounds != 0)
	{
		float3 bmin = MeshBounds[b.BoundsSlot * 2].xyz;
		float3 bmax = MeshBounds[b.BoundsSlot * 2 + 1].xyz;
		float grow = b.LiftMargin / max(length(b.WorldRow0.xyz), 1e-6);
		bmin -= grow;
		bmax += grow;
		pastRange = BoxPastFadeEnd(bmin, bmax, b.WorldRow0, b.WorldRow1, b.FadeEnd);
		if (ProjectLocalBox(bmin, bmax, b.WorldRow0, b.WorldRow1, b.WorldRow2, lo, hi, zn))
			bounded = boxed = true;
		else
			reason = length((bmax - bmin) * length(b.WorldRow0.xyz)) > 2000.0 ? 7 : 1;
	}
	else
	{
		pastRange = b.FadeEnd > 0.0 && length(b.Center.xy - CullCameraPosAdjust.xy) - b.Radius > b.FadeEnd;
		if (ProjectSphere(b.Center, b.Radius, lo, hi, zn))
			bounded = true;
		else
			reason = 6;
	}

	[branch] if (bounded)
	{
		uint verdict = TestProjectedBox(lo, hi, zn);
		[branch] if (verdict >= 3 && boxed)
		{
			float2 slo, shi;
			float szn;
			if (ProjectSphere(b.Center, b.Radius, slo, shi, szn))
			{
				uint sphereVerdict = TestProjectedBox(slo, shi, szn);
				if (sphereVerdict < 3)
					verdict = sphereVerdict;
			}
			else
				verdict = 6;
		}
		reason = verdict;
		if (verdict >= 3)
			draw = 0;
	}
	[flatten] if (draw != 0 && pastRange)
	{
		draw = 0;
		reason = 8;
	}
	[branch] if (draw != 0 && b.ClusterCount == 0)
	{
		float2 slo, shi;
		float szn;
		if (!ProjectSphere(b.Center, b.Radius, slo, shi, szn)) {
			slo = -1.0;
			shi = 1.0;
		}
		AddFootprint(SkinFootprintKeys(slo, shi));
	}
	uint base = id.x * 20;
	SkinArgs.Store(base, b.IndexCount);
	SkinArgs.Store(base + 4, draw);
	SkinArgs.Store(base + 8, 0);
	SkinArgs.Store(base + 12, 0);
	SkinArgs.Store(base + 16, reason);
}

cbuffer TerrainFineCB : register(b1)
{
	float2 FineOriginWorld;
	float2 WindowOriginWorld;
	uint FineDim;
	uint WindowDim;
	float TexelSize;
	float FineTexel;
}
Texture2D<float4> FineSrcWindow : register(t8);
RWTexture2D<float> FineDest : register(u5);

float FineCatmull(float p0, float p1, float p2, float p3, float t)
{
	return 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
}

[numthreads(8, 8, 1)] void TerrainFineCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= FineDim))
		return;
	float2 world = FineOriginWorld + float2(id.xy) * FineTexel;
	float2 uf = (world - WindowOriginWorld) / TexelSize;
	int2 i = (int2)floor(uf);
	float2 f = uf - float2(i);
	int2 lo = (i >> 5) << 5;
	int2 hi = lo + 32;
	float h[4][4];
	bool missing = false;
	[unroll] for (int j = 0; j < 4; j++)
	{
		[unroll] for (int k = 0; k < 4; k++)
		{
			int2 sIdx = i + int2(k - 1, j - 1);
			int2 c = clamp(sIdx, lo, hi);
			int2 m = 2 * c - sIdx;
			int2 wm = int2(WindowDim - 1, WindowDim - 1);
			float he = FineSrcWindow.Load(int3(clamp(c, int2(0, 0), wm), 0)).x;
			float hm = FineSrcWindow.Load(int3(clamp(m, int2(0, 0), wm), 0)).x;
			if (he < -50000.0 || hm < -50000.0)
				missing = true;
			h[j][k] = any(sIdx != c) ? 2.0 * he - hm : he;
		}
	}
	float outH = -100000.0;
	[branch] if (!missing)
	{
		float rows[4];
		[unroll] for (int r = 0; r < 4; r++)
			rows[r] = FineCatmull(h[r][0], h[r][1], h[r][2], h[r][3], f.x);
		outH = FineCatmull(rows[0], rows[1], rows[2], rows[3], f.y);
	}
	FineDest[id.xy] = outH;
}


#ifdef SNOW_CLUSTER_CULL
StructuredBuffer<float4> ClusterBounds : register(t5);
ByteAddressBuffer ClusterIndexPool : register(t6);
RWByteAddressBuffer ClusterScratchIndices : register(u4);

#define SNOW_CLUSTER_GROUP 256
groupshared uint gClusterScan[SNOW_CLUSTER_GROUP];
groupshared uint gClusterFootprint[4];

[numthreads(SNOW_CLUSTER_GROUP, 1, 1)] void ClusterCullCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
	if (gtid.x < 4)
		gClusterFootprint[gtid.x] = 0;
	GroupMemoryBarrierWithGroupSync();
	uint skin = gid.x;
	SkinBound b = SkinBounds[min(skin, max(CullSkinCount, 1u) - 1u)];
	uint argBase = skin * 20;
	bool active = skin < CullSkinCount && b.ClusterCount != 0 && SkinArgs.Load(argBase + 4) != 0;

	uint idxStart = 0;
	uint idxCount = 0;
	uint keep = 0;
	[branch] if (active && gtid.x < b.ClusterCount)
	{
		float4 cmin = ClusterBounds[(b.ClusterOffset + gtid.x) * 2];
		float4 cmax = ClusterBounds[(b.ClusterOffset + gtid.x) * 2 + 1];
		idxStart = asuint(cmin.w);
		idxCount = asuint(cmax.w);
		float grow = b.LiftMargin / max(length(b.WorldRow0.xyz), 1e-6);
		float2 lo = 0.0, hi = 0.0;
		float zn;
		bool drawsOnScreen = true;
		if (BoxPastFadeEnd(cmin.xyz - grow, cmax.xyz + grow, b.WorldRow0, b.WorldRow1, b.FadeEnd))
			keep = 0;
		else if (!ProjectLocalBox(cmin.xyz - grow, cmax.xyz + grow, b.WorldRow0, b.WorldRow1, b.WorldRow2, lo, hi, zn)) {
			keep = idxCount;
			drawsOnScreen = ProjectLocalBoxNear(cmin.xyz - grow, cmax.xyz + grow, b.WorldRow0, b.WorldRow1, b.WorldRow2, lo, hi);
		} else
			keep = TestProjectedBox(lo, hi, zn) >= 3 ? 0 : idxCount;
		[branch] if (keep != 0 && drawsOnScreen)
		{
			const uint4 keys = SkinFootprintKeys(lo, hi);
			InterlockedMax(gClusterFootprint[0], keys.x);
			InterlockedMax(gClusterFootprint[1], keys.y);
			InterlockedMax(gClusterFootprint[2], keys.z);
			InterlockedMax(gClusterFootprint[3], keys.w);
		}
	}

	gClusterScan[gtid.x] = keep;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint step = 1; step < SNOW_CLUSTER_GROUP; step <<= 1)
	{
		uint add = (gtid.x >= step) ? gClusterScan[gtid.x - step] : 0;
		GroupMemoryBarrierWithGroupSync();
		gClusterScan[gtid.x] += add;
		GroupMemoryBarrierWithGroupSync();
	}
	uint exclusive = gClusterScan[gtid.x] - keep;
	uint total = gClusterScan[SNOW_CLUSTER_GROUP - 1];

	[branch] if (keep != 0)
	{
		uint dst = b.ScratchBase + exclusive;
		for (uint k = 0; k < idxCount; k++)
		{
			uint si = b.IndexPoolOffset + idxStart + k;
			uint word = ClusterIndexPool.Load((si >> 1) << 2);
			uint v = (si & 1) ? (word >> 16) : (word & 0xFFFF);
			ClusterScratchIndices.Store((dst + k) << 2, v);
		}
	}

	if (gtid.x == 0 && active)
	{
		SkinArgs.Store(argBase, total);
		SkinArgs.Store(argBase + 4, total > 0 ? 1 : 0);
		SkinArgs.Store(argBase + 8, b.ScratchBase);
		if (gClusterFootprint[2] != 0)
			AddFootprint(uint4(gClusterFootprint[0], gClusterFootprint[1], gClusterFootprint[2], gClusterFootprint[3]));
	}
}
#endif
