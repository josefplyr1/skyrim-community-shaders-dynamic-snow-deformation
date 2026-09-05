// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Syncs the snow shell's depth writes into Terrain Blending's blended depth
// copies. Screen-space passes that run after the shell (Screen Space GI in
// particular) consume the blended depth, finalized during opaque rendering;
// without this sync they still see buried geometry poking up through the
// snow and paint occlusion halos onto the shell surface.
//
// min() only takes effect where the shell drew closer than the pre-shell
// scene, so Terrain Blending's soft-blend depth values survive everywhere
// the shell is absent. The 16-bit copy is written from the 32-bit one's
// value (typed UAV loads are only guaranteed for R32).

Texture2D<float> MainDepth : register(t0);
RWTexture2D<float> BlendedDepth : register(u0);
RWTexture2D<float> BlendedDepth16 : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint2 dims;
	MainDepth.GetDimensions(dims.x, dims.y);
	if (any(dtid.xy >= dims))
		return;

	float src = MainDepth[dtid.xy];
	float blended = min(BlendedDepth[dtid.xy], src);
	BlendedDepth[dtid.xy] = blended;
	BlendedDepth16[dtid.xy] = blended;
}

// Shell prepass fill: turns the prepass's raster-depth colour target into
// the shading pass's private depth buffer (fullscreen triangle, depth
// ALWAYS + write). Pixels the prepass did not win hold the clear value 0,
// which no shell fragment can equal, so the shading pass's EQUAL test is a
// hardware early-Z that admits exactly the pixels the prepass wrote.
Texture2D<float> FillSource : register(t9);

