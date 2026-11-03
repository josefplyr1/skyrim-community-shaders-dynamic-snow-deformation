// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

cbuffer VoxelCB : register(b0)
{
	int4 OriginVox;
	int4 ScrollDelta;
	float VoxelSize;
	float Decay;
	int padDim;
	int SliceAxis;
	int SliceIndex;
	int SliceXray;
	int SliceSource;
	int BlurAxis;
	float2 HeightWindowCenter;
	float HeightHalfExtent;
	float ShelterDust;
	float DepthVox;
	float FieldThreshold;
	float RoundSigma;
	float SlopeMinNz;
	float4 CentreVox;
	float SkyStrength;
	float ForceDirty;
	float OverhangVox;
	float HeadroomVox;
	float4 EdgeParams;
	float4 LipParams;
	int4 Dims;
}

struct VS_OUTPUT
{
	float3 Vox : TEXCOORD0;
	float3 Normal : TEXCOORD1;
};

struct GS_OUTPUT
{
	float4 Position : SV_POSITION;
	float3 Vox : TEXCOORD0;
	nointerpolation uint Axis : TEXCOORD1;
	nointerpolation float Nz : TEXCOORD2;
	nointerpolation float4 Plane : TEXCOORD3;
	nointerpolation uint Copy : TEXCOORD4;
	uint Viewport : SV_ViewportArrayIndex;
};

#define VOXEL_UNDER_LAYERS 4
#define FLAG_OCC 0x100u
#define FLAG_FIELD 0x200u

uint BrickIndex(uint3 physBrick)
{
	uint3 bricks = (uint3)Dims.xyz >> 3;
	return (physBrick.z * bricks.y + physBrick.y) * bricks.x + physBrick.x;
}

#if defined(VSHADER)
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
[maxvertexcount(6)] void main(triangle VS_OUTPUT tri[3], inout TriangleStream<GS_OUTPUT> stream)
{
	float3 gn = cross(tri[1].Vox - tri[0].Vox, tri[2].Vox - tri[0].Vox);
	float3 n = abs(gn);
	uint axis = (n.x > n.y && n.x > n.z) ? 0 : (n.y > n.z ? 1 : 2);
	float3 vn = tri[0].Normal + tri[1].Normal + tri[2].Normal;
	float nz = dot(vn, vn) > 1e-4 ? normalize(vn).z : (dot(gn, gn) > 1e-8 ? normalize(gn).z : 1.0);
	[unroll] for (int i = 0; i < 3; i++)
	{
		float3 v = tri[i].Vox;
		float2 proj = axis == 0 ? v.yz : (axis == 1 ? v.xz : v.xy);
		GS_OUTPUT o;
		o.Position = float4(proj * (2.0 / Dims.x) - 1.0, 0.5, 1.0);
		o.Vox = v;
		o.Axis = axis;
		o.Nz = nz;
		o.Plane = float4(gn, dot(gn, tri[0].Vox));
		o.Copy = 0;
		o.Viewport = 0;
		stream.Append(o);
	}
	stream.RestartStrip();
	[branch] if (abs(gn.z) > 0.05 * length(gn))
	{
		[unroll] for (int i2 = 0; i2 < 3; i2++)
		{
			float3 v = tri[i2].Vox;
			GS_OUTPUT o;
			o.Position = float4(v.xy * (2.0 / Dims.x) - 1.0, 0.5, 1.0);
			o.Vox = v;
			o.Axis = 2;
			o.Nz = nz;
			o.Plane = float4(gn, dot(gn, tri[0].Vox));
			o.Copy = 1;
			o.Viewport = 1;
			stream.Append(o);
		}
		stream.RestartStrip();
	}
}

#elif defined(PSHADER)
RWTexture3D<float> Volume : register(u0);
RWTexture2D<uint> HeightMap : register(u2);
RWTexture2DArray<uint> HeightMapUnder : register(u1);
RWByteAddressBuffer DirtyBricks : register(u3);
RWByteAddressBuffer BrickFlags : register(u4);

