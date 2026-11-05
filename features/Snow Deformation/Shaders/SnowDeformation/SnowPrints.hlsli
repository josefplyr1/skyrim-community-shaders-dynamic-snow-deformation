#ifndef __SNOW_PRINTS_HLSLI__
#define __SNOW_PRINTS_HLSLI__

namespace SnowPrints
{
	Texture2DArray<uint> PrintMap : register(t114);
	Texture2D<uint> PrintTiles : register(t115);

	static const float kTexel = 3.0;
	static const float kZOffset = 524288.0;
	static const int kTileShift = 5;

	float TexelDepth(int2 a_texel, int a_mask, float a_z)
	{
		const int2 phys = a_texel & a_mask;
		const uint a = PrintMap.Load(int4(phys, 0, 0));
		const uint b = PrintMap.Load(int4(phys, 1, 0));
		const float za = float(a >> 10) * 0.25 - kZOffset;
		const float zb = float(b >> 10) * 0.25 - kZOffset;
		return max(float(a & 1023u) * saturate(2.0 - abs(a_z - za) / 6.0), float(b & 1023u) * saturate(2.0 - abs(a_z - zb) / 6.0)) * (1.0 / 1023.0);
	}

	int Mask(int2 a_t0, int a_span)
	{
		const int4 rect = SharedData::snowDeformationSettings.PrintRect;
		int mask = -1;
		[branch] if (all(a_t0 >= rect.xy) && all(a_t0 + a_span < rect.zw))
		{
			uint3 dims;
			PrintMap.GetDimensions(dims.x, dims.y, dims.z);
			const int m = int(dims.x) - 1;
			[flatten] if (PrintTiles.Load(int3((a_t0 >> kTileShift) & (m >> kTileShift), 0)) != 0u)
				mask = m;
		}
		return mask;
	}

	float DepthFast(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0, 1);
		float d = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 f = tf - t0f;
			d = lerp(lerp(TexelDepth(t0, mask, a_world.z), TexelDepth(t0 + int2(1, 0), mask, a_world.z), f.x),
				lerp(TexelDepth(t0 + int2(0, 1), mask, a_world.z), TexelDepth(t0 + int2(1, 1), mask, a_world.z), f.x), f.y);
		}
		return d;
	}

	float4 BSpline(float a_f)
	{
		const float f2 = a_f * a_f;
		const float f3 = f2 * a_f;
		return float4(1.0 - 3.0 * a_f + 3.0 * f2 - f3, 4.0 - 6.0 * f2 + 3.0 * f3, 1.0 + 3.0 * a_f + 3.0 * f2 - 3.0 * f3, f3) * (1.0 / 6.0);
	}

	float4 BSplineSlope(float a_f)
	{
		const float f2 = a_f * a_f;
		return float4(-0.5 + a_f - 0.5 * f2, -2.0 * a_f + 1.5 * f2, 0.5 + a_f - 1.5 * f2, 0.5 * f2);
	}

	float4 BlockRow(int2 a_t0, int a_y, int a_mask, float a_z)
	{
		return float4(TexelDepth(a_t0 + int2(-1, a_y), a_mask, a_z), TexelDepth(a_t0 + int2(0, a_y), a_mask, a_z),
			TexelDepth(a_t0 + int2(1, a_y), a_mask, a_z), TexelDepth(a_t0 + int2(2, a_y), a_mask, a_z));
	}

	float Depth(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0 - 1, 3);
		float d = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 inner0 = float2(TexelDepth(t0, mask, a_world.z), TexelDepth(t0 + int2(1, 0), mask, a_world.z));
			const float2 inner1 = float2(TexelDepth(t0 + int2(0, 1), mask, a_world.z), TexelDepth(t0 + int2(1, 1), mask, a_world.z));
			[branch] if (any(max(inner0, inner1) > 0.0))
			{
				const float2 f = tf - t0f;
				const float4 wx = BSpline(f.x);
				const float4 r1 = float4(TexelDepth(t0 + int2(-1, 0), mask, a_world.z), inner0, TexelDepth(t0 + int2(2, 0), mask, a_world.z));
				const float4 r2 = float4(TexelDepth(t0 + int2(-1, 1), mask, a_world.z), inner1, TexelDepth(t0 + int2(2, 1), mask, a_world.z));
				const float4 across = float4(dot(wx, BlockRow(t0, -1, mask, a_world.z)), dot(wx, r1), dot(wx, r2), dot(wx, BlockRow(t0, 2, mask, a_world.z)));
				d = dot(BSpline(f.y), across);
			}
		}
		return d;
	}

	float Share(float3 a_world)
	{
		const float full = SharedData::snowDeformationSettings.PrintFullDepth;
		float share = 0.0;
		[branch] if (full > 0.001)
			share = Depth(a_world) / full;
		return share;
	}

	float Bare(float a_share, float a_noise)
	{
		const float noiseLo = 0.05;
		const float noiseHi = 0.45;
		const float soft = 0.03;
		const float thr = max(lerp(0.85, 0.38, (a_noise - noiseLo) / (noiseHi - noiseLo)), 0.36);
		return smoothstep(thr - soft, thr + soft, a_share) * saturate(4.0 * SharedData::snowDeformationSettings.PrintFullDepth);
	}

	float3 DepthSlope(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int mask = Mask(t0 - 1, 3);
		float3 r = 0.0;
		[branch] if (mask >= 0)
		{
			const float2 f = tf - t0f;
			const float4 wx = BSpline(f.x);
			const float4 sx = BSplineSlope(f.x);
			const float4 r0 = BlockRow(t0, -1, mask, a_world.z);
			const float4 r1 = BlockRow(t0, 0, mask, a_world.z);
			const float4 r2 = BlockRow(t0, 1, mask, a_world.z);
			const float4 r3 = BlockRow(t0, 2, mask, a_world.z);
			const float4 across = float4(dot(wx, r0), dot(wx, r1), dot(wx, r2), dot(wx, r3));
			r.x = dot(BSpline(f.y), across);
			r.y = dot(BSpline(f.y), float4(dot(sx, r0), dot(sx, r1), dot(sx, r2), dot(sx, r3))) / kTexel;
			r.z = dot(BSplineSlope(f.y), across) / kTexel;
		}
		return r;
	}

