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
		bool behind = false;
		float zMin = 1e9;
		[unroll] for (uint i = 0; i < 8; i++)
		{
			float3 l = float3((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
			float3 wp = float3(dot(b.WorldRow0.xyz, l) + b.WorldRow0.w,
				dot(b.WorldRow1.xyz, l) + b.WorldRow1.w,
				dot(b.WorldRow2.xyz, l) + b.WorldRow2.w);
			float4 clip = mul(CullViewProj, float4(wp - CullCameraPosAdjust.xyz, 1.0));
			if (clip.w <= 1e-3)
				behind = true;
			else
			{
				float2 n = clip.xy / clip.w;
				lo = min(lo, n);
				hi = max(hi, n);
				zMin = min(zMin, clip.z / clip.w);
			}
		}
		if (behind)
			reason = length((bmax - bmin) * length(b.WorldRow0.xyz)) > 2000.0 ? 7 : 1;
		else
		{
			bounded = true;
			zn = zMin;
		}
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
		if (hi.x < -1.0 || lo.x > 1.0 || hi.y < -1.0 || lo.y > 1.0)
		{
			draw = 0;
			reason = 3;
		}
		else if (zn > 1.0)
		{
			draw = 0;
			reason = 4;
		}
		else
		{
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
			// Scene depth is never exactly 0 (near-plane clip); a zero here
			// is an unbound or unwritten read, and the skin must draw.
			if (occluder <= 0.0)
				reason = 2;
			else if (depthNear > occluder)
			{
				draw = 0;
				reason = 5;
			}
		}
	}
	uint base = id.x * 20;
	SkinArgs.Store(base, b.IndexCount);
	SkinArgs.Store(base + 4, draw);
	SkinArgs.Store(base + 8, 0);
	SkinArgs.Store(base + 12, 0);
	SkinArgs.Store(base + 16, reason);
}
