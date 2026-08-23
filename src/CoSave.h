#pragma once

/**
 * @brief Community Shaders' SKSE co-save channel.
 *
 * A co-save is a `.skse` file created, copied and deleted WITH the save it
 * belongs to. Features needing per-save state therefore need no file of their
 * own, no cleanup tool, and leave nothing behind when uninstalled: a plugin's
 * save callback is the only source of its bytes, so a build without a feature
 * simply stops writing that feature's record. The `.ess` itself never grows,
 * and save-cleaning tools neither see this data nor need to.
 *
 * Features register one record type each and are dispatched by it. A record
 * whose owner is absent - a feature disabled at boot, or gone from the build -
 * is SKIPPED with a log line rather than mis-read, which is the whole reason
 * for a registry: a shared channel with no per-record ownership cannot tell an
 * unknown record from a corrupt one.
 *
 * Every record carries a version. A loader is handed the version that was
 * written and is expected to refuse anything it does not understand, so an
 * older build meeting a newer save drops that feature's state instead of
 * reading a different struct's bytes.
 *
 * Callbacks arrive on the GAME thread, not the render thread. A feature whose
 * state is touched during rendering must do its own locking.
 */
class CoSave
{
public:
	static CoSave* GetSingleton()
	{
		static CoSave singleton;
		return std::addressof(singleton);
	}

	/** @brief Writes this feature's records. Open each with OpenRecord, then WriteRecordData. */
	using SaveCallback = std::function<void(const SKSE::SerializationInterface*)>;
	/** @brief Reads one record back. Refuse a version this build does not understand rather than reading it. */
	using LoadCallback = std::function<void(const SKSE::SerializationInterface*, uint32_t a_version, uint32_t a_length)>;
	/** @brief Drops in-memory state. Called before a save is loaded AND when a new game starts, so state cannot leak between timelines. */
	using RevertCallback = std::function<void()>;

	/** @brief Plugin-wide co-save ID. Changing it orphans every existing chunk. */
	static constexpr uint32_t kUniqueID = 'CSHD';

	/** @brief Installs the SKSE callbacks. Call once, from the plugin's Load(), before any save can be loaded. */
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
	/** @brief Registration happens on the game thread and dispatch on the game thread, but a feature may register late; the map must not be rehashed under a reader. */
	std::mutex channelMutex;
	bool installed = false;
};
