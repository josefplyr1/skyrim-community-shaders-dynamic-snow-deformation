ByteAddressBuffer SrcVerts : register(t0);
RWByteAddressBuffer HashTable : register(u0);
RWStructuredBuffer<float4> OutNormals : register(u1);

cbuffer SmoothCB : register(b0)
{
	uint VertexCount;
	uint StrideBytes;
	uint NormalOffsetBytes;
	uint PosIsFloat32;

	uint TableMask;
	uint ColorOffsetBytes;
	uint HasColor;
	uint BoundsSlot;

	uint IndexPoolOffset;
	uint IndexCount;
	uint ClusterOffset;
	uint ClusterStrideIndices;

	uint OutBase;
	uint3 SmoothPad;
}

static const uint kMaxProbe = 16;
static const float kFixedScale = 4096.0;

float3 LoadPosition(uint v)
{
	uint base = v * StrideBytes;
	[branch] if (PosIsFloat32 != 0) {
		return asfloat(SrcVerts.Load3(base));
	} else {
		uint2 raw = SrcVerts.Load2(base);
		return float3(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF));
	}
}

float3 LoadNormal(uint v)
{
	uint raw = SrcVerts.Load(v * StrideBytes + NormalOffsetBytes);
	return float3(raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF) / 255.0 * 2.0 - 1.0;
}

float LoadColorAlpha(uint v)
{
	float alpha = 1.0;
	[branch] if (HasColor != 0) {
		uint raw = SrcVerts.Load(v * StrideBytes + ColorOffsetBytes);
		alpha = float(raw >> 24) / 255.0;
	}
	return alpha;
}

uint2 PositionHashes(float3 p)
{
	int3 q = (int3)round(p * 32.0);
	uint h1 = (uint)(q.x * 73856093) ^ (uint)(q.y * 19349663) ^ (uint)(q.z * 83492791);
	uint h2 = ((uint)(q.x * 40503) ^ (uint)(q.y * 104729) ^ (uint)(q.z * 12289)) | 1u;
	return uint2(h1, h2);
}

uint FindSlot(uint h1, uint h2, bool claim)
{
	uint slot = h1 & TableMask;
	for (uint probeI = 0; probeI < kMaxProbe; probeI++) {
		uint addr = slot * 16 + 12;
		uint existing;
		if (claim) {
			HashTable.InterlockedCompareExchange(addr, 0u, h2, existing);
			if (existing == 0 || existing == h2)
				return slot;
		} else {
			existing = HashTable.Load(addr);
			if (existing == h2)
				return slot;
			if (existing == 0)
				return 0xFFFFFFFF;
		}
		slot = (slot + 1) & TableMask;
	}
	return 0xFFFFFFFF;
}

#ifdef ACCUMULATE
[numthreads(64, 1, 1)] void main(uint3 dtid
								 : SV_DispatchThreadID) {
	uint v = dtid.x;
	if (v >= VertexCount)
		return;

	uint2 h = PositionHashes(LoadPosition(v));
	uint slot = FindSlot(h.x, h.y, true);
	if (slot == 0xFFFFFFFF)
		return;

	int3 fixedNrm = (int3)(LoadNormal(v) * kFixedScale);
	uint base = slot * 16;
	uint ignored;
	HashTable.InterlockedAdd(base + 0, asuint(fixedNrm.x), ignored);
	HashTable.InterlockedAdd(base + 4, asuint(fixedNrm.y), ignored);
	HashTable.InterlockedAdd(base + 8, asuint(fixedNrm.z), ignored);
}
#endif