void main(GS_OUTPUT input)
{
	float3 vox = input.Vox;
	uint nzq = (uint)round(saturate(input.Nz * 0.5 + 0.5) * 15.0);
	[branch] if (input.Copy == 1)
	{
		int mask4 = Dims.x * 4 - 1;
		int2 sub = ((int2)floor(vox.xy * 4.0) + OriginVox.xy * 4) & mask4;
		float worldZ = (vox.z + (float)OriginVox.z) * VoxelSize;
		uint key = (asuint(worldZ + 32768.0) & ~0xFu) | nzq;
		float gap = HeadroomVox * VoxelSize;
		uint above = HeightMap[(uint2)sub];
		uint was;
		InterlockedMax(HeightMap[(uint2)sub], key, was);
		bool rose = key > was;
		[branch] if (above != 0u && nzq > 7u && worldZ < asfloat(above & ~0xFu) - 32768.0 - gap)
		{
			int mask2 = Dims.x * 2 - 1;
			uint2 sub2 = (uint2)(((int2)floor(vox.xy * 2.0) + OriginVox.xy * 2) & mask2);
			uint layer = 0u;
			[unroll] for (uint k = 0u; k < VOXEL_UNDER_LAYERS - 1u; k++)
			{
				uint held = HeightMapUnder[uint3(sub2, layer)];
				[flatten] if (held != 0u && worldZ < asfloat(held & ~0xFu) - 32768.0 - gap)
					layer++;
			}
			InterlockedMax(HeightMapUnder[uint3(sub2, layer)], key, was);
			rose = rose || key > was;
		}
		[branch] if (rose)
		{
			int3 pv = (int3)floor(vox);
			[branch] if (all(pv >= 0) && all(pv < Dims.xyz))
				DirtyBricks.InterlockedOr(BrickIndex((uint3)((pv + OriginVox.xyz) & (Dims.xyz - 1)) >> 3) * 4, 1u);
		}
		return;
	}
	float d = input.Axis == 0 ? vox.x : (input.Axis == 1 ? vox.y : vox.z);
	float spread = 0.5 * (abs(ddx(d)) + abs(ddy(d)));
	int lo = (int)floor(d - spread);
	int hi = min((int)floor(d + spread), lo + 3);
	int3 base = (int3)floor(vox);
	int3 mask = Dims.xyz - 1;
	float packed = (float)(nzq * 16u + 15u) / 255.0;
	for (int k = lo; k <= hi; k++) {
		int3 p = base;
		if (input.Axis == 0)
			p.x = k;
		else if (input.Axis == 1)
			p.y = k;
		else
			p.z = k;
		if (any(p < 0) || any(p >= Dims.xyz))
			continue;
		uint3 phys = (uint3)((p + OriginVox.xyz) & mask);
		float was = Volume[phys];
		Volume[phys] = packed;
		[branch] if (was <= 0.0)
		{
			uint idx = BrickIndex(phys >> 3) * 4;
			DirtyBricks.InterlockedOr(idx, 1u);
			BrickFlags.InterlockedOr(idx, FLAG_OCC);
		}
	}
}

#else
Texture3D<float> VolumeIn : register(t0);
Texture2D<float2> ShelterMask : register(t1);
Texture2D<float> SkyOpen : register(t2);
Texture3D<float> FieldIn : register(t3);
Texture3D<float> OccupancyIn : register(t4);
Texture2D<uint> HeightMapIn : register(t10);
Texture2DArray<uint> HeightMapUnderIn : register(t11);
RWTexture2D<uint> HeightMapOut : register(u8);
RWTexture2DArray<uint> HeightMapUnderOut : register(u9);
RWTexture3D<float> VolumeOut : register(u0);
RWTexture2D<float> SliceOut : register(u1);
RWByteAddressBuffer OccupancyCount : register(u2);

groupshared uint gOccupied;

RWByteAddressBuffer DirtyBricks : register(u5);
RWByteAddressBuffer BrickFlags : register(u6);
RWByteAddressBuffer DirtyLists : register(u7);
ByteAddressBuffer DirtyBricksIn : register(t6);
ByteAddressBuffer DirtyListsIn : register(t7);
ByteAddressBuffer BrickFlagsIn : register(t8);
#define LIST_ARGS_SEED 0
#define LIST_ARGS_Z 12
#define LIST_ARGS_X 24
#define LIST_ARGS_Y 36
#define LIST_ARGS_FLAGS 48
#define LIST0_OFF 64
#define LIST2_OFF 16448
#define LIST3_OFF 32832

