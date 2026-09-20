// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Blood detail tiles (BloodTiles.cpp; BLOOD-DESIGN.md "Detail tiles"). The
// blood map's texel is several units, a splatter's contours a fraction of
// one, so ground that holds blood near the camera gets a tile of a fine
// atlas. A tile is drawn into a scratch first; these passes fold the scratch
// into the atlas, date the tile's clock blocks, and build the nearest-blood
// field the shell grows the soak from.
//
// Pigment textures are read and written as raw UNORM bytes, sRGB-encoded:
// the scratch is drawn through an sRGB view and the atlas sampled through
// one. The scratch holds the colour as drawn; the atlas holds it
// PREMULTIPLIED by concentration, so the shell's filtered taps and the mip
// chain weigh a colour by how much blood carries it and a mark's rim does
// not darken toward the empty texels beside it.

cbuffer BloodTileCB : register(b0)
{
	int2 TileTexel;  // the tile's first texel in the atlas
	int2 TileBlock;  // its first block in the clock atlas

	float2 TileWorldMin;
	float FineTexel;
	int JfaStep;

	// The blood map (SeedTileCS).
	float2 WindowOrigin;
	float CoarseTexel;
	int CoarseDim;

	int2 MapOrigin;
	float BurialNow;
	float BurialRefills;

	// MipDownCS: the tile's first texel at the level written, its size there.
	int2 MipTexel;
	int MipDim;
	int padTile;
}

static const int kTileDim = 512;
static const int kBlockTexels = 8;
static const int kBlockShift = 3;
static const uint kNoSeed = 0xFFFFFFFFu;
// Concentration a texel needs to feed the soak: the solid mark, not its
// faint fringe, so the halo inherits a colour worth spreading.
static const float kSeedAlpha = 0.25;
static const float kMinAlpha = 0.02;

float3 LinearToSrgb(float3 c)
{
	return lerp(c * 12.92, 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float3 SrgbToLinear(float3 c)
{
	return lerp(c / 12.92, pow(max((c + 0.055) / 1.055, 1e-6), 2.4), step(0.04045, c));
}

#if defined(SEED_TILE)
Texture2D<float4> CoarseBlood : register(t0);
Texture2D<float2> CoarseClock : register(t1);
RWTexture2D<float4> Pigment : register(u0);
RWTexture2D<float4> Clock : register(u1);

int3 CoarsePhys(int2 logical)
{
	return int3((logical + MapOrigin) & (CoarseDim - 1), 0);
}

// A tile starts as the blood map's own picture of its ground, so marks whose
// decals are gone stay where they were; live decals redraw themselves over it.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
	if (any(dtid.xy >= (uint)kTileDim))
		return;
	float2 world = TileWorldMin + (float2(dtid.xy) + 0.5) * FineTexel;
	float2 t = (world - WindowOrigin) / CoarseTexel - 0.5;
	float4 acc = 0.0;
	float2 newest = 0.0;
	if (all(t >= 0.0) && all(t < float(CoarseDim - 1)))
	{
		int2 t0 = (int2)t;
		float2 f = t - t0;
		const int2 offs[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
		const float w[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
		for (int i = 0; i < 4; i++)
		{
			int3 p = CoarsePhys(t0 + offs[i]);
			float4 b = CoarseBlood.Load(p);
			if (b.a > kMinAlpha)
			{
				acc += float4(b.rgb * b.a, b.a) * w[i];
				float2 c = CoarseClock.Load(p);
				if (c.y > newest.y)
					newest = c;
			}
		}
	}
	float4 o = 0.0;
	if (acc.a > kMinAlpha)
		o = float4(LinearToSrgb(acc.rgb), acc.a);
	Pigment[TileTexel + int2(dtid.xy)] = o;
	// One thread per block dates it.
	if (all(gtid.xy == 3u))
		Clock[TileBlock + (int2(dtid.xy) >> kBlockShift)] = newest.y > 0.0 ? float4(newest.x, newest.y, newest.y, 0.0) : float4(0.0, 0.0, 0.0, 0.0);
}
#endif

#if defined(MERGE_CLOCK)
Texture2D<float4> ScratchPigment : register(t0);
Texture2D<float2> ScratchClock : register(t1);
Texture2D<float4> PrevClock : register(t2);
RWTexture2D<float4> Clock : register(u0);

// Per block: x = burial clock and y = game hours of the newest deposit,
// z = game hours of the oldest still showing (the soak grows from it),
// w = 1 when what lay here was buried for good and this merge replaces it.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const int blocks = kTileDim / kBlockTexels;
	if (any(dtid.xy >= (uint)blocks))
		return;
	float2 newest = 0.0;
	for (int y = 0; y < kBlockTexels; y++)
	{
		for (int x = 0; x < kBlockTexels; x++)
		{
			int3 p = int3(int2(dtid.xy) * kBlockTexels + int2(x, y), 0);
			if (ScratchPigment.Load(p).a >= kMinAlpha)
			{
				float2 c = ScratchClock.Load(p);
				if (c.y > newest.y)
					newest = c;
			}
		}
	}
	float4 prev = PrevClock.Load(int3(dtid.xy, 0));
	float4 o = float4(prev.xyz, 0.0);
	if (newest.y > 0.0)
	{
		const bool prevLive = prev.y > 0.0 && (BurialNow - prev.x) < BurialRefills;
		if (prevLive)
		{
			o.xy = newest.y >= prev.y ? newest : prev.xy;
			o.z = min(prev.z, newest.y);
		}
		else
		{
			o = float4(newest.x, newest.y, newest.y, prev.y > 0.0 ? 1.0 : 0.0);
		}
	}
	Clock[TileBlock + int2(dtid.xy)] = o;
}
#endif

#if defined(MERGE_PIGMENT)
Texture2D<float4> ScratchPigment : register(t0);
Texture2D<float4> PrevPigment : register(t1);
Texture2D<float4> ClockAtlas : register(t2);
RWTexture2D<float4> Pigment : register(u0);

// The blood map's own blend: the pigment as drawn, the concentration's
// high-water mark.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= (uint)kTileDim))
		return;
	int3 p = int3(dtid.xy, 0);
	float4 n = ScratchPigment.Load(p);
	float4 o = PrevPigment.Load(p);
	if (ClockAtlas.Load(int3(TileBlock + (int2(dtid.xy) >> kBlockShift), 0)).w > 0.5)
		o = 0.0;
	if (n.a >= kMinAlpha)
	{
		float a = max(n.a, o.a);
		o = float4(LinearToSrgb(SrgbToLinear(n.rgb) * a), a);
	}
	Pigment[TileTexel + int2(dtid.xy)] = o;
}
#endif

#if defined(MIP_DOWN)
Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

// One level of the tile's own mip chain, from a copy of the level above:
// tiles never filter into their neighbours in the atlas.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= (uint)MipDim))
		return;
	float4 acc = 0.0;
	for (int i = 0; i < 4; i++)
	{
		float4 s = Src.Load(int3(int2(dtid.xy) * 2 + int2(i & 1, i >> 1), 0));
		acc += float4(SrgbToLinear(s.rgb), s.a);
	}
	acc *= 0.25;
	Dst[MipTexel + int2(dtid.xy)] = float4(LinearToSrgb(acc.rgb), acc.a);
}
#endif

