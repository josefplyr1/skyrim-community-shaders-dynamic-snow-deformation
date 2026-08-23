#include "CoSave.h"

namespace
{
	// Four-character codes read back to front in memory; spell them out for the log.
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
	for (auto& [type, channel] : self->channels) {
		if (channel.save)
			channel.save(a_intfc);
	}
}

void CoSave::OnGameLoaded(SKSE::SerializationInterface* a_intfc)
{
	auto* self = GetSingleton();
	std::scoped_lock lock(self->channelMutex);

	uint32_t type = 0;
	uint32_t version = 0;
	uint32_t length = 0;
	while (a_intfc->GetNextRecordInfo(type, version, length)) {
		auto found = self->channels.find(type);
		if (found == self->channels.end()) {
			// A feature disabled at boot, or one this build does not have. The
			// record is skipped rather than guessed at - reading someone
			// else's bytes is worse than losing state.
			logger::info("[COSAVE] Record '{}' has no owner in this build; skipped ({} bytes)", RecordName(type), length);
			continue;
		}
		if (!found->second.load)
			continue;
		found->second.load(a_intfc, version, length);
	}
}

void CoSave::OnRevert(SKSE::SerializationInterface*)
{
	// Fires before a save is loaded AND on a new game. Everything must go, or a
	// fresh game inherits the last one's state.
	auto* self = GetSingleton();
	std::scoped_lock lock(self->channelMutex);
	for (auto& [type, channel] : self->channels) {
		if (channel.revert)
			channel.revert();
	}
}