uint2 ListColumn(uint offset, uint index)
{
	uint e = DirtyListsIn.Load(offset + index * 4);
	return uint2(e & 0xFFFFu, e >> 16);
}

groupshared uint gNear;
groupshared uint gZBrick[32];

bool BricksNear(uint3 brick, int rx, int ry, int rz, uint bit, uint gi)
{
	if (gi == 0)
		gNear = 0u;
	GroupMemoryBarrierWithGroupSync();
	int nx = 2 * rx + 1;
	int ny = 2 * ry + 1;
	uint n = (uint)(nx * ny * (2 * rz + 1));
	int3 bricks = Dims.xyz >> 3;
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

groupshared uint gSolid;
[numthreads(8, 8, 8)] void VoxelOccFlagsCS(uint3 p
										   : SV_DispatchThreadID, uint gi
										   : SV_GroupIndex) {
	if (gi == 0)
		gSolid = 0;
	GroupMemoryBarrierWithGroupSync();
	if (VolumeIn[p] > 0.0)
		InterlockedOr(gSolid, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		uint idx = BrickIndex(p >> 3) * 4;
		if (gSolid != 0)
			BrickFlags.InterlockedOr(idx, FLAG_OCC);
		else
			BrickFlags.InterlockedAnd(idx, ~FLAG_OCC);
	}
}

#define COL_TILE 32
#define COL_HALO 6
#define COL_W 44
groupshared uint gColA[COL_W * COL_W];
groupshared uint gColB[COL_W * COL_W];
[numthreads(32, 32, 1)] void VoxelDirtyColsCS(uint3 tid
											  : SV_GroupThreadID, uint3 gid
											  : SV_GroupID) {
	int2 bricks = Dims.xy >> 3;
	int2 tileOrigin = (int2)gid.xy * COL_TILE;
	uint li = tid.y * 32 + tid.x;
	uint zb = (uint)(Dims.z >> 3);
	[loop] for (uint e = li; e < (uint)(COL_W * COL_W); e += 1024u)
	{
		int2 t = int2((int)(e % (uint)COL_W), (int)(e / (uint)COL_W)) - COL_HALO;
		int2 c = (tileOrigin + t) & (bricks - 1);
		uint d = 0;
		[branch] if (ForceDirty > 0.5)
			d = 1;
		else
		{
			[loop] for (uint bz = 0; bz < zb; bz++)
				d |= DirtyBricksIn.Load(BrickIndex(uint3((uint2)c, bz)) * 4);
		}
		gColA[e] = d != 0 ? 1u : 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	uint own = (tid.y + COL_HALO) * COL_W + (tid.x + COL_HALO);
	uint d0 = gColA[own];
	[loop] for (uint e1 = li; e1 < (uint)(COL_W * COL_W); e1 += 1024u)
	{
		int x = (int)(e1 % (uint)COL_W);
		uint d = 0;
		[unroll] for (int dx = -2; dx <= 2; dx++)
		{
			int xx = x + dx;
			if (xx >= 0 && xx < COL_W)
				d |= gColA[(int)e1 + dx];
		}
		gColB[e1] = d;
	}
	GroupMemoryBarrierWithGroupSync();
	[loop] for (uint e2 = li; e2 < (uint)(COL_W * COL_W); e2 += 1024u)
	{
		int y = (int)(e2 / (uint)COL_W);
		uint d = 0;
		[unroll] for (int dy = -2; dy <= 2; dy++)
		{
			int yy = y + dy;
			if (yy >= 0 && yy < COL_W)
				d |= gColB[(int)e2 + dy * COL_W];
		}
		gColA[e2] = d;
	}
	GroupMemoryBarrierWithGroupSync();
	uint d2 = gColA[own];
	[loop] for (uint e3 = li; e3 < (uint)(COL_W * COL_W); e3 += 1024u)
	{
		int y = (int)(e3 / (uint)COL_W);
		uint d = 0;
		[unroll] for (int dy2 = -2; dy2 <= 2; dy2++)
		{
			int yy = y + dy2;
			if (yy >= 0 && yy < COL_W)
				d |= gColA[(int)e3 + dy2 * COL_W];
		}
		gColB[e3] = d;
	}
	GroupMemoryBarrierWithGroupSync();
	uint d3 = gColB[own - 1] | gColB[own] | gColB[own + 1];
	int2 colXY = tileOrigin + (int2)tid.xy;
	bool live = all(colXY < bricks);
	[branch] if (live)
	{
		uint packed = (uint)colXY.x | ((uint)colXY.y << 16);
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
	return (uint3)((logical + OriginVox.xyz) & (Dims.xyz - 1));
}

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

float OccNz(float v)
{
	uint q = (uint)round(v * 255.0);
	return (float)(q >> 4) / 15.0 * 2.0 - 1.0;
}

[numthreads(8, 8, 8)] void VoxelSeedCS(uint3 gid
									   : SV_GroupID, uint3 tid
									   : SV_GroupThreadID) {
	uint2 col = ListColumn(LIST0_OFF, gid.x);
	uint3 p = uint3(col.x * 8 + tid.x, col.y * 8 + tid.y, gid.y * 8 + tid.z);
	uint gi = tid.x + tid.y * 8 + tid.z * 64;
	[branch] if (!BricksNear(uint3(col, gid.y), 0, 0, 0, FLAG_OCC, gi))
	{
		VolumeOut[p] = 0.0;
		return;
	}
	int3 mask = Dims.xyz - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	float occ = VolumeIn[p];
	float seed = 0.0;
	bool open = occ > 0.0;
	int headroom = (int)HeadroomVox;
	[loop] for (int k = 1; k <= headroom && open; k++)
	{
		int3 a = logical + int3(0, 0, k);
		open = a.z >= Dims.z || VolumeIn[Phys(a)] <= 0.0;
	}
	[branch] if (open)
	{
		uint2 hb = p.xy * 4;
		float hmax = -1.0e4;
		uint keys[16];
		[unroll] for (int i = 0; i < 16; i++)
		{
			keys[i] = HeightMapIn[hb + uint2(i & 3, i >> 2)];
			[flatten] if (keys[i] != 0u)
				hmax = max(hmax, (asfloat(keys[i] & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z);
		}
		float nz = OccNz(occ);
		[branch] if (hmax > -1.0e3 && hmax < (float)logical.z + 1.25)
		{
			float nsum = 0.0;
			float nn = 0.0;
			[unroll] for (int j = 0; j < 16; j++)
			{
				[flatten] if (keys[j] != 0u)
				{
					float hz = (asfloat(keys[j] & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z;
					[flatten] if (hz > hmax - 1.0)
					{
						nsum += (float)(keys[j] & 15u) / 15.0 * 2.0 - 1.0;
						nn += 1.0;
					}
				}
			}
			nz = nsum / nn;
		}
		else
		{
			uint2 hb2 = p.xy * 2;
			float h2max = -1.0e4;
			uint keys2[4 * VOXEL_UNDER_LAYERS];
			[unroll] for (int i2 = 0; i2 < 4 * VOXEL_UNDER_LAYERS; i2++)
			{
				keys2[i2] = HeightMapUnderIn[uint3(hb2 + uint2(i2 & 1, (i2 >> 1) & 1), i2 >> 2)];
				[flatten] if (keys2[i2] != 0u)
				{
					float hz = (asfloat(keys2[i2] & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z;
					[flatten] if (hz < (float)logical.z + 1.25)
						h2max = max(h2max, hz);
				}
			}
			[branch] if (h2max > (float)logical.z - 1.25)
			{
				float nsum = 0.0;
				float nn = 0.0;
				[unroll] for (int j2 = 0; j2 < 4 * VOXEL_UNDER_LAYERS; j2++)
				{
					[flatten] if (keys2[j2] != 0u)
					{
						float hz = (asfloat(keys2[j2] & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z;
						[flatten] if (hz > h2max - 1.0 && hz < (float)logical.z + 1.25)
						{
							nsum += (float)(keys2[j2] & 15u) / 15.0 * 2.0 - 1.0;
							nn += 1.0;
						}
					}
				}
				nz = nsum / max(nn, 1.0);
			}
		}
		float facing = smoothstep(SlopeMinNz - 0.05, SlopeMinNz + 0.05, nz);
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

[numthreads(8, 8, 1)] void VoxelBlurZCS(uint3 gid
										: SV_GroupID, uint3 tid
										: SV_GroupThreadID) {
	uint2 col = ListColumn(LIST0_OFF, gid.x);
	uint2 p = col * 8 + tid.xy;
	uint gi = tid.x + tid.y * 8;
	if (gi < 32u)
		gZBrick[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();
	int2 mask = Dims.xy - 1;
	int2 lxy = ((int2)p - OriginVox.xy) & mask;
	float carry = 0.0;
	float top = -1.0e4;
	bool prevSolid = false;
	int runStart = 0;
	float hs[16];
	uint2 hb = p * 4;
	[unroll] for (int i = 0; i < 16; i++)
	{
		uint key = HeightMapIn[hb + uint2(i & 3, i >> 2)];
		hs[i] = key != 0u ? (asfloat(key & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z : -1.0e4;
	}
	float rate = EdgeParams.z;
	[loop] for (int z = 0; z < Dims.z; z++)
	{
		uint3 ph = Phys(int3(lxy, z));
		float s = VolumeIn[ph];
		bool solid = OccupancyIn[ph] > 0.0;
		float zc = (float)z + 0.5;
		[flatten] if (solid && !prevSolid)
			runStart = z;
		[flatten] if (solid)
			carry = s;
		[branch] if (solid && s > 0.01)
		{
			float sum = 0.0;
			float n = 0.0;
			[unroll] for (int k = 0; k < 16; k++)
			{
				float hz = hs[k];
				[flatten] if (hz > (float)runStart - 0.25 && hz < (float)z + 1.25)
				{
					sum += hz;
					n += 1.0;
				}
			}
			float surf = n > 0.0 ? sum / n : (float)z + 0.5;
			top = max(surf + DepthVox * s, (float)runStart + 0.52);
			int zFrom = max(runStart, (int)floor(top) - 3);
			[unroll] for (int q = 1; q <= 6; q++)
			{
				int zz = z - q;
				[branch] if (zz >= zFrom)
				{
					float zzc = (float)zz + 0.5;
					float fb = saturate(FieldThreshold * exp(rate * (top - zzc)));
					uint3 pb = Phys(int3(lxy, zz));
					VolumeOut[pb] = fb;
					if (fb >= 2.0 / 255.0)
						InterlockedOr(gZBrick[pb.z >> 3], 1u);
				}
			}
		}
		float f = 0.0;
		[flatten] if (carry > 0.01)
		{
			f = saturate(FieldThreshold * exp(rate * (top - zc)));
			[flatten] if (zc < top + 4.0)
				f = max(f, 1.0 / 255.0);
		}
		VolumeOut[ph] = f;
		if (f >= 2.0 / 255.0)
			InterlockedOr(gZBrick[ph.z >> 3], 1u);
		prevSolid = solid;
	}
	GroupMemoryBarrierWithGroupSync();
	uint bricks = (uint)Dims.z >> 3;
	if (gi < bricks)
	{
		uint idx = BrickIndex(uint3(col, gi)) * 4;
		if (gZBrick[gi] != 0u)
			BrickFlags.InterlockedOr(idx, FLAG_FIELD);
		else
			BrickFlags.InterlockedAnd(idx, ~FLAG_FIELD);
	}
}

RWTexture3D<float> SupportOut : register(u4);
Texture3D<float> SupportIn : register(t5);
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

float EdgeNoise(float2 q)
{
	float2 q2 = float2(q.x * 2.3 + q.y * 1.1, q.y * 2.7 - q.x * 0.9) + 17.3;
	return 0.65 * EdgeValueNoise(q) + 0.35 * EdgeValueNoise(q2);
}

float ColumnTop(uint2 ph)
{
	uint2 hb = ph * 4;
	float sum = 0.0;
	float n = 0.0;
	[unroll] for (int i = 0; i < 4; i++)
	{
		uint key = HeightMapIn[hb + uint2(i, i)];
		[flatten] if (key != 0u)
		{
			sum += (asfloat(key & ~0xFu) - 32768.0) / VoxelSize - (float)OriginVox.z;
			n += 1.0;
		}
	}
	return n > 0.0 ? sum / n : 1.0e4;
}

[numthreads(8, 8, 8)] void VoxelBlurCS(uint3 gid
									   : SV_GroupID, uint3 tid
									   : SV_GroupThreadID) {
	bool alongY = BlurAxis == 1;
	uint2 col = ListColumn(alongY ? LIST2_OFF : LIST3_OFF, gid.x);
	uint3 p = uint3(col.x * 8 + tid.x, col.y * 8 + tid.y, gid.y * 8 + tid.z);
	uint gi = tid.x + tid.y * 8 + tid.z * 64;
	uint3 brick = uint3(col, gid.y);
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
	int3 mask = Dims.xyz - 1;
	int3 logical = ((int3)p - OriginVox.xyz) & mask;
	int3 step = alongY ? int3(0, 1, 0) : int3(1, 0, 0);
	float sigma = max(RoundSigma, 0.25);
	float invTwoS2 = 0.5 / (sigma * sigma);
	int reach = (int)OverhangVox + 1;
	int span = min(max((int)ceil(sigma * 2.5), reach), 9);
	float airWeight = LipParams.x;
	float v0 = VolumeIn[p];
	float sum = v0;
	float wsum = v0 > 0.0 ? 1.0 : airWeight;
	bool anySnow = v0 > 0.0;
	float dist = alongY ? round(SupportIn[p] * 8.0) : (v0 > 0.0 ? 0.0 : (float)reach);
	float zc = (float)logical.z + 0.5;
	[unroll] for (int dir = -1; dir <= 1; dir += 2)
	{
		[loop] for (int k = 1; k <= span; k++)
		{
			int3 l = logical + step * (k * dir);
			if (any(l < 0) || any(l >= Dims.xyz))
				break;
			uint3 ph = Phys(l);
			if (OccupancyIn[ph] > 0.0 && ColumnTop(ph.xy) - zc > 1.5)
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
		float2 worldXY = ((float2)(logical.xy + OriginVox.xy) + 0.5) * VoxelSize;
		float n = EdgeNoise(worldXY / max(EdgeParams.y, 1.0));
		float dU = dist * VoxelSize - n * EdgeParams.x;
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

AppendStructuredBuffer<uint> BrickList : register(u3);

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
	int3 bricks = Dims.xyz >> 3;
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
		bool inside = all(l >= 0) && all(l < Dims.xyz) && FieldIn[Phys(l)] >= FieldThreshold;
		if (inside)
			InterlockedOr(gAnyIn, cells);
		else
			InterlockedAnd(gAllIn, ~cells);
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0)
	{
		uint idx = BrickIndex(phys) * 4;
		BrickFlags.InterlockedAnd(idx, ~0xFFu);
		BrickFlags.InterlockedOr(idx, gAnyIn & ~gAllIn & 0xFFu);
	}
}

[numthreads(8, 8, 8)] void VoxelBrickListCS(uint3 b
											: SV_DispatchThreadID) {
	int3 bricks = Dims.xyz >> 3;
	if (any((int3)b >= bricks))
		return;
	uint mask = BrickFlagsIn.Load(BrickIndex(b) * 4) & 0xFFu;
	if (mask == 0u)
		return;
	int3 logical = ((int3)b - (OriginVox.xyz >> 3)) & (bricks - 1);
	float3 centre = (float3)(logical * 8 + OriginVox.xyz) + 4.0;
	float3 fromCentre = abs(centre - CentreVox.xyz);
	float3 reach = CentreVox.w * float3(1.0, 1.0, (float)Dims.z / (float)Dims.x);
	if (any(fromCentre - 4.0 > reach))
		return;
	BrickList.Append((uint)logical.x | ((uint)logical.y << 8) | ((uint)logical.z << 16) | (mask << 24));
}

float SliceRead(int3 logical)
{
	uint3 ph = Phys(logical);
	float occ = VolumeIn[ph] > 0.0 ? 1.0 : 0.0;
	float f = FieldIn[ph];
	float cut = f >= FieldThreshold ? 1.0 : 0.0;
	return SliceSource == 0 ? occ : (SliceSource == 1 ? f : cut);
}

[numthreads(8, 8, 8)] void VoxelScrollCS(uint3 tid
										 : SV_DispatchThreadID, uint gi
										 : SV_GroupIndex) {
	int axis = ScrollDelta.w;
	int3 t = (int3)tid;
	int3 logical;
	[branch] if (axis == 3)
		logical = t;
	else
	{
		int d = axis == 0 ? ScrollDelta.x : (axis == 1 ? ScrollDelta.y : ScrollDelta.z);
		int dimA = axis == 0 ? Dims.x : (axis == 1 ? Dims.y : Dims.z);
		int base = d > 0 ? dimA - d : 0;
		logical = axis == 0 ? int3(base + t.x, t.y, t.z) : (axis == 1 ? int3(t.y, base + t.x, t.z) : int3(t.y, t.z, base + t.x));
	}
	if (any(logical < 0) || any(logical >= Dims.xyz))
		return;
	uint3 ph = Phys(logical);
	VolumeOut[ph] = 0.0;
	[branch] if (axis != 2 && logical.z == 0)
	{
		uint2 hb = ph.xy * 4;
		[unroll] for (int i = 0; i < 16; i++)
			HeightMapOut[hb + uint2(i & 3, i >> 2)] = 0u;
		uint2 hb2 = ph.xy * 2;
		[unroll] for (int j = 0; j < 4 * VOXEL_UNDER_LAYERS; j++)
			HeightMapUnderOut[uint3(hb2 + uint2(j & 1, (j >> 1) & 1), j >> 2)] = 0u;
	}
	if (gi == 0)
	{
		uint idx = BrickIndex(ph >> 3) * 4;
		DirtyBricks.InterlockedOr(idx, 1u);
		BrickFlags.InterlockedAnd(idx, ~FLAG_OCC);
	}
}

groupshared uint gTick;
groupshared uint gExpired;
[numthreads(8, 8, 8)] void VoxelDecayCS(uint3 p
										: SV_DispatchThreadID, uint gi
										: SV_GroupIndex) {
	uint idx = BrickIndex(p >> 3) * 4;
	if (gi == 0)
	{
		gTick = BrickFlags.Load(idx) & FLAG_OCC;
		gExpired = 0;
		gSolid = 0;
		gOccupied = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	bool live = gTick != 0u;
	[branch] if (live)
	{
		float v = VolumeOut[p];
		[branch] if (v > 0.0)
		{
			uint q = (uint)round(v * 255.0);
			uint life = q & 15u;
			life = life > 0u ? life - 1u : 0u;
			[branch] if (life == 0u)
			{
				v = 0.0;
				InterlockedOr(gExpired, 1u);
			}
			else
			{
				v = (float)((q & 0xF0u) | life) / 255.0;
				InterlockedOr(gSolid, 1u);
				InterlockedAdd(gOccupied, 1u);
			}
			VolumeOut[p] = v;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0 && live)
	{
		if (gExpired != 0u)
			DirtyBricks.InterlockedOr(idx, 1u);
		if (gSolid != 0u)
			BrickFlags.InterlockedOr(idx, FLAG_OCC);
		else
			BrickFlags.InterlockedAnd(idx, ~FLAG_OCC);
		OccupancyCount.InterlockedAdd(0, gOccupied);
	}
}

[numthreads(8, 8, 1)] void VoxelSliceCS(uint3 id
										: SV_DispatchThreadID) {
	int2 img = SliceAxis == 0 ? Dims.xy : (SliceAxis == 1 ? int2(Dims.x, Dims.z) : int2(Dims.y, Dims.z));
	int fixedDim = SliceAxis == 0 ? Dims.z : (SliceAxis == 1 ? Dims.y : Dims.x);
	if (any(id.xy >= (uint2)img))
	{
		SliceOut[id.xy] = 0.0;
		return;
	}
	int x = (int)id.x;
	int y = img.y - 1 - (int)id.y;
	int3 logical;
	if (SliceAxis == 0)
		logical = int3(x, y, SliceIndex);
	else if (SliceAxis == 1)
		logical = int3(x, SliceIndex, y);
	else
		logical = int3(SliceIndex, x, y);
	float v = 0.0;
	if (SliceXray > 0) {
		[loop] for (int i = 0; i < fixedDim; i++)
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
