#pragma once

#include "AudioTypes.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World::Audio
{
	enum class ContainerType : uint8_t
	{
		Single = 0,
		Random = 1,
		Sequence = 2,
		Switch = 3,
		Blend = 4,
		Layer = 5
	};

	struct EvaluatedSoundTrack
	{
		std::string soundAssetPath;
		float volumeScale { 1.0f };
		float delaySeconds { 0.0f };
	};

	struct SoundVariation
	{
		std::string soundAssetPath;
		float weight { 1.0f };
	};

	struct SoundContainerNode
	{
		ContainerType type { ContainerType::Single };
		std::vector<SoundVariation> variations;
		bool shuffleNoRepeat { true };
		std::string switchParamName;
		std::unordered_map<std::string, std::shared_ptr<SoundContainerNode>> switchBranches;

		// Blend parameters
		std::string blendParamName;
		std::vector<std::pair<float, std::shared_ptr<SoundContainerNode>>> blendPoints;

		// Layer parameters (ContainerType::Layer): multiple simultaneous sub-nodes
		std::vector<std::pair<float, std::shared_ptr<SoundContainerNode>>> layerChildren;

		// Internal state for playback
		uint32_t lastChosenIndex { 0xFFFFFFFFu };
		uint32_t sequenceIndex { 0 };

		std::string EvaluateSelection(
			const std::unordered_map<std::string, std::string>& switchParams,
			const std::unordered_map<std::string, float>& blendParams);

		std::vector<EvaluatedSoundTrack> EvaluateTracks(
			const std::unordered_map<std::string, std::string>& switchParams,
			const std::unordered_map<std::string, float>& blendParams);
	};

	struct SoundSpatialDesc
	{
		bool is3D { false };
		SpatialMode mode { SpatialMode::None };
		AttenuationModel model { AttenuationModel::InverseSquare };
		float minDistance { 1.0f };
		float maxDistance { 25.0f };
		float innerAngle { 360.0f };
		float outerAngle { 360.0f };
		float outerGain { 1.0f };
		float halfViewWidth { 10.0f };
		float maxPan { 0.75f };
	};

	class WLD_API SoundEvent
	{
	public:
		SoundEvent() = default;
		explicit SoundEvent(std::string id, BusId busId = kBusSfx);

		const std::string& GetId() const noexcept { return m_Id; }
		BusId GetBusId() const noexcept { return m_BusId; }
		void SetBusId(BusId busId) noexcept { m_BusId = busId; }

		uint32_t GetMaxInstances() const noexcept { return m_MaxInstances; }
		void SetMaxInstances(uint32_t maxInst) noexcept { m_MaxInstances = maxInst; }

		StealPolicy GetStealPolicy() const noexcept { return m_StealPolicy; }
		void SetStealPolicy(StealPolicy policy) noexcept { m_StealPolicy = policy; }

		float GetCooldownSeconds() const noexcept { return m_CooldownSeconds; }
		void SetCooldownSeconds(float sec) noexcept { m_CooldownSeconds = sec; }

		void SetVolumeRange(float minVol, float maxVol) noexcept { m_MinVolume = minVol; m_MaxVolume = maxVol; }
		void SetPitchRangeSemitones(float minSemi, float maxSemi) noexcept { m_MinPitchSemitones = minSemi; m_MaxPitchSemitones = maxSemi; }

		float EvaluateRandomVolume() const noexcept;
		float EvaluateRandomPitch() const noexcept;

		SoundSpatialDesc& GetSpatial() noexcept { return m_Spatial; }
		const SoundSpatialDesc& GetSpatial() const noexcept { return m_Spatial; }

		std::shared_ptr<SoundContainerNode>& GetRootContainer() noexcept { return m_RootContainer; }
		const std::shared_ptr<SoundContainerNode>& GetRootContainer() const noexcept { return m_RootContainer; }

		std::vector<EvaluatedSoundTrack> EvaluateTracks(
			const std::unordered_map<std::string, std::string>& switchParams,
			const std::unordered_map<std::string, float>& blendParams) const;

		// Serialization / Deserialization from YAML
		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yamlText);

	private:
		std::string m_Id { "Default_Event" };
		BusId m_BusId { kBusSfx };
		uint32_t m_MaxInstances { 8 };
		StealPolicy m_StealPolicy { StealPolicy::StealOldest };
		float m_CooldownSeconds { 0.05f };

		float m_MinVolume { 1.0f };
		float m_MaxVolume { 1.0f };
		float m_MinPitchSemitones { 0.0f };
		float m_MaxPitchSemitones { 0.0f };

		SoundSpatialDesc m_Spatial;
		std::shared_ptr<SoundContainerNode> m_RootContainer;
	};
}