#ifdef SNOW_PRINT_RIM
	Texture2D<uint> PrintRim : register(t116);
	static const int kRimApron = 5;

	float RimTexel(int2 a_texel, int a_mask, float a_z)
	{
		const uint v = PrintRim.Load(int3(a_texel & a_mask, 0));
		const float z = float(v >> 10) * 0.25 - kZOffset;
		return float(v & 1023u) * saturate(2.0 - abs(a_z - z) / 6.0) * (1.0 / 1023.0);
	}

	float Rim(float3 a_world)
	{
		const float2 tf = a_world.xy / kTexel - 0.5;
		const float2 t0f = floor(tf);
		const int2 t0 = int2(t0f);
		const int4 rect = SharedData::snowDeformationSettings.PrintRect;
		float r = 0.0;
		[branch] if (all(t0 >= rect.xy) && all(t0 + 1 < rect.zw))
		{
			uint3 dims;
			PrintMap.GetDimensions(dims.x, dims.y, dims.z);
			const int m = int(dims.x) - 1;
			[branch] if (PrintTiles.Load(int3(((t0 - kRimApron) >> kTileShift) & (m >> kTileShift), 0)) != 0u)
			{
				const float2 f = tf - t0f;
				r = lerp(lerp(RimTexel(t0, m, a_world.z), RimTexel(t0 + int2(1, 0), m, a_world.z), f.x),
					lerp(RimTexel(t0 + int2(0, 1), m, a_world.z), RimTexel(t0 + int2(1, 1), m, a_world.z), f.x), f.y);
			}
		}
		return r;
	}
#endif

	float DentSunShadow(float3 a_world, float3 L, float2 a_g, float a_dentDepth, float a_strength, float a_noise)
	{
		const float climb = L.z - dot(L.xy, a_g);
		const float h0 = -a_dentDepth * DepthFast(a_world);
		float lit = 1.0;
		[branch] if (climb > 0.01 && h0 < -0.01)
		{
			const float reach = min(-h0 / climb, 16.0);
			float occlusion = 0.0;
			[loop] for (float i = 0.0; i < 4.0; i += 1.0)
			{
				const float s = reach * (i + 0.1 + 0.9 * a_noise) / 4.0;
				const float2 step = L.xy * s;
				const float h = -a_dentDepth * DepthFast(float3(a_world.xy + step, a_world.z + dot(step, a_g)));
				occlusion = max(occlusion, (h - (h0 + climb * s)) / (0.2 * s + 0.05));
			}
			lit = 1.0 - saturate(occlusion * a_strength);
		}
		return lit;
	}
}

#endif