float4 FillVS(uint vertexID : SV_VertexID) : SV_Position
{
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float FillPS(float4 pos : SV_Position) : SV_Depth
{
	return FillSource.Load(int3(pos.xy, 0));
}

// Whole-skin occlusion cull for the object snow. HiZBuildCS folds the scene
// depth into a max pyramid (level 0 = half res; an odd source folds its
// leftover row/column into the last texel, so every pixel is covered).
// SkinCullCS tests each skin's bounding sphere against it and writes the
// skin's DrawIndexedInstancedIndirect arguments with instance count 0 or 1.
// Conservative: a sphere is culled only when its nearest possible depth is
// behind the farthest scene depth over its whole screen footprint, or when
// it lies outside the view; a skin that could not have produced a fragment
// is skipped, so the image is unchanged bit for bit.
cbuffer SkinCullCB : register(b0)
{
	row_major float4x4 CullViewProj;
	float4 CullCameraPosAdjust;
	float4 CullViewport;  // x, y, width, height in pixels
	float4 CullDepth;     // viewport min, max, depth margin, HiZ level count
	uint CullSkinCount;
	float CullLevel;  // HiZBuildCS: 0 = reading the raw depth (viewport-masked)
	float2 cullPad;
}

Texture2D<float> HiZSource : register(t1);
RWTexture2D<float> HiZDest : register(u2);

// Level 0 reads the raw depth, whose texture can be larger than the area
// the frame rendered into (dynamic resolution): outside the viewport the
// clear value 1.0 would pass as an occluder-free far plane. Masked to 0.
float HiZTap(uint2 p)
{
	float d = HiZSource[p];
	[branch] if (CullLevel < 0.5)
	{
		float2 pf = float2(p);
		if (any(pf < CullViewport.xy) || any(pf >= CullViewport.xy + CullViewport.zw))
			d = 0.0;
	}
	return d;
}

[numthreads(8, 8, 1)] void HiZBuildCS(uint3 id : SV_DispatchThreadID)
{
	uint2 dstDims;
	HiZDest.GetDimensions(dstDims.x, dstDims.y);
	if (any(id.xy >= dstDims))
		return;
	uint2 srcDims;
	HiZSource.GetDimensions(srcDims.x, srcDims.y);
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
	float Radius;  // worldBound radius + lift margin
	uint IndexCount;
	uint BoundsSlot;  // MeshBounds slot when HasBounds
	uint HasBounds;
	float LiftMargin;  // world units the skin can stand off the mesh
	uint ClusterOffset;  // first cluster slot, when ClusterCount > 0
	uint ClusterCount;
	uint IndexPoolOffset;  // this mesh's first index in the pool
	uint ScratchBase;  // where this skin's compacted indices start
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
};
StructuredBuffer<SkinBound> SkinBounds : register(t2);
Texture2D<float> HiZ : register(t3);
StructuredBuffer<float4> MeshBounds : register(t4);
RWByteAddressBuffer SkinArgs : register(u3);

// Reason codes, stored in the argument block's StartInstanceLocation (no
// instance streams, so the draw ignores it) and read back for the census:
// 0 drawn after the test, 1 kept - a box corner is behind the eye, 2 kept -
// occluder read as zero (dead read), 3 culled - outside the view, 4 culled -
// beyond the far plane, 5 culled - behind the scene, 6 kept - sphere path,
// sphere reaches the eye plane, 7 kept - as 1 but the box spans over 2,000
// units (a precombined cell chunk, not an object).
// Projects a local-space box through a skin's world rows into the camera's
// clip space. Returns false when a corner is at or behind the eye plane, so
// the footprint is unbounded and the caller must keep the geometry.
bool ProjectLocalBox(float3 bmin, float3 bmax, float4 r0, float4 r1, float4 r2,
	out float2 lo, out float2 hi, out float zn)
{
	lo = 1e9;
	hi = -1e9;
	zn = 1e9;
	bool behind = false;
	[unroll] for (uint i = 0; i < 8; i++)
	{
		float3 l = float3((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
		float3 wp = float3(dot(r0.xyz, l) + r0.w, dot(r1.xyz, l) + r1.w, dot(r2.xyz, l) + r2.w);
		float4 clip = mul(CullViewProj, float4(wp - CullCameraPosAdjust.xyz, 1.0));
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

// Verdict for a projected box: 0 draw, 2 keep (the pyramid read as zero, so
// the read is dead and the geometry must not be trusted away), 3 outside the
// view, 4 past the far plane, 5 behind the scene.
uint TestProjectedBox(float2 lo, float2 hi, float zn)
{
	if (hi.x < -1.0 || lo.x > 1.0 || hi.y < -1.0 || lo.y > 1.0)
		return 3;
	if (zn > 1.0)
		return 4;
	float depthNear = CullDepth.x + saturate(zn) * (CullDepth.y - CullDepth.x) - CullDepth.z;
	float2 vpMin = CullViewport.xy;
	float2 vpMax = CullViewport.xy + CullViewport.zw - 1.0;
	// One pixel of slack each way covers rasterisation rounding.
	float2 pxMin = clamp(float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5) * CullViewport.zw + CullViewport.xy - 1.0, vpMin, vpMax);
	float2 pxMax = clamp(float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5) * CullViewport.zw + CullViewport.xy + 1.0, vpMin, vpMax);
	float span = max(max(pxMax.x - pxMin.x, pxMax.y - pxMin.y), 1.0);
	// Level where the footprint spans at most two texels per axis.
	uint level = (uint)clamp(ceil(log2(span)) - 1.0, 0.0, CullDepth.w - 1.0);
	uint shift = level + 1;
	uint w, h, levels;
	HiZ.GetDimensions(level, w, h, levels);
	uint2 t0 = min(uint2(pxMin) >> shift, uint2(w, h) - 1);
	uint2 t1 = min(uint2(pxMax) >> shift, uint2(w, h) - 1);
	float occluder = max(max(HiZ.Load(int3(t0.x, t0.y, level)), HiZ.Load(int3(t1.x, t0.y, level))),
		max(HiZ.Load(int3(t0.x, t1.y, level)), HiZ.Load(int3(t1.x, t1.y, level))));
	// Scene depth is never exactly 0 (near-plane clip); a zero here is an
	// unbound or unwritten read, and the geometry must draw.
	if (occluder <= 0.0)
		return 2;
	return depthNear > occluder ? 5 : 0;
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

	[branch] if (b.HasBounds != 0)
	{
		// Tight path: the mesh's local box, grown by the lift margin, placed
		// by the same rows the skin VS uses; eight corners projected. The
		// nearest depth of a convex box is at a corner.
		float3 bmin = MeshBounds[b.BoundsSlot * 2].xyz;
		float3 bmax = MeshBounds[b.BoundsSlot * 2 + 1].xyz;
		float grow = b.LiftMargin / max(length(b.WorldRow0.xyz), 1e-6);
		bmin -= grow;
		bmax += grow;
		if (ProjectLocalBox(bmin, bmax, b.WorldRow0, b.WorldRow1, b.WorldRow2, lo, hi, zn))
			bounded = true;
		else
			reason = length((bmax - bmin) * length(b.WorldRow0.xyz)) > 2000.0 ? 7 : 1;
	}
	else
	{
		// Sphere path (no box yet): exact screen bounds of the sphere from
		// the view-projection rows. Row 3 is the eye plane: w = dot(row3, p)
		// is view depth times |row3.xyz|. Per axis, the tangents from the eye
		// to the sphere's cross-section in the plane spanned by the axis'
		// lateral direction and the depth direction (Mara & McGuire); the
		// axis row is split into its part across the depth axis (scales with
		// 1/w) and its part along it (a constant offset), so a jittered or
		// skewed projection is handled exactly.
		float3 rel = b.Center - CullCameraPosAdjust.xyz;
		float r = b.Radius;
		float4 rowW = CullViewProj[3];
		float fLen = max(length(rowW.xyz), 1e-6);
		float3 fh = rowW.xyz / fLen;
		float wC = dot(rowW.xyz, rel) + rowW.w;
		float wNear = wC - r * fLen;
		[branch] if (wNear <= 1e-3)
		{
			reason = 6;
		}
		else
		{
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
			bounded = true;
		}
	}

	[branch] if (bounded)
	{
		uint verdict = TestProjectedBox(lo, hi, zn);
		reason = verdict;
		if (verdict >= 3)
			draw = 0;
	}
	uint base = id.x * 20;
	SkinArgs.Store(base, b.IndexCount);
	SkinArgs.Store(base + 4, draw);
	SkinArgs.Store(base + 8, 0);
	SkinArgs.Store(base + 12, 0);
	SkinArgs.Store(base + 16, reason);
}

// ---------------------------------------------------------------------------
// Land-exact height layer. Skyrim renders each cell's ground as a 129x129
// mesh built from the 33x33 LAND heightmap by uniform bicubic Catmull-Rom at
// quarter steps, extrapolating linearly across the cell edge instead of
// reading the neighbour (measured to 0.00 in-cell and 0.08 at the edges by
// the landscape probe). This pass evaluates exactly that rule from the
// 128-texel terrain window into a 32-texel window, so the shell can stand
// on the ground the player sees rather than on a chord through its samples.
cbuffer TerrainFineCB : register(b1)
{
	float2 FineOriginWorld;
	float2 WindowOriginWorld;
	uint FineDim;
	uint WindowDim;
	float TexelSize;  // 128
	float FineTexel;  // 32
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
	// Both origins are multiples of the texel, so this is exact.
	float2 uf = (world - WindowOriginWorld) / TexelSize;
	int2 i = (int2)floor(uf);
	float2 f = uf - float2(i);
	// The quad's cell, 32 texels per cell with the window origin on a corner
	// (shifts, not a divide: fxc flags integer division as slow).
	int2 lo = (i >> 5) << 5;
	int2 hi = lo + 32;
	float h[4][4];
	bool missing = false;
	[unroll] for (int j = 0; j < 4; j++)
	{
		[unroll] for (int k = 0; k < 4; k++)
		{
			int2 sIdx = i + int2(k - 1, j - 1);
			int2 c = clamp(sIdx, lo, hi);  // edge vertex when outside the cell
			int2 m = 2 * c - sIdx;         // its reflection back inside
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

// Far-band ground maximum: 2x2 max of the level below, so a shell vertex at
// a 64- or 128-unit band can take the highest ground over the quads it
// touches with four loads. The sentinel wins any max it appears in.
Texture2D<float> FineMaxSrc : register(t9);
RWTexture2D<float> FineMaxDest : register(u6);

[numthreads(8, 8, 1)] void TerrainFineMaxCS(uint3 id : SV_DispatchThreadID)
{
	uint2 dims;
	FineMaxDest.GetDimensions(dims.x, dims.y);
	if (any(id.xy >= dims))
		return;
	uint2 p0 = id.xy * 2;
	float a = FineMaxSrc[p0];
	float b = FineMaxSrc[p0 + uint2(1, 0)];
	float c = FineMaxSrc[p0 + uint2(0, 1)];
	float d = FineMaxSrc[p0 + uint2(1, 1)];
	float lo = min(min(a, b), min(c, d));
	FineMaxDest[id.xy] = lo < -50000.0 ? -100000.0 : max(max(a, b), max(c, d));
}

#ifdef SNOW_CLUSTER_CULL
// ---------------------------------------------------------------------------
// Cluster cull. Skyrim precombines exterior meshes, so a single "object" is
// often a merged chunk of a cell whose box contains the camera - untestable
// as a whole, however tight the box is. This reaches inside it: one thread
// group per skin, one thread per cluster, testing each cluster's own box
// against the same pyramid and compacting the survivors' indices into a
// scratch index buffer that BOTH the prepass and the shading loop draw.
//
// A mesh is cut into AT MOST one group's worth of clusters (256), 64
// triangles each until that would overflow and proportionally larger after -
// so the pass is a single chunk with no loop over chunks. That is not only
// simpler: a barrier inside a loop whose trip count comes from a buffer is
// non-uniform flow control and will not compile.
//
// Bit-identical on two counts: a cluster whose every fragment would fail the
// depth test contributes nothing, and the compaction is ORDERED - an
// exclusive prefix sum over ascending clusters - so surviving triangles keep
// their original relative order. That matters because these draws are
// two-sided and the shading pass tests EQUAL with depth writes off, where
// draw order decides between coincident faces.
StructuredBuffer<float4> ClusterBounds : register(t5);
ByteAddressBuffer ClusterIndexPool : register(t6);
RWByteAddressBuffer ClusterScratchIndices : register(u4);

#define SNOW_CLUSTER_GROUP 256
groupshared uint gClusterScan[SNOW_CLUSTER_GROUP];

[numthreads(SNOW_CLUSTER_GROUP, 1, 1)] void ClusterCullCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
	uint skin = gid.x;
	SkinBound b = SkinBounds[min(skin, max(CullSkinCount, 1u) - 1u)];
	uint argBase = skin * 20;
	// Inactive when the mesh has no clusters (the CPU then binds the mesh's
	// own index buffer and SkinCullCS's arguments stand) or the whole skin is
	// already culled. Uniform across the group, but nothing below branches a
	// barrier on it.
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
		float2 lo, hi;
		float zn;
		if (!ProjectLocalBox(cmin.xyz - grow, cmax.xyz + grow, b.WorldRow0, b.WorldRow1, b.WorldRow2, lo, hi, zn))
			keep = idxCount;  // reaches the eye plane: unbounded, keep
		else
			keep = TestProjectedBox(lo, hi, zn) >= 3 ? 0 : idxCount;
	}

	// Ordered exclusive scan of the surviving index counts. The trip count is
	// a compile-time constant, so every barrier is in uniform flow control.
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
	}
}
#endif
