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
	// 0 = occupancy, 1 = the snow field, 2 = the field at the threshold
	int SliceSource;
	int BlurAxis;
	// The top-down height window (SnowHeightCapture), for the seed gate's
	// shelter and sky-openness maps; HalfExtent 0 = no maps, open sky.
	float2 HeightWindowCenter;
	float HeightHalfExtent;
	// Seed weight where the column is sheltered (a dusting, not bare)
	float ShelterDust;
	// The snow depth in this level's voxels, unfloored: the field crosses
	// the threshold at the true top, so a depth under a voxel still reads
	// at height
	float DepthVox;
	// Coverage: the field's value at the snow top, and its crossing
	float FieldThreshold;
	// Sideways sigma in voxels: the shoulder's width at an edge
	float RoundSigma;
	// cos(max slope): the least up-ness a seed's own surface may have
	float SlopeMinNz;
	// xyz = this window's centre in voxel units (absolute), w = the bricks'
	// reach from it in voxels, Chebyshev (the window is a cube, so are its
	// rings). The centre sits AHEAD of the camera, not on it.
	float4 CentreVox;
	// Strength of the sky-openness weighting on the seed, 0-1.
	float SkyStrength;
	// >0.5: every brick column is dirty this rebuild
	float ForceDirty;
	// How many voxels past a snow column the snow may reach sideways
	float OverhangVox;
	// Air voxels a seed needs above it: a member's own inside is not sky
	float HeadroomVox;
	// x = the edge noise amplitude in world units, y = its cell size in world
	// units, z = the field's exponential rate per voxel, w = the overhang in
	// units
	float4 EdgeParams;
	// x = an air neighbour's weight in the sideways average, the rest padding
	float4 LipParams;
}

struct VS_OUTPUT
{
	// Position in voxel units, relative to the window origin.
	float3 Vox : TEXCOORD0;
	float3 Normal : TEXCOORD1;
};

struct GS_OUTPUT
{
	float4 Position : SV_POSITION;
	float3 Vox : TEXCOORD0;
	nointerpolation uint Axis : TEXCOORD1;
	// The surface's up-ness, stored with the voxel: the seed gate reads
	// it instead of reconstructing a facing from the shell's neighbours,
	// which was noise on rough walls and blind to undersides.
	nointerpolation float Nz : TEXCOORD2;
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
	float3 nMS = input.Normal.xyz * 2.0 - 1.0;
	vsout.Normal = float3(dot(WorldRow0.xyz, nMS), dot(WorldRow1.xyz, nMS), dot(WorldRow2.xyz, nMS));
	return vsout;
}

#elif defined(GSHADER)
[maxvertexcount(3)] void main(triangle VS_OUTPUT tri[3], inout TriangleStream<GS_OUTPUT> stream)
{
	float3 gn = cross(tri[1].Vox - tri[0].Vox, tri[2].Vox - tri[0].Vox);
	float3 n = abs(gn);
	uint axis = (n.x > n.y && n.x > n.z) ? 0 : (n.y > n.z ? 1 : 2);
	// The vertex normal's up-ness: it knows a top from an underside, which
	// a winding-blind facing cannot. The geometric normal only for a mesh
	// without one.
	float3 vn = tri[0].Normal + tri[1].Normal + tri[2].Normal;
	float nz = dot(vn, vn) > 1e-4 ? normalize(vn).z : (dot(gn, gn) > 1e-8 ? normalize(gn).z : 1.0);
	[unroll] for (int i = 0; i < 3; i++)
	{
		float3 v = tri[i].Vox;
		float2 proj = axis == 0 ? v.yz : (axis == 1 ? v.xz : v.xy);
		GS_OUTPUT o;
		// The viewport is Dim x Dim, so one pixel is one voxel column.
		o.Position = float4(proj * (2.0 / Dim) - 1.0, 0.5, 1.0);
		o.Vox = v;
		o.Axis = axis;
		o.Nz = nz;
		stream.Append(o);
	}
	stream.RestartStrip();
}

#elif defined(PSHADER)
RWTexture3D<float> Volume : register(u0);
// The surface's height within its voxel (0 = bottom), so the field can put
// the snow top at the true surface plus the depth, not at the voxel centre
// - on a 32-unit ring that is the difference between a 12-unit layer and a
// 78-unit lump (Josef's far rings, 2026-09-07).
RWTexture3D<float> HeightOut : register(u1);

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
	// Packed: high nibble = up-ness (-1..1 over 0..15), low nibble = life,
	// 15 fresh. Any non-zero value is solid; the scroll counts life down.
	uint nzq = (uint)round(saturate(input.Nz * 0.5 + 0.5) * 15.0);
	float packed = (float)(nzq * 16u + 15u) / 255.0;
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
		uint3 phys = (uint3)((p + OriginVox.xyz) & mask);
		Volume[phys] = packed;
		HeightOut[phys] = saturate(vox.z - (float)p.z);
	}
}

