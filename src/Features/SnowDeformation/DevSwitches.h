#pragma once

namespace SnowDevSwitches
{
	inline std::atomic<bool> decalOverlaySplit{ false };

	inline const char* DecalOverlayRow(const char* a_part)
	{
		return decalOverlaySplit.load(std::memory_order_relaxed) ? a_part : "SnowDeformation::Decals - Overlay";
	}

	inline std::atomic<bool> heightMapSplit{ false };

	inline const char* HeightMapRow(const char* a_part)
	{
		return heightMapSplit.load(std::memory_order_relaxed) ? a_part : "SnowDeformation::Objects - Height Maps";
	}
}