#if defined(JFA_INIT)
Texture2D<float4> PigmentRaw : register(t0);
RWTexture2D<uint> Dst : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= (uint)kTileDim))
		return;
	float a = PigmentRaw.Load(int3(TileTexel + int2(dtid.xy), 0)).a;
	Dst[dtid.xy] = a >= kSeedAlpha ? (dtid.x | (dtid.y << 16)) : kNoSeed;
}
#endif

#if defined(JFA_STEP)
Texture2D<uint> Src : register(t0);
RWTexture2D<uint> Dst : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= (uint)kTileDim))
		return;
	const int2 p = int2(dtid.xy);
	uint best = kNoSeed;
	int bestD = 0x7FFFFFFF;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			int2 q = p + int2(x, y) * JfaStep;
			if (any(q < 0) || any(q >= kTileDim))
				continue;
			uint s = Src.Load(int3(q, 0));
			if (s == kNoSeed)
				continue;
			int2 d = int2(s & 0xFFFFu, s >> 16) - p;
			int dd = d.x * d.x + d.y * d.y;
			if (dd < bestD)
			{
				bestD = dd;
				best = s;
			}
		}
	}
	Dst[dtid.xy] = best;
}
#endif

#if defined(JFA_RESOLVE)
Texture2D<uint> Src : register(t0);
RWTexture2D<float4> Seeds : register(u0);

// rg = offset to the nearest solid blood in texels, + 128; 0 = none in reach.
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	if (any(dtid.xy >= (uint)kTileDim))
		return;
	uint s = Src.Load(int3(dtid.xy, 0));
	float4 o = 0.0;
	if (s != kNoSeed)
	{
		int2 d = int2(s & 0xFFFFu, s >> 16) - int2(dtid.xy);
		if (all(abs(d) <= 127))
			o.xy = float2(d + 128) / 255.0;
	}
	Seeds[TileTexel + int2(dtid.xy)] = o;
}
#endif