#else
Texture3D<float> VolumeIn : register(t0);
// HeightMapProcessCS CombineCS mask: x = suppress (doors), y = melt (shelter, fires)
Texture2D<float2> ShelterMask : register(t1);
// ObjectSkyOpenCS: 1 = open sky
Texture2D<float> SkyOpen : register(t2);
Texture3D<float> FieldIn : register(t3);
// The sideways blur's solid blocker: this frame's occupancy.
Texture3D<float> OccupancyIn : register(t4);
// The raster's surface height within each voxel (see the capture PS).
Texture3D<float> HeightIn : register(t9);
RWTexture3D<float> VolumeOut : register(u0);
RWTexture2D<float> SliceOut : register(u1);
// Occupied-voxel count for the menu: one atomic per group, not per thread.
RWByteAddressBuffer OccupancyCount : register(u2);

groupshared uint gOccupied;
groupshared uint gScrolledIn;

// ---- Dirty bricks. Physical throughout: the window origin snaps to whole
// bricks, so a physical brick is exactly one logical brick, and a scroll
// moves nothing - the slots it reuses are simply marked. ----
// A uint per brick, 1 = its field must be rebuilt this rebuild.
RWByteAddressBuffer DirtyBricks : register(u5);
// A uint per brick: its 8-bit sub-cell crossing mask, kept between rebuilds.
RWByteAddressBuffer BrickFlags : register(u6);
// Dispatch args for the partial passes, then three brick-column lists.
RWByteAddressBuffer DirtyLists : register(u7);
ByteAddressBuffer DirtyBricksIn : register(t6);
ByteAddressBuffer DirtyListsIn : register(t7);
ByteAddressBuffer BrickFlagsIn : register(t8);
// Mirrors kVoxelListArgs* / kVoxelListBytes in SnowDeformation.h.
#define LIST_ARGS_SEED 0
#define LIST_ARGS_Z 12
#define LIST_ARGS_X 24
#define LIST_ARGS_Y 36
#define LIST_ARGS_FLAGS 48
#define LIST0_OFF 64
#define LIST2_OFF 4160
#define LIST3_OFF 8256

uint BrickIndex(uint3 physBrick)
{
	uint bricks = (uint)Dim >> 3;
	return (physBrick.z * bricks + physBrick.y) * bricks + physBrick.x;
}

uint2 ListColumn(uint offset, uint index)
{
	uint e = DirtyListsIn.Load(offset + index * 4);
	return uint2(e & 0xFFFFu, e >> 16);
}

// ---- Active bricks. Bits above the sub-cell mask in BrickFlags: the
// compare pass keeps OCC (any solid voxel in the brick) for every brick,
// the Z sweep keeps FIELD (any snow field in it) for the columns it ran.
// A dirty column is still a 32-brick stack of mostly air; a pass whose
// inputs cannot reach a brick from any FIELD brick skips its loads and
// writes the zeros its persistent outputs need. ----
#define FLAG_OCC 0x100u
#define FLAG_FIELD 0x200u
groupshared uint gNear;
groupshared uint gZBrick[32];

