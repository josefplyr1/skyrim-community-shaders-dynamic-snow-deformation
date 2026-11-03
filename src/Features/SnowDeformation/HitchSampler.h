#pragma once

namespace SnowHitch
{
	void OnFrame();
	bool Start(uint32_t a_intervalUs);
	json Report(float a_thresholdMs, uint32_t a_top, const std::string& a_module);
}
