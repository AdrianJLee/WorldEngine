#pragma once

#include "AudioTypes.h"
#include "AudioDsp.h"
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <unordered_map>

namespace World::Audio
{
	

	struct BusEffectRack
	{
		bool enableEq { false };
		BiquadFilter eqBands[5]; // LowShelf, LowMid, Mid, HighMid, HighShelf

		bool enableLowPass { false };
		BiquadFilter lowPass;

		bool enableCompressor { false };
		DynamicsCompressor compressor;
	};

	class WLD_API MixerBus
	{
	public:
		MixerBus() = default;
		MixerBus(BusId id, std::string name, BusId parentId = kInvalidBusId);
		MixerBus(const MixerBus& other)
			: m_Id(other.m_Id), m_Name(other.m_Name), m_ParentId(other.m_ParentId),
			  m_Volume(other.m_Volume), m_Mute(other.m_Mute), m_Solo(other.m_Solo),
			  m_DuckingFactor(other.m_DuckingFactor), m_Effects(other.m_Effects),
			  m_LeftPeakDb(other.m_LeftPeakDb.load()), m_RightPeakDb(other.m_RightPeakDb.load()),
			  m_LeftRmsDb(other.m_LeftRmsDb.load()), m_RightRmsDb(other.m_RightRmsDb.load())
		{}
		MixerBus& operator=(const MixerBus& other)
		{
			if (this != &other)
			{
				m_Id = other.m_Id;
				m_Name = other.m_Name;
				m_ParentId = other.m_ParentId;
				m_Volume = other.m_Volume;
				m_Mute = other.m_Mute;
				m_Solo = other.m_Solo;
				m_DuckingFactor = other.m_DuckingFactor;
				m_Effects = other.m_Effects;
				m_LeftPeakDb.store(other.m_LeftPeakDb.load());
				m_RightPeakDb.store(other.m_RightPeakDb.load());
				m_LeftRmsDb.store(other.m_LeftRmsDb.load());
				m_RightRmsDb.store(other.m_RightRmsDb.load());
			}
			return *this;
		}

		BusId GetId() const noexcept { return m_Id; }
		const std::string& GetName() const noexcept { return m_Name; }
		BusId GetParentId() const noexcept { return m_ParentId; }

		void SetVolume(float volume) noexcept { m_Volume = std::clamp(volume, 0.0f, 4.0f); }
		float GetVolume() const noexcept { return m_Volume; }

		void SetMute(bool mute) noexcept { m_Mute = mute; }
		bool IsMute() const noexcept { return m_Mute; }

		void SetSolo(bool solo) noexcept { m_Solo = solo; }
		bool IsSolo() const noexcept { return m_Solo; }

		void SetDuckingFactor(float duck) noexcept { m_DuckingFactor = std::clamp(duck, 0.0f, 1.0f); }
		float GetDuckingFactor() const noexcept { return m_DuckingFactor; }

		BusEffectRack& GetEffectRack() noexcept { return m_Effects; }

		void UpdateMeter(float lPeak, float rPeak, float lRms, float rRms) noexcept;
		StereoMeter GetMeter() const noexcept;

		void ProcessEffects(float* left, float* right, uint32_t frameCount) noexcept;

	private:
		BusId m_Id { kBusMaster };
		std::string m_Name { "Master" };
		BusId m_ParentId { kInvalidBusId };

		float m_Volume { 1.0f };
		bool m_Mute { false };
		bool m_Solo { false };
		float m_DuckingFactor { 1.0f };

		BusEffectRack m_Effects;

		// Atomic double-buffered metering in dBFS
		std::atomic<float> m_LeftPeakDb { -96.0f };
		std::atomic<float> m_RightPeakDb { -96.0f };
		std::atomic<float> m_LeftRmsDb { -96.0f };
		std::atomic<float> m_RightRmsDb { -96.0f };
	};

	struct MixerSnapshot
	{
		std::string name;
		std::unordered_map<BusId, float> busVolumes;
		std::unordered_map<BusId, float> busLowPassCutoff;
	};

	class WLD_API MixerGraph
	{
	public:
		MixerGraph();

		void ResetToDefaults();

		void AddBus(BusId id, std::string name, BusId parentId = kBusMaster);
		MixerBus* FindBus(BusId id);
		const MixerBus* FindBus(BusId id) const;
		const std::vector<BusId>& GetBusOrder() const noexcept { return m_BusOrder; }
		const std::unordered_map<BusId, MixerBus>& GetBuses() const noexcept { return m_Buses; }

		void SetBusVolume(BusId id, float volume);
		void SetBusMute(BusId id, bool mute);
		void SetBusSolo(BusId id, bool solo);

		void RegisterSnapshot(const MixerSnapshot& snapshot);
		void TriggerSnapshot(const std::string& name, float transitionSeconds = 0.3f);
		void ClearSnapshot(float transitionSeconds = 0.3f);

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);

		void ProcessGraph(
			std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>>& busBuffers,
			float* finalMasterLeft,
			float* finalMasterRight,
			uint32_t frameCount,
			float sampleRate);

	private:
		std::unordered_map<BusId, MixerBus> m_Buses;
		std::vector<BusId> m_BusOrder; // Topological order (leaves to root)
		std::unordered_map<std::string, MixerSnapshot> m_Snapshots;

		// Snapshot crossfade state
		bool m_SnapshotActive { false };
		MixerSnapshot m_CurrentSnapshot;
		float m_SnapshotBlend { 0.0f }; // 0.0 -> 1.0
		float m_SnapshotTargetBlend { 0.0f };
		float m_SnapshotSpeed { 1.0f };

		// Master Limiter
		TruePeakLimiter m_MasterLimiter;

		// Sidechain Ducking detector state
		float m_VoiceRmsSmoothed { 0.0f };
	};
}
