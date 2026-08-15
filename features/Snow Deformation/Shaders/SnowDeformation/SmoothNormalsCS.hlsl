// Smoothed-normal builder for the statics snow skin.
//
// Flat hard-edged meshes (walkway planks, roofs, fence pole caps) carry SPLIT
// normals: every face's vertices point along that face alone. Inflating snow
// along split normals turns a flat top into a rigid parallel sheet with an
// air gap at the rim, because the side faces' twin vertices never rise.
// Averaging normals across all vertices that SHARE A POSITION produces the
// smooth normals such meshes lack: shared-position twins displace to the
// same point (the crack is sealed by construction), plank edges tilt
// outward and mushroom like pole caps, and already-smooth meshes (rocks)
// are unchanged since their normals already agree.
//
// Three passes over a copy of the mesh's vertex buffer:
//   AccumulateCS; quantized-position hash table accumulates fixed-point
//                  normal sums (linear probing, fingerprint-guarded).
//   ResolveCS   ; per vertex, look up its position's sum and write the
//                  normalized average. w=1 marks validity; unresolvable
//                  vertices write 0 and the VS falls back to the raw normal.
//   FlatStatsCS ; one-group reduction writing the mesh's FLATNESS stats to
//                  OutNormals[VertexCount]: x = fraction of vertices whose
//                  smoothed normal diverges from the raw one. Split-normal
//                  plates (walkways, roofs, planks) score high; organically
//                  smooth meshes (rocks, drifts) score ~0. The skin VS reads
//                  it to pick flat vs rounded snow; no CPU readback.
//
// Runs ONCE per unique geometry (cached CPU-side by vertex buffer).

ByteAddressBuffer SrcVerts : register(t0);
RWByteAddressBuffer HashTable : register(u0);  // 16B/slot: int sumX, sumY, sumZ, uint fingerprint
RWStructuredBuffer<float4> OutNormals : register(u1);

cbuffer SmoothCB : register(b0)
{
	uint VertexCount;
	uint StrideBytes;
	uint NormalOffsetBytes;
	uint PosIsFloat32;  // 1 = float4 position, 0 = half4

	uint TableMask;  // slots - 1 (power of two)
	uint3 padSm;
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

// Quantize to 1/32 model unit: welds coincident vertices, keeps distinct ones apart.
uint2 PositionHashes(float3 p)
{
	int3 q = (int3)round(p * 32.0);
	uint h1 = (uint)(q.x * 73856093) ^ (uint)(q.y * 19349663) ^ (uint)(q.z * 83492791);
	uint h2 = ((uint)(q.x * 40503) ^ (uint)(q.y * 104729) ^ (uint)(q.z * 12289)) | 1u;  // never 0 (0 = empty slot)
	return uint2(h1, h2);
}

// Finds (or in AccumulateCS claims) the slot for fingerprint h2. Returns
// 0xFFFFFFFF when probing fails (table pressure); vertex falls back.
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
		float4 smoothN = OutNormals[v];
		[branch] if (smoothN.w > 0.5)
		{
			total++;
			float3 rawN = normalize(LoadNormal(v));
			normalSum += rawN;
			// ~18-degree divergence marks a split-normal (edge/corner) vertex.
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
		// Alignment ratio: 1 when every normal agrees (a planar SHEET), near
		// 0 when they spread or cancel (rocks, boxes). y carries the mean
		// direction's z. Neither is consumed today: two alignment-based
		// classifier extensions (any-planar, then sloped-sheets-only) both
		// misclassified real compound meshes worse than they helped; the
		// stats stay available, but do not build a third heuristic on them.
		float sumLen = length(sumNormal);
		float alignRatio = sumTotal > 0 ? sumLen / float(sumTotal) : 0.0;
		float meanNz = sumLen > 1e-3 ? sumNormal.z / sumLen : 0.0;
		OutNormals[VertexCount] = float4(flatFraction, meanNz, alignRatio, 1.0);
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
			result = float4(n / len, 1.0);
	}
	OutNormals[v] = result;
}
#endif
