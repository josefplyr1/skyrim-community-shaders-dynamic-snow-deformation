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

	void Install();

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
