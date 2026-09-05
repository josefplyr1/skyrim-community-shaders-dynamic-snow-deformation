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
	float3 cullPad;
}

Texture2D<float> HiZSource : register(t1);
RWTexture2D<float> HiZDest : register(u2);

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
	float d = max(max(HiZSource[uint2(p0.x, p0.y)], HiZSource[uint2(p1.x, p0.y)]),
		max(HiZSource[uint2(p0.x, p1.y)], HiZSource[uint2(p1.x, p1.y)]));
	if (tail.x)
		d = max(d, max(HiZSource[uint2(p2.x, p0.y)], HiZSource[uint2(p2.x, p1.y)]));
	if (tail.y)
		d = max(d, max(HiZSource[uint2(p0.x, p2.y)], HiZSource[uint2(p1.x, p2.y)]));
	if (tail.x && tail.y)
		d = max(d, HiZSource[uint2(p2.x, p2.y)]);
	HiZDest[id.xy] = d;
}

struct SkinBound
{
	float3 Center;
	float Radius;
	uint IndexCount;
	uint3 pad;
};
StructuredBuffer<SkinBound> SkinBounds : register(t2);
Texture2D<float> HiZ : register(t3);
RWByteAddressBuffer SkinArgs : register(u3);

[numthreads(64, 1, 1)] void SkinCullCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= CullSkinCount)
		return;
	SkinBound b = SkinBounds[id.x];
	float3 rel = b.Center - CullCameraPosAdjust.xyz;
	uint draw = 1;

	float2 ndcMin = 1e9;
	float2 ndcMax = -1e9;
	bool behind = false;
	[unroll] for (uint i = 0; i < 8; i++)
	{
		float3 corner = rel + b.Radius * float3((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0);
		float4 clip = mul(CullViewProj, float4(corner, 1.0));
		if (clip.w <= 1e-3)
			behind = true;
		else
		{
			float2 n = clip.xy / clip.w;
			ndcMin = min(ndcMin, n);
			ndcMax = max(ndcMax, n);
		}
	}
	// Any corner at or behind the eye plane: keep, the footprint is unbounded.
	[branch] if (!behind)
	{
		if (ndcMax.x < -1.0 || ndcMin.x > 1.0 || ndcMax.y < -1.0 || ndcMin.y > 1.0)
			draw = 0;
		else
		{
			// Nearest point of the sphere in view depth: the centre pulled
			// toward the eye plane by the radius (row 3 of the matrix is that
			// plane's normal for any perspective whose w is view depth).
			float3 fwd = normalize(CullViewProj[3].xyz);
			float4 cn = mul(CullViewProj, float4(rel - fwd * b.Radius, 1.0));
			if (cn.w > 1e-3)
			{
				float zn = cn.z / cn.w;
				if (zn > 1.0)
					draw = 0;  // whole sphere beyond the far plane
				else
				{
					float depthNear = CullDepth.x + saturate(zn) * (CullDepth.y - CullDepth.x) - CullDepth.z;
					float2 vpMin = CullViewport.xy;
					float2 vpMax = CullViewport.xy + CullViewport.zw - 1.0;
					float2 pxMin = clamp(float2(ndcMin.x * 0.5 + 0.5, 0.5 - ndcMax.y * 0.5) * CullViewport.zw + CullViewport.xy, vpMin, vpMax);
					float2 pxMax = clamp(float2(ndcMax.x * 0.5 + 0.5, 0.5 - ndcMin.y * 0.5) * CullViewport.zw + CullViewport.xy, vpMin, vpMax);
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
					if (occluder > 0.0 && depthNear > occluder)
						draw = 0;
				}
			}
		}
	}
	uint base = id.x * 20;
	SkinArgs.Store(base, b.IndexCount);
	SkinArgs.Store(base + 4, draw);
	SkinArgs.Store(base + 8, 0);
	SkinArgs.Store(base + 12, 0);
	SkinArgs.Store(base + 16, 0);
}
