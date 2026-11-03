// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <emmintrin.h>

namespace TrenchSimd
{
	/** @brief Every byte becomes max(0, byte - a_drop); returns whether any byte stays non-zero. a_drop >= 1. */
	inline bool DecayBytes(uint8_t* a_bytes, size_t a_count, int a_drop)
	{
		const __m128i drop = _mm_set1_epi8((char)(uint8_t)std::min(a_drop, 255));
		__m128i any = _mm_setzero_si128();
		size_t i = 0;
		for (; i + 16 <= a_count; i += 16) {
			const __m128i v = _mm_subs_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes + i)), drop);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(a_bytes + i), v);
			any = _mm_or_si128(any, v);
		}
		bool alive = _mm_movemask_epi8(_mm_cmpeq_epi8(any, _mm_setzero_si128())) != 0xFFFF;
		for (; i < a_count; i++) {
			a_bytes[i] = (uint8_t)std::max(0, (int)a_bytes[i] - a_drop);
			alive = alive || a_bytes[i] != 0;
		}
		return alive;
	}

	/** @brief Non-zero bytes, and pairs of the store's byte RLE (runs of equal bytes, a run longer than 255 split). */
	inline void CountRuns(const uint8_t* a_bytes, size_t a_count, size_t& a_nonZero, size_t& a_pairs)
	{
		size_t zeros = 0;
		size_t pairs = 0;
		size_t start = 0;
		size_t b = 0;
		const __m128i zero = _mm_setzero_si128();
		for (; b + 16 <= a_count; b += 16) {
			const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes + b));
			const __m128i prev = b ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_bytes + b - 1)) :
			                         _mm_or_si128(_mm_slli_si128(v, 1), _mm_cvtsi32_si128(a_bytes[0]));
			uint32_t edges = ~(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, prev)) & 0xFFFFu;
			while (edges) {
				const size_t at = b + (size_t)std::countr_zero(edges);
				edges &= edges - 1;
				pairs += (at - start + 254) / 255;
				start = at;
			}
			zeros += (size_t)std::popcount((uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, zero)));
		}
		for (; b < a_count; b++) {
			if (b && a_bytes[b] != a_bytes[b - 1]) {
				pairs += (b - start + 254) / 255;
				start = b;
			}
			zeros += a_bytes[b] == 0;
		}
		if (a_count)
			pairs += (a_count - start + 254) / 255;
		a_nonZero = a_count - zeros;
		a_pairs = pairs;
	}

	/** @brief True if any of the eight RGBA16F texels at a_texels has a .x that is not negative and not below a_epsilonBits (bit compare). a_epsilonBits <= 0x7FFF. */
	inline bool AnyAtLeast8(const uint16_t* a_texels, uint16_t a_epsilonBits)
	{
		const __m128i bound = _mm_set1_epi16((short)(a_epsilonBits - 1));
		const auto* p = reinterpret_cast<const __m128i*>(a_texels);
		const __m128i hit = _mm_or_si128(
			_mm_or_si128(_mm_cmpgt_epi16(_mm_loadu_si128(p), bound), _mm_cmpgt_epi16(_mm_loadu_si128(p + 1), bound)),
			_mm_or_si128(_mm_cmpgt_epi16(_mm_loadu_si128(p + 2), bound), _mm_cmpgt_epi16(_mm_loadu_si128(p + 3), bound)));
		return (_mm_movemask_epi8(hit) & 0x0101) != 0;
	}
}