// Any brick within (rx, ry, rz) of `brick` with `bit` set - cooperative
// loads through the UAV (the flags pass writes the same buffer), so every
// thread of the group must call it. Physical wrap is the logical far edge:
// conservative, never wrong. Dense mode (ForceDirty bit 1) says always.
bool BricksNear(uint3 brick, int rx, int ry, int rz, uint bit, uint gi)
{
	if (gi == 0)
		gNear = ((uint)ForceDirty & 2u) != 0u ? 1u : 0u;
	GroupMemoryBarrierWithGroupSync();
	int nx = 2 * rx + 1;
	int ny = 2 * ry + 1;
	uint n = (uint)(nx * ny * (2 * rz + 1));
	int bricks = Dim >> 3;
	[loop] for (uint i = gi; i < n; i += 512u)
	{
		int3 d = int3((int)(i % (uint)nx) - rx, (int)((i / (uint)nx) % (uint)ny) - ry, (int)(i / (uint)(nx * ny)) - rz);
		int3 b = ((int3)brick + d) & (bricks - 1);
		if (BrickFlags.Load(BrickIndex((uint3)b) * 4) & bit)
			InterlockedOr(gNear, 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	return gNear != 0u;
}

// After the raster: which bricks' occupancy changed since the last rebuild
// (t4 = the previous volume), solid/empty only - the facing nibble can flip
// between two triangles' last writes and the life nibble ticks, and neither
// moves the snow. One atomic per brick.
groupshared uint gChanged;
groupshared uint gSolid;
[numthreads(8, 8, 8)] void VoxelDiffCS(uint3 p
									   : SV_DispatchThreadID, uint gi
									   : SV_GroupIndex) {
	if (gi == 0)
	{
		gChanged = 0;
		gSolid = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	bool wasSolid = OccupancyIn[p] > 0.0;
	bool isSolid = VolumeIn[p] > 0.0;
	if (wasSolid != isSolid)
		InterlockedOr(gChanged, 1u);
	if (isSolid)
		InterlockedOr(gSolid, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		uint idx = BrickIndex(p >> 3) * 4;
		if (gChanged != 0)
			DirtyBricks.InterlockedOr(idx, 1u);
		// OCC for every brick, every rebuild: the seed pass's gate.
		if (gSolid != 0)
			BrickFlags.InterlockedOr(idx, FLAG_OCC);
		else
			BrickFlags.InterlockedAnd(idx, ~FLAG_OCC);
	}
}

// One group. A brick column is dirty (D0) if any brick in its stack is; the
// seed and Z passes run there, Z being per column anyway. X reads Z's
// result up to 9 voxels aside, so it runs on D3; Y reads X's result, which
// exists only where X ran, and Y's result is read by the flags a voxel
// aside. So: D1 = D0 + 2 bricks in x; D2 = D1 + 2 in y (Y); D3 = D2 + 2 in
// y + 1 in x (X, and the flags). Each list is the dispatch's group count.
groupshared uint gCol0[1024];
groupshared uint gCol1[1024];
groupshared uint gCol2[1024];
groupshared uint gCol3[1024];
[numthreads(32, 32, 1)] void VoxelDirtyColsCS(uint3 tid
											  : SV_GroupThreadID) {
	uint bricks = (uint)Dim >> 3;
	bool live = all(tid.xy < bricks);
	uint idx = tid.y * 32 + tid.x;
	uint d0 = 0;
	[branch] if (live)
	{
		[branch] if (ForceDirty > 0.5)
			d0 = 1;
		else
		{
			[loop] for (uint bz = 0; bz < bricks; bz++)
				d0 |= DirtyBricksIn.Load(BrickIndex(uint3(tid.xy, bz)) * 4);
		}
	}
	gCol0[idx] = d0 != 0 ? 1u : 0u;
	GroupMemoryBarrierWithGroupSync();
	uint d1 = 0;
	[unroll] for (int dx = -2; dx <= 2; dx++)
	{
		int x = (int)tid.x + dx;
		if (live && x >= 0 && x < (int)bricks)
			d1 |= gCol0[tid.y * 32 + x];
	}
	gCol1[idx] = d1;
	GroupMemoryBarrierWithGroupSync();
	uint d2 = 0;
	[unroll] for (int dy = -2; dy <= 2; dy++)
	{
		int y = (int)tid.y + dy;
		if (live && y >= 0 && y < (int)bricks)
			d2 |= gCol1[y * 32 + tid.x];
	}
	gCol2[idx] = d2;
	GroupMemoryBarrierWithGroupSync();
	uint d3y = 0;
	[unroll] for (int dy2 = -2; dy2 <= 2; dy2++)
	{
		int y = (int)tid.y + dy2;
		if (live && y >= 0 && y < (int)bricks)
			d3y |= gCol2[y * 32 + tid.x];
	}
	gCol3[idx] = d3y;
	GroupMemoryBarrierWithGroupSync();
	uint d3 = 0;
	[unroll] for (int dx2 = -1; dx2 <= 1; dx2++)
	{
		int x = (int)tid.x + dx2;
		if (live && x >= 0 && x < (int)bricks)
			d3 |= gCol3[tid.y * 32 + x];
	}
	[branch] if (live)
	{
		uint packed = tid.x | (tid.y << 16);
		uint slot;
		if (d0 != 0)
		{
			DirtyLists.InterlockedAdd(LIST_ARGS_SEED, 1u, slot);
			DirtyLists.InterlockedAdd(LIST_ARGS_Z, 1u);
			DirtyLists.Store(LIST0_OFF + slot * 4, packed);
		}
		if (d2 != 0)
		{
			DirtyLists.InterlockedAdd(LIST_ARGS_Y, 1u, slot);
			DirtyLists.Store(LIST2_OFF + slot * 4, packed);
		}
		if (d3 != 0)
		{
			DirtyLists.InterlockedAdd(LIST_ARGS_X, 1u, slot);
			DirtyLists.InterlockedAdd(LIST_ARGS_FLAGS, 1u);
			DirtyLists.Store(LIST3_OFF + slot * 4, packed);
		}
	}
}

uint3 Phys(int3 logical)
{
	return (uint3)((logical + OriginVox.xyz) & (Dim - 1));
}

// PatchTexel's mapping (SnowStaticsShell): +worldY is texture v = 0.
bool InWindow(float2 worldXY)
{
	if (HeightHalfExtent <= 0.0)
		return false;
	float2 wl = abs(worldXY - HeightWindowCenter);
	return max(wl.x, wl.y) < HeightHalfExtent;
}

float2 WindowTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - HeightWindowCenter) / HeightHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

float SkyOpenAt(float2 worldXY)
{
	if (!InWindow(worldXY))
		return 1.0;
	float2 dims;
	SkyOpen.GetDimensions(dims.x, dims.y);
	float2 t = WindowTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = SkyOpen.Load(int3(t0.x, t0.y, 0));
	float s10 = SkyOpen.Load(int3(t1.x, t0.y, 0));
	float s01 = SkyOpen.Load(int3(t0.x, t1.y, 0));
	float s11 = SkyOpen.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

float2 ShelterAt(float2 worldXY)
{
	if (!InWindow(worldXY))
		return 0.0;
	float2 dims;
	ShelterMask.GetDimensions(dims.x, dims.y);
	float2 t = WindowTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float2 s00 = ShelterMask.Load(int3(t0.x, t0.y, 0));
	float2 s10 = ShelterMask.Load(int3(t1.x, t0.y, 0));
	float2 s01 = ShelterMask.Load(int3(t0.x, t1.y, 0));
	float2 s11 = ShelterMask.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// The up-ness the raster stored with the voxel (see the capture PS).
// Two rounds of reconstructing a facing from the shell's neighbours - the
// empty-neighbour direction, then the seed layer's rise - were noise on a
// rough wall and blind to an underside; the mesh knows both.
float OccNz(float v)
{
	uint q = (uint)round(v * 255.0);
	return (float)(q >> 4) / 15.0 * 2.0 - 1.0;
}

// Snow seeds: occupied voxels facing up enough, with open air above, and
// weighted by the column's sky openness and shelter the way the 2D
// pipeline weights the skin, doors suppressing.
// Indirect over the D0 column list: group x = list entry, group y = brick z.
[numthreads(8, 8, 8)] void VoxelSeedCS(uint3 gid
									   : SV_GroupID, uint3 tid
									   : SV_GroupThreadID) {
	uint2 col = ListColumn(LIST0_OFF, gid.x);
	uint3 p = uint3(col.x * 8 + tid.x, col.y * 8 + tid.y, gid.y * 8 + tid.z);
	// A seed is an occupied voxel: a brick with none has none. Zero, since
	// the Z sweep reads this volume through the whole column.
	uint gi = tid.x + tid.y * 8 + tid.z * 64;
	[branch] if (!BricksNear(uint3(col, gid.y), 0, 0, 0, FLAG_OCC, gi))
	{
		VolumeOut[p] = 0.0;
		return;
	}
	int mask = Dim - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	float occ = VolumeIn[p];
	float seed = 0.0;
	// Open air above, HeadroomVox deep. The underside shell of a roof or a
	// beam has one air voxel above it - the member's own inside - and its
	// seeded snow grew out through the eave as a fringe (Josef, 2026-09-07).
	bool open = occ > 0.0;
	int headroom = (int)HeadroomVox;
	[loop] for (int k = 1; k <= headroom && open; k++)
	{
		int3 a = logical + int3(0, 0, k);
		open = a.z >= Dim || VolumeIn[Phys(a)] <= 0.0;
	}
	[branch] if (open)
	{
		// An underside is nz -1 and never seeds, at any setting.
		float facing = smoothstep(SlopeMinNz - 0.05, SlopeMinNz + 0.05, OccNz(occ));
		[branch] if (facing > 0.0)
		{
			float2 worldXY = ((float2)(logical.xy + OriginVox.xy) + 0.5) * VoxelSize;
			float2 shelter = ShelterAt(worldXY);
			float exposure = min(SkyOpenAt(worldXY), 1.0 - shelter.y);
			exposure = lerp(1.0, exposure, SkyStrength);
			seed = lerp(ShelterDust, 1.0, exposure) * (1.0 - shelter.x) * facing;
		}
	}
	VolumeOut[p] = seed;
}

// The field, in three passes over logical space (past the window edge is
// empty): Z, then X, then Y.
//
// Z IS A COLUMN SWEEP, bottom to top, one thread per column. A voxel takes
// the seed of the first solid beneath it - nothing past that solid, nothing
// from above, so no seed reaches under the surface it sits on (no snow
// under a beam, a rail or an eave, however thin the member).
//
// THE FIELD IS A RAMP ABOUT THE SNOW TOP, not a gaussian off the voxel
// centre: 0.5 + (top - z) / 2 over a voxel either side, top = the surface's
// height within its voxel (the raster's) plus the depth. Between the two
// centres that bracket it the ramp is linear, so the draw's interpolation
// puts the crossing AT the top, on any ring - the gaussian needed a sigma
// of at least a voxel to be visible at all, and on the 32-unit ring that
// made a 12-unit layer a 78-unit lump, and the hand-over between rings a
// gap with a shadow (Josef, 2026-09-07). The seed weight scales the depth.
// The top never sits under the voxel centre, so the crossing is always
// between this centre and the next, never under the surface.
// Indirect over the D0 column list, one group per entry.
[numthreads(8, 8, 1)] void VoxelBlurZCS(uint3 gid
										: SV_GroupID, uint3 tid
										: SV_GroupThreadID) {
	uint2 col = ListColumn(LIST0_OFF, gid.x);
	uint2 p = col * 8 + tid.xy;
	uint gi = tid.x + tid.y * 8;
	// FIELD per brick of this stack, for the passes after: set or cleared,
	// since the sweep rewrites the whole column.
	if (gi < 32u)
		gZBrick[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();
	int mask = Dim - 1;
	int2 lxy = ((int2)p - OriginVox.xy) & mask;
	float carry = 0.0;
	float top = -1.0e4;
	// THE FIELD IS EXPONENTIAL IN HEIGHT: Coverage * exp(rate * (top - z)),
	// so it crosses Coverage AT the top on every ring (near-linear over the
	// two centres that bracket the top while the rate stays under ~0.35 a
	// voxel, which the C++ side caps), and an air neighbour that dilutes it
	// by k lowers the crossing by ln(1/k) / rate - the gaussian's own log
	// response, which is what made its domes round and its lips long, and
	// what the linear ramp lost (Josef, 2026-09-07: "rectangular", a lifted
	// plateau). The rate is ln 2 over the Rounding in voxels: a half-air
	// neighbourhood drops the snow by the Rounding.
	float rate = EdgeParams.z;
	[loop] for (int z = 0; z < Dim; z++)
	{
		uint3 ph = Phys(int3(lxy, z));
		float s = VolumeIn[ph];
		[flatten] if (OccupancyIn[ph] > 0.0)
		{
			carry = s;
			top = max((float)z + HeightIn[ph] + DepthVox * s, (float)z + 0.52);
		}
		float zc = (float)z + 0.5;
		float f = 0.0;
		[flatten] if (carry > 0.01)
		{
			f = saturate(FieldThreshold * exp(rate * (top - zc)));
			// The support epsilon (one R8 step, under everything's threshold):
			// for a few voxels over its top a snow column still says "mine,
			// nothing at this height", so the sideways passes average it
			// against a taller neighbour instead of ignoring it.
			[flatten] if (zc < top + 4.0)
				f = max(f, 1.0 / 255.0);
		}
		VolumeOut[ph] = f;
		if (f >= 2.0 / 255.0)
			InterlockedOr(gZBrick[ph.z >> 3], 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	uint bricks = (uint)Dim >> 3;
	if (gi < bricks)
	{
		uint idx = BrickIndex(uint3(col, gi)) * 4;
		if (gZBrick[gi] != 0u)
			BrickFlags.InterlockedOr(idx, FLAG_FIELD);
		else
			BrickFlags.InterlockedAnd(idx, ~FLAG_FIELD);
	}
}

// Support for the overhang cap, built with the sideways passes.
RWTexture3D<float> SupportOut : register(u4);
Texture3D<float> SupportIn : register(t5);
// World-anchored value noise in [-1, 1] for the snow's edge.
float EdgeHash(float2 c)
{
	return frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453);
}

float EdgeValueNoise(float2 q)
{
	float2 i = floor(q);
	float2 f = frac(q);
	f = f * f * (3.0 - 2.0 * f);
	float a = EdgeHash(i);
	float b = EdgeHash(i + float2(1.0, 0.0));
	float c = EdgeHash(i + float2(0.0, 1.0));
	float d = EdgeHash(i + float2(1.0, 1.0));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) * 2.0 - 1.0;
}

// Two octaves, the second off-axis: one octave of value noise reads as a
// grid of blobs (Josef, 2026-09-07: "too patterned").
float EdgeNoise(float2 q)
{
	float2 q2 = float2(q.x * 2.3 + q.y * 1.1, q.y * 2.7 - q.x * 0.9) + 17.3;
	return 0.65 * EdgeValueNoise(q) + 0.35 * EdgeValueNoise(q2);
}

// SIDEWAYS IS A NORMALISED AVERAGE THAT STOPS AT SOLID, with an overhang
// cap on top. The average over RoundSigma is the shoulder: a slab's inside
// stays at 1, its rim falls to a half, so the surface rounds off over the
// kernel's width toward the edge, and a narrow member carries less - snow
// on a post is a small cap, as it is. (A max kept every lone seed at full
// height: the 20-u spikes and the fringe of teeth Josef saw.) Normalised
// over what was reached, so snow piles against a wall instead of thinning
// beside it; walking outward it stops at the first occupied voxel, so a
// step riser or a wall blocks one plane's snow from melding into the next.
//
// THE CAP is the Chebyshev distance to the nearest column with snow, built
// separably: X stores its 1D distance in Support, Y takes min over j of
// max(dx, |j|), and the field is cut past OverhangVox with a one-voxel
// ramp. So the shoulder's width and the reach past an edge are two
// settings: deep, rounded snow that still stops at the step's edge.
// Indirect: X over the D3 column list, Y over D2.
[numthreads(8, 8, 8)] void VoxelBlurCS(uint3 gid
									   : SV_GroupID, uint3 tid
									   : SV_GroupThreadID) {
	bool alongY = BlurAxis == 1;
	uint2 col = ListColumn(alongY ? LIST2_OFF : LIST3_OFF, gid.x);
	uint3 p = uint3(col.x * 8 + tid.x, col.y * 8 + tid.y, gid.y * 8 + tid.z);
	uint gi = tid.x + tid.y * 8 + tid.z * 64;
	uint3 brick = uint3(col, gid.y);
	// X's output is non-zero only within 2 bricks (9 voxels) in x of a
	// FIELD brick; Y reads it, and the support, up to 2 bricks in y from
	// where ITS output can be non-zero - within 2 in x and 2 in y of FIELD.
	// So X computes within (2, 0), writes the zero and the "no support"
	// distance within (2, 4), and skips the rest; Y computes within (2, 2)
	// and zeroes the rest, the field being persistent.
	[branch] if (alongY)
	{
		[branch] if (!BricksNear(brick, 2, 2, 0, FLAG_FIELD, gi))
		{
			VolumeOut[p] = 0.0;
			return;
		}
	}
	else
	{
		// Both lookups unconditionally: a group sync inside a branch on
		// the first one's result is X4026 (fxc cannot see it is uniform).
		bool nearXY = BricksNear(brick, 2, 4, 0, FLAG_FIELD, gi);
		bool nearX = BricksNear(brick, 2, 0, 0, FLAG_FIELD, gi);
		[branch] if (!nearX)
		{
			if (nearXY)
			{
				VolumeOut[p] = 0.0;
				SupportOut[p] = 1.0;
			}
			return;
		}
	}
	int mask = Dim - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	int3 step = alongY ? int3(0, 1, 0) : int3(1, 0, 0);
	float sigma = max(RoundSigma, 0.25);
	float invTwoS2 = 0.5 / (sigma * sigma);
	int reach = (int)OverhangVox + 1;
	int span = min(max((int)ceil(sigma * 2.5), reach), 9);
	// DILUTION IS THE SHAPE. An air neighbour weighs AirWeight (1 on rings
	// whose voxel is small against the Rounding, 0 on the far ones): where
	// it votes, the field beside an edge is diluted and the exponential
	// turns that into a smooth, log-shaped drop - the dome and the lip;
	// where it does not, a far ring's rim keeps its full slab instead of
	// eroding a whole voxel at every edge. Snow columns (the sweep's
	// epsilon included) always vote, so a lone post keeps its cap. The
	// weights run out to the overhang, so a lip can reach it.
	float airWeight = LipParams.x;
	float v0 = VolumeIn[p];
	float sum = v0;
	float wsum = v0 > 0.0 ? 1.0 : airWeight;
	bool anySnow = v0 > 0.0;
	float dist = alongY ? round(SupportIn[p] * 8.0) : (v0 > 0.0 ? 0.0 : (float)reach);
	[unroll] for (int dir = -1; dir <= 1; dir += 2)
	{
		[loop] for (int k = 1; k <= span; k++)
		{
			int3 l = logical + step * (k * dir);
			if (any(l < 0) || any(l >= Dim))
				break;
			uint3 ph = Phys(l);
			if (OccupancyIn[ph] > 0.0)
				break;
			float v = VolumeIn[ph];
			float w = exp(-(float)(k * k) * invTwoS2);
			[flatten] if (v > 0.0)
			{
				wsum += w;
				sum += w * v;
				anySnow = true;
			}
			else
				wsum += w * airWeight;
			[flatten] if (alongY)
				dist = min(dist, max(round(SupportIn[ph] * 8.0), (float)k));
			else if (v > 0.0)
				dist = min(dist, (float)k);
		}
	}
	float avg = anySnow ? max(sum / max(wsum, 1e-6), 1.0 / 255.0) : 0.0;
	[branch] if (alongY)
	{
		// The overhang: cut one voxel past it, the cut wandering with a
		// world-anchored noise that also raises and lowers the lip - and
		// ONLY the lip: inside the edge (dist 0) neither term reaches.
		float2 worldXY = ((float2)(logical.xy + OriginVox.xy) + 0.5) * VoxelSize;
		float n = EdgeNoise(worldXY / max(EdgeParams.y, 1.0));
		float dU = dist * VoxelSize - n * EdgeParams.x;
		// A snow column (dist 0) is never cut: with an amplitude past the
		// Overhang the noise reached inside the edge (Josef, 2026-09-07).
		float cut = dist > 0.5 ? saturate((EdgeParams.w + VoxelSize - dU) / VoxelSize) : 1.0;
		float wobble = 1.0 + 0.35 * n * saturate(dist);
		VolumeOut[p] = avg * wobble * cut;
	}
	else
	{
		VolumeOut[p] = avg;
		SupportOut[p] = min(dist, 8.0) / 8.0;
	}
}

// V1b: the draw's brick list. One thread per 8^3 brick, listed when the
// threshold crosses inside its one-voxel-dilated neighbourhood (a crossing
// between two bricks' voxel centres lands in either, so both must draw)
// and it lies within the draw's reach. The append counter is the instance
// count.
AppendStructuredBuffer<uint> BrickList : register(u3);

// The flags: per PHYSICAL brick, its 8-bit sub-cell crossing mask, kept
// between rebuilds and recomputed only on the D3 column list (indirect, one
// group per brick, 512 threads over the 1000 dilated voxels). Per 4^3
// sub-cell (bit = sx | sy << 1 | sz << 2): a crossing inside its own one-
// voxel-dilated range; a voxel at local 3 or 4 on an axis is in both of
// that axis's sub-cells. The draw's march skips sub-cells with no bit.
groupshared uint gAnyIn;
groupshared uint gAllIn;
[numthreads(8, 8, 8)] void VoxelBrickFlagsCS(uint3 gid
											 : SV_GroupID, uint gi
											 : SV_GroupIndex) {
	if (gi == 0)
	{
		gAnyIn = 0u;
		gAllIn = 0xFFu;
	}
	GroupMemoryBarrierWithGroupSync();
	uint2 col = ListColumn(LIST3_OFF, gid.x);
	uint3 phys = uint3(col, gid.y);
	int bricks = Dim >> 3;
	// The field (Y's output) is non-zero within 2 bricks in x and y of a
	// FIELD brick, and the crossing test reads a voxel past the brick: a
	// brick beyond (3, 3, 1) of any FIELD brick has no crossing - its mask
	// is cleared without the loads.
	[branch] if (!BricksNear(phys, 3, 3, 1, FLAG_FIELD, gi))
	{
		if (gi == 0)
			BrickFlags.InterlockedAnd(BrickIndex(phys) * 4, ~0xFFu);
		return;
	}
	int3 base = (((int3)phys - (OriginVox.xyz >> 3)) & (bricks - 1)) * 8;
	[loop] for (uint i = gi; i < 1000u; i += 512u)
	{
		int x = (int)(i % 10u) - 1;
		int y = (int)((i / 10u) % 10u) - 1;
		int z = (int)(i / 100u) - 1;
		uint cells = (z <= 2 ? 0x0Fu : (z >= 5 ? 0xF0u : 0xFFu)) & (y <= 2 ? 0x33u : (y >= 5 ? 0xCCu : 0xFFu)) & (x <= 2 ? 0x55u : (x >= 5 ? 0xAAu : 0xFFu));
		int3 l = base + int3(x, y, z);
		bool inside = all(l >= 0) && all(l < Dim) && FieldIn[Phys(l)] >= FieldThreshold;
		if (inside)
			InterlockedOr(gAnyIn, cells);
		else
			InterlockedAnd(gAllIn, ~cells);
	}
	GroupMemoryBarrierWithGroupSync();
	// The mask only; OCC and FIELD above it belong to the other passes.
	if (gi == 0)
	{
		uint idx = BrickIndex(phys) * 4;
		BrickFlags.InterlockedAnd(idx, ~0xFFu);
		BrickFlags.InterlockedOr(idx, gAnyIn & ~gAllIn & 0xFFu);
	}
}

// The draw list, every rebuild: each flagged physical brick in reach, as a
// LOGICAL brick for the draw VS. Chebyshev: the reach is a cube, as the
// rings are. (A radial reach against a cubic hole left the corners of every
// ring to nobody - the borders Josef saw, 2026-09-07.) Half a brick of
// slack. The hole for the finer level is cut in the draw VS, per frame.
[numthreads(8, 8, 8)] void VoxelBrickListCS(uint3 b
											: SV_DispatchThreadID) {
	int bricks = Dim >> 3;
	if (any((int3)b >= bricks))
		return;
	uint mask = BrickFlagsIn.Load(BrickIndex(b) * 4) & 0xFFu;
	if (mask == 0u)
		return;
	int3 logical = ((int3)b - (OriginVox.xyz >> 3)) & (bricks - 1);
	float3 centre = (float3)(logical * 8 + OriginVox.xyz) + 4.0;
	float3 fromCentre = abs(centre - CentreVox.xyz);
	if (any(fromCentre - 4.0 > CentreVox.w))
		return;
	BrickList.Append((uint)logical.x | ((uint)logical.y << 8) | ((uint)logical.z << 16) | (mask << 24));
}

// Single return: an early return inside a branch reads as X4000 to fxc.
float SliceRead(int3 logical)
{
	uint3 ph = Phys(logical);
	float occ = VolumeIn[ph] > 0.0 ? 1.0 : 0.0;
	float f = FieldIn[ph];
	float cut = f >= FieldThreshold ? 1.0 : 0.0;
	return SliceSource == 0 ? occ : (SliceSource == 1 ? f : cut);
}

// Physical voxels never move; the origin does. A voxel whose logical
// position was inside last frame's window keeps its value, decayed; one
// that scrolled in is cleared.
[numthreads(8, 8, 8)] void VoxelScrollCS(uint3 p
										 : SV_DispatchThreadID, uint gi
										 : SV_GroupIndex) {
	if (gi == 0)
	{
		gOccupied = 0;
		gScrolledIn = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	int mask = Dim - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	int3 old = logical + ScrollDelta.xyz;
	bool inside = OriginVox.w == 0 && all(old >= 0) && all(old < Dim);
	// A slot reused for a voxel 256 away holds the OLD world's field: dirty,
	// whatever the occupancy compare says. One atomic per brick.
	if (!inside)
		InterlockedOr(gScrolledIn, 1u);
	float v = inside ? VolumeIn[p] : 0.0;
	// Memory is the low nibble: down one on tick frames (Decay = 1), gone
	// at zero; the raster rewrites a re-seen voxel at 15 after this.
	[flatten] if (v > 0.0 && Decay > 0.5)
	{
		uint q = (uint)round(v * 255.0);
		uint life = q & 15u;
		life = life > 0u ? life - 1u : 0u;
		v = life == 0u ? 0.0 : (float)((q & 0xF0u) | life) / 255.0;
	}
	VolumeOut[p] = v;

	if (v > 0.0)
		InterlockedAdd(gOccupied, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		OccupancyCount.InterlockedAdd(0, gOccupied);
		if (gScrolledIn != 0)
			DirtyBricks.InterlockedOr(BrickIndex(p >> 3) * 4, 1u);
	}
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
			v = max(v, SliceRead(l));
		}
	} else {
		v = SliceRead(logical);
	}
	SliceOut[id.xy] = v;
}
#endif
