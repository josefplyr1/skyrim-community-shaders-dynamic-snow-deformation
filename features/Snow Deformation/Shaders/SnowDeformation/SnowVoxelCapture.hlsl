// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Voxel occupancy of snow statics (VOLUME-SNOW-PLAN V0).
//
// Dominant-axis voxelisation: the GS projects each triangle along its
// largest normal component, the PS marks every voxel the fragment spans
// along that axis. The window is a camera-anchored torus (physical =
// (logical + origin) & (Dim - 1), the deformation map's rule); VoxelScrollCS
// decays it each frame and clears what scrolled in, the raster resets
// covered voxels to 1. VoxelSliceCS writes one plane of it for the menu.
//
// VoxelCB layout must match VoxelVolumeCB in SnowDeformation.h.

cbuffer VoxelCB : register(b0)
{
	// xyz = window origin in voxels (world = voxel * VoxelSize), w = clear all
	int4 OriginVox;
	// xyz = this frame's origin minus last frame's, in voxels
	int4 ScrollDelta;
	float VoxelSize;
	float Decay;
	int Dim;
	int SliceAxis;
	int SliceIndex;
	// >0: the slice is a max over the whole axis (silhouettes), not one plane
	int SliceXray;
	int2 padSlice;
}

struct VS_OUTPUT
{
	// Position in voxel units, relative to the window origin.
	float3 Vox : TEXCOORD0;
};

struct GS_OUTPUT
{
	float4 Position : SV_POSITION;
	float3 Vox : TEXCOORD0;
	nointerpolation uint Axis : TEXCOORD1;
};

#if defined(VSHADER)
// Prefix mirror of StaticsCB (SnowDeformation.h): only the transform is read.
cbuffer StaticCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;
}

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
};

VS_OUTPUT main(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);
	VS_OUTPUT vsout;
	vsout.Vox = worldAbs / VoxelSize - (float3)OriginVox.xyz;
	return vsout;
}

#elif defined(GSHADER)
[maxvertexcount(3)] void main(triangle VS_OUTPUT tri[3], inout TriangleStream<GS_OUTPUT> stream)
{
	float3 n = abs(cross(tri[1].Vox - tri[0].Vox, tri[2].Vox - tri[0].Vox));
	uint axis = (n.x > n.y && n.x > n.z) ? 0 : (n.y > n.z ? 1 : 2);
	[unroll] for (int i = 0; i < 3; i++)
	{
		float3 v = tri[i].Vox;
		float2 proj = axis == 0 ? v.yz : (axis == 1 ? v.xz : v.xy);
		GS_OUTPUT o;
		// The viewport is Dim x Dim, so one pixel is one voxel column.
		o.Position = float4(proj * (2.0 / Dim) - 1.0, 0.5, 1.0);
		o.Vox = v;
		o.Axis = axis;
		stream.Append(o);
	}
	stream.RestartStrip();
}

#elif defined(PSHADER)
RWTexture3D<float> Volume : register(u0);

void main(GS_OUTPUT input)
{
	float3 vox = input.Vox;
	// Extent of the fragment along the projection axis, from the screen
	// derivatives: closes the crack a steep triangle leaves between voxels.
	float d = input.Axis == 0 ? vox.x : (input.Axis == 1 ? vox.y : vox.z);
	float spread = 0.5 * (abs(ddx(d)) + abs(ddy(d)));
	int lo = (int)floor(d - spread);
	int hi = min((int)floor(d + spread), lo + 3);
	int3 base = (int3)floor(vox);
	int mask = Dim - 1;
	for (int k = lo; k <= hi; k++) {
		int3 p = base;
		if (input.Axis == 0)
			p.x = k;
		else if (input.Axis == 1)
			p.y = k;
		else
			p.z = k;
		if (any(p < 0) || any(p >= Dim))
			continue;
		Volume[(uint3)((p + OriginVox.xyz) & mask)] = 1.0;
	}
}

#else
Texture3D<float> VolumeIn : register(t0);
RWTexture3D<float> VolumeOut : register(u0);
RWTexture2D<float> SliceOut : register(u1);
// Occupied-voxel count for the menu: one atomic per group, not per thread.
RWByteAddressBuffer OccupancyCount : register(u2);

groupshared uint gOccupied;

// Physical voxels never move; the origin does. A voxel whose logical
// position was inside last frame's window keeps its value, decayed; one
// that scrolled in is cleared.
[numthreads(8, 8, 8)] void VoxelScrollCS(uint3 p
										 : SV_DispatchThreadID, uint gi
										 : SV_GroupIndex) {
	if (gi == 0)
		gOccupied = 0;
	GroupMemoryBarrierWithGroupSync();

	int mask = Dim - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	int3 old = logical + ScrollDelta.xyz;
	bool inside = OriginVox.w == 0 && all(old >= 0) && all(old < Dim);
	float v = inside ? VolumeIn[p] : 0.0;
	v = max(v - Decay, 0.0);
	VolumeOut[p] = v;

	if (v > 0.0)
		InterlockedAdd(gOccupied, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
		OccupancyCount.InterlockedAdd(0, gOccupied);
}

// One plane of the window for the menu, world-aligned: image top is north
// (axis 0) or up (axes 1 and 2). X-ray: the max over the whole fixed axis,
// so objects read as silhouettes.
[numthreads(8, 8, 1)] void VoxelSliceCS(uint3 id
										: SV_DispatchThreadID) {
	if (any(id.xy >= (uint2)Dim))
		return;
	int x = (int)id.x;
	int y = Dim - 1 - (int)id.y;
	int3 logical;
	if (SliceAxis == 0)
		logical = int3(x, y, SliceIndex);
	else if (SliceAxis == 1)
		logical = int3(x, SliceIndex, y);
	else
		logical = int3(SliceIndex, x, y);
	int mask = Dim - 1;
	float v = 0.0;
	if (SliceXray > 0) {
		[loop] for (int i = 0; i < Dim; i++)
		{
			int3 l = logical;
			if (SliceAxis == 0)
				l.z = i;
			else if (SliceAxis == 1)
				l.y = i;
			else
				l.x = i;
			v = max(v, VolumeIn[(uint3)((l + OriginVox.xyz) & mask)]);
		}
	} else {
		v = VolumeIn[(uint3)((logical + OriginVox.xyz) & mask)];
	}
	SliceOut[id.xy] = v;
}
#endif
