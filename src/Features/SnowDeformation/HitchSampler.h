#pragma once

namespace SnowHitch
{
	/** @brief Once a frame on the render thread: a frame boundary, and the thread to watch. */
	void OnFrame();
	/** @brief Starts sampling every a_intervalUs microseconds; false when it cannot. */
	bool Start(uint32_t a_intervalUs);
	/** @brief Stops sampling and reports frames over a_thresholdMs against the rest, a_top rows a table. A non-empty a_module also charges each sample to that module: its own code at the leaf, else the nearest return address into it. */
	json Report(float a_thresholdMs, uint32_t a_top, const std::string& a_module);
}
