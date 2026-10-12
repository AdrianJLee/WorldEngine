#pragma once

#include "AudioTypes.h"
#include "AudioBuffer.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>

namespace World::Audio
{
	enum class VoiceEmotion : uint8_t
	{
		Neutral = 0,
		Angry = 1,
		Whisper = 2,
		Sad = 3,
		Happy = 4,
		Fear = 5,
		Heroic = 6
	};

	struct WLD_API TimbreProfile
	{
		std::string characterId;       // e.g. "Old_King", "Young_Knight"
		std::string presetName;        // e.g. "Deep_Old_Male", "Crisp_Female"
		std::string referenceAudioPath; // Optional 3-5s sample for zero-shot cloning
		float pitchShiftSemi { 0.0f }; // -12 to +12 semitones
		float rateScale { 1.0f };       // 0.5x to 2.0x playback speed
		float warmth { 0.5f };          // 0.0 to 1.0
		float roughness { 0.2f };       // 0.0 to 1.0
		std::vector<float> latentEmbedding; // 256-dim acoustic vector if cloned

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);
	};

	struct PhonemeViseme
	{
		float timeSeconds { 0.0f };
		std::string phoneme; // e.g. "AA", "IH", "T", "S", "M"
		float visemeWeight { 1.0f };
	};

	struct TtsSynthesisResult
	{
		bool success { false };
		AudioBuffer audio;
		std::vector<PhonemeViseme> lipSyncTrack;
		std::string debugMessage;
	};

	class WLD_API IAiAudioProvider
	{
	public:
		virtual ~IAiAudioProvider() = default;

		virtual TtsSynthesisResult SynthesizeDialogue(
			const std::string& dialogueText,
			const TimbreProfile& timbre,
			VoiceEmotion emotion,
			float emotionIntensity) = 0;

		virtual AudioBuffer GenerateSfx(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) = 0;

		virtual AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) = 0;
	};

	class WLD_API LocalEdgeOnnxProvider final : public IAiAudioProvider
	{
	public:
		LocalEdgeOnnxProvider() = default;

		TtsSynthesisResult SynthesizeDialogue(
			const std::string& dialogueText,
			const TimbreProfile& timbre,
			VoiceEmotion emotion,
			float emotionIntensity) override;

		AudioBuffer GenerateSfx(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) override;
	};

	class WLD_API CloudApiAudioProvider final : public IAiAudioProvider
	{
	public:
		CloudApiAudioProvider(std::string apiKey, std::string endpoint);

		TtsSynthesisResult SynthesizeDialogue(
			const std::string& dialogueText,
			const TimbreProfile& timbre,
			VoiceEmotion emotion,
			float emotionIntensity) override;

		AudioBuffer GenerateSfx(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) override;

	private:
		std::string m_ApiKey;
		std::string m_Endpoint;
	};

	class WLD_API AiAudioStudioManager
	{
	public:
		AiAudioStudioManager();

		void SetActiveProvider(std::shared_ptr<IAiAudioProvider> provider) { m_Provider = std::move(provider); }
		IAiAudioProvider* GetActiveProvider() const noexcept { return m_Provider.get(); }

		void RegisterTimbre(const TimbreProfile& profile);
		const TimbreProfile* FindTimbre(const std::string& characterId) const;
		std::vector<std::string> GetRegisteredTimbreIds() const;

		TtsSynthesisResult SynthesizeLine(
			const std::string& characterId,
			const std::string& lineText,
			VoiceEmotion emotion = VoiceEmotion::Neutral,
			float emotionIntensity = 0.5f);

		AudioBuffer GenerateSfxCues(
			const std::string& prompt,
			float durationSeconds = 1.5f,
			uint32_t variationCount = 4);

		AudioBuffer GenerateMusicTrack(
			const std::string& moodPrompt,
			float bpm = 120.0f,
			float durationSeconds = 30.0f);

		// Batch dialogue generator for quest scripts
		struct BatchDialogueEntry
		{
			std::string lineId;
			std::string characterId;
			std::string text;
			VoiceEmotion emotion { VoiceEmotion::Neutral };
		};
		std::vector<TtsSynthesisResult> BatchSynthesize(const std::vector<BatchDialogueEntry>& entries);

	private:
		std::shared_ptr<IAiAudioProvider> m_Provider;
		std::unordered_map<std::string, TimbreProfile> m_Timbres;
	};
}
