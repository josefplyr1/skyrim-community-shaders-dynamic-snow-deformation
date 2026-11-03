// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#pragma once

class CoSave
{
public:
	static CoSave* GetSingleton()
	{
		static CoSave singleton;
		return std::addressof(singleton);
	}

	using SaveCallback = std::function<void(const SKSE::SerializationInterface*)>;
	using LoadCallback = std::function<void(const SKSE::SerializationInterface*, uint32_t a_version, uint32_t a_length)>;
	using RevertCallback = std::function<void()>;

	static constexpr uint32_t kUniqueID = 'CSHD';

	/** @brief Installs the SKSE callbacks. Idempotent; called from the feature's PostPostLoad, before any save can be loaded. */
	void Install();

	/**
	 * @brief Claims a record type for a feature.
	 * @param a_recordType Four-character code, unique across features.
	 * @param a_version Version written into the record. Bump it when the payload layout changes.
	 * Registration must happen before the main menu; PostPostLoad is early enough.
	 */
	void Register(uint32_t a_recordType, uint32_t a_version, SaveCallback a_save, LoadCallback a_load, RevertCallback a_revert);

private:
	struct Channel
	{
		uint32_t version;
		SaveCallback save;
		LoadCallback load;
		RevertCallback revert;
	};

	static void OnGameSaved(SKSE::SerializationInterface* a_intfc);
	static void OnGameLoaded(SKSE::SerializationInterface* a_intfc);
	static void OnRevert(SKSE::SerializationInterface* a_intfc);

	std::unordered_map<uint32_t, Channel> channels;
	std::mutex channelMutex;
	bool installed = false;
};