#ifdef FLATSTATS
groupshared uint gsDivergent[64];
groupshared uint gsTotal[64];
groupshared float3 gsNormalSum[64];
[numthreads(64, 1, 1)] void main(uint3 gtid
								 : SV_GroupThreadID) {
	uint divergent = 0;
	uint total = 0;
	float3 normalSum = float3(0.0, 0.0, 0.0);
	for (uint v = gtid.x; v < VertexCount; v += 64) {
		float4 smoothN = OutNormals[OutBase + v];
		[branch] if (smoothN.w > 0.5)
		{
			total++;
			float3 rawN = normalize(LoadNormal(v));
			normalSum += rawN;
			if (dot(smoothN.xyz, rawN) < 0.95)
				divergent++;
		}
	}
	gsDivergent[gtid.x] = divergent;
	gsTotal[gtid.x] = total;
	gsNormalSum[gtid.x] = normalSum;
	GroupMemoryBarrierWithGroupSync();
	if (gtid.x == 0) {
		uint sumDivergent = 0;
		uint sumTotal = 0;
		float3 sumNormal = float3(0.0, 0.0, 0.0);
		for (uint i = 0; i < 64; i++) {
			sumDivergent += gsDivergent[i];
			sumTotal += gsTotal[i];
			sumNormal += gsNormalSum[i];
		}
		float flatFraction = sumTotal > 0 ? float(sumDivergent) / float(sumTotal) : 0.0;
		float sumLen = length(sumNormal);
		float alignRatio = sumTotal > 0 ? sumLen / float(sumTotal) : 0.0;
		float meanNz = sumLen > 1e-3 ? sumNormal.z / sumLen : 0.0;
		OutNormals[OutBase + VertexCount] = float4(flatFraction, meanNz, alignRatio, 1.0);
	}
}
#endif

#ifdef RESOLVE
[numthreads(64, 1, 1)] void main(uint3 dtid
								 : SV_DispatchThreadID) {
	uint v = dtid.x;
	if (v >= VertexCount)
		return;

	uint2 h = PositionHashes(LoadPosition(v));
	uint slot = FindSlot(h.x, h.y, false);

	float4 result = float4(0.0, 0.0, 0.0, 0.0);
	if (slot != 0xFFFFFFFF) {
		uint base = slot * 16;
		int3 sum = int3(asint(HashTable.Load(base + 0)), asint(HashTable.Load(base + 4)), asint(HashTable.Load(base + 8)));
		float3 n = float3(sum) / kFixedScale;
		float len = length(n);
		if (len > 0.001)
			result = float4(n / len, 1.0 + LoadColorAlpha(v));
	}
	OutNormals[OutBase + v] = result;
}
#endif

#ifdef MESHBOUNDS
RWStructuredBuffer<float4> MeshBounds : register(u2);
groupshared float3 gBoundsMin[256];
groupshared float3 gBoundsMax[256];

[numthreads(256, 1, 1)] void main(uint3 gtid : SV_GroupThreadID)
{
	float3 mn = 1e30;
	float3 mx = -1e30;
	for (uint v = gtid.x; v < VertexCount; v += 256) {
		float3 p = LoadPosition(v);
		mn = min(mn, p);
		mx = max(mx, p);
	}
	gBoundsMin[gtid.x] = mn;
	gBoundsMax[gtid.x] = mx;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint s = 128; s > 0; s >>= 1) {
		if (gtid.x < s) {
			gBoundsMin[gtid.x] = min(gBoundsMin[gtid.x], gBoundsMin[gtid.x + s]);
			gBoundsMax[gtid.x] = max(gBoundsMax[gtid.x], gBoundsMax[gtid.x + s]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gtid.x == 0) {
		MeshBounds[BoundsSlot * 2] = float4(gBoundsMin[0], 0.0);
		MeshBounds[BoundsSlot * 2 + 1] = float4(gBoundsMax[0], 0.0);
	}
}
#endif

#ifdef CLUSTERBOUNDS
ByteAddressBuffer ClusterSrcIndices : register(t1);
RWStructuredBuffer<float4> ClusterBoundsOut : register(u3);

[numthreads(64, 1, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	uint stride = max(ClusterStrideIndices, 3u);
	uint clusterCount = (IndexCount + stride - 1) / stride;
	if (dtid.x >= clusterCount)
		return;
	uint start = dtid.x * stride;
	uint count = min(stride, IndexCount - start);

	float3 mn = 1e30;
	float3 mx = -1e30;
	for (uint k = 0; k < count; k++) {
		uint si = IndexPoolOffset + start + k;
		uint word = ClusterSrcIndices.Load((si >> 1) << 2);
		uint vi = (si & 1) ? (word >> 16) : (word & 0xFFFF);
		float3 p = LoadPosition(min(vi, VertexCount - 1));
		mn = min(mn, p);
		mx = max(mx, p);
	}
	ClusterBoundsOut[(ClusterOffset + dtid.x) * 2] = float4(mn, asfloat(start));
	ClusterBoundsOut[(ClusterOffset + dtid.x) * 2 + 1] = float4(mx, asfloat(count));
}
#endif
