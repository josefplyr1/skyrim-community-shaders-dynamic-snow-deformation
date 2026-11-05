#include "Features/SnowDeformation/CoSave.h"

namespace
{
	std::string RecordName(uint32_t a_type)
	{
		char name[5] = {};
		name[0] = (char)((a_type >> 24) & 0xFF);
		name[1] = (char)((a_type >> 16) & 0xFF);
		name[2] = (char)((a_type >> 8) & 0xFF);
		name[3] = (char)(a_type & 0xFF);
		return name;
	}
}

void CoSave::Install()
{
	if (installed)
		return;

	auto* serialization = SKSE::GetSerializationInterface();
	if (!serialization) {
		logger::warn("[COSAVE] No serialization interface; per-save state will not persist");
		return;
	}

	serialization->SetUniqueID(kUniqueID);
	serialization->SetSaveCallback(OnGameSaved);
	serialization->SetLoadCallback(OnGameLoaded);
	serialization->SetRevertCallback(OnRevert);
	installed = true;
	logger::info("[COSAVE] Channel installed");
}

void CoSave::Register(uint32_t a_recordType, uint32_t a_version, SaveCallback a_save, LoadCallback a_load, RevertCallback a_revert)
{
	std::scoped_lock lock(channelMutex);
	if (channels.contains(a_recordType)) {
		logger::error("[COSAVE] Record '{}' claimed twice; the second registration is ignored", RecordName(a_recordType));
		return;
	}
	channels.emplace(a_recordType, Channel{ a_version, std::move(a_save), std::move(a_load), std::move(a_revert) });
	logger::info("[COSAVE] Record '{}' registered at version {}", RecordName(a_recordType), a_version);
}

void CoSave::OnGameSaved(SKSE::SerializationInterface* a_intfc)
{
	auto* self = GetSingleton();
	std::scoped_lock lock(self->channelMutex);
	logger::info("[COSAVE] Save callback entered with {} registered channel(s)", self->channels.size());
	for (auto& [type, channel] : self->channels) {
		if (channel.save)
			channel.save(a_intfc);
	}
}

void CoSave::OnGameLoaded(SKSE::SerializationInterface* a_intfc)
{
	auto* self = GetSingleton();
	std::scoped_lock lock(self->channelMutex);

	logger::info("[COSAVE] Load callback entered with {} registered channel(s)", self->channels.size());

	uint32_t type = 0;
	uint32_t version = 0;
	uint32_t length = 0;
	uint32_t seen = 0;
	while (a_intfc->GetNextRecordInfo(type, version, length)) {
		seen++;
		auto found = self->channels.find(type);
		if (found == self->channels.end()) {
			logger::info("[COSAVE] Record '{}' has no owner in this build; skipped ({} bytes)", RecordName(type), length);
			continue;
		}
		if (!found->second.load)
			continue;
		logger::info("[COSAVE] Dispatching record '{}' version {} ({} bytes)", RecordName(type), version, length);
		found->second.load(a_intfc, version, length);
	}

	logger::info("[COSAVE] Load callback saw {} record(s)", seen);
}

void CoSave::OnRevert(SKSE::SerializationInterface*)
{
	auto* self = GetSingleton();
	std::scoped_lock lock(self->channelMutex);
	logger::info("[COSAVE] Revert callback entered");
	for (auto& [type, channel] : self->channels) {
		if (channel.revert)
			channel.revert();
	}
}
