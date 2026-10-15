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

	enum class AiStudioProviderMode : uint8_t
	{
		LocalProcedural = 0,
		SidecarService = 1,
		CloudApi = 2
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

		bool SaveToFile(const std::string& filepath) const;
		bool LoadFromFile(const std::string& filepath);
		static bool ExtractAcousticFeatures(const AudioBuffer& sample, std::vector<float>& outEmbedding);
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

	struct SfxVariation
	{
		uint32_t variationIndex { 1 };
		std::string name;
		AudioBuffer audio;
		float durationSeconds { 0.0f };
		float peakDb { 0.0f };
		float lufs { -16.0f };
	};

	struct BgmLoopPoints
	{
		float startSeconds { 0.0f };
		float endSeconds { 0.0f };
		float confidence { 0.0f };
		bool valid { false };
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

		virtual std::vector<SfxVariation> GenerateSfxMulti(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) = 0;

		virtual AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) = 0;

		virtual BgmLoopPoints DetectBgmLoop(
			const AudioBuffer& buf,
			float minSec,
			float maxSec) = 0;

		virtual bool ExtractCloneEmbedding(
			const AudioBuffer& refWav,
			std::vector<float>& outEmbedding) = 0;
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

		std::vector<SfxVariation> GenerateSfxMulti(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) override;

		BgmLoopPoints DetectBgmLoop(
			const AudioBuffer& buf,
			float minSec,
			float maxSec) override;

		bool ExtractCloneEmbedding(
			const AudioBuffer& refWav,
			std::vector<float>& outEmbedding) override;
	};

	class WLD_API SidecarAudioProvider final : public IAiAudioProvider
	{
	public:
		SidecarAudioProvider(std::string host = "127.0.0.1", uint16_t port = 8090);

		bool ProbeHealth();
		bool IsOnline() const noexcept { return m_IsOnline; }

		TtsSynthesisResult SynthesizeDialogue(
			const std::string& dialogueText,
			const TimbreProfile& timbre,
			VoiceEmotion emotion,
			float emotionIntensity) override;

		AudioBuffer GenerateSfx(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		std::vector<SfxVariation> GenerateSfxMulti(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) override;

		BgmLoopPoints DetectBgmLoop(
			const AudioBuffer& buf,
			float minSec,
			float maxSec) override;

		bool ExtractCloneEmbedding(
			const AudioBuffer& refWav,
			std::vector<float>& outEmbedding) override;

	private:
		std::string m_Host;
		uint16_t m_Port;
		bool m_IsOnline { false };
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

		std::vector<SfxVariation> GenerateSfxMulti(
			const std::string& prompt,
			float durationSeconds,
			uint32_t variationCount) override;

		AudioBuffer GenerateMusic(
			const std::string& moodPrompt,
			float bpm,
			float durationSeconds) override;

		BgmLoopPoints DetectBgmLoop(
			const AudioBuffer& buf,
			float minSec,
			float maxSec) override;

		bool ExtractCloneEmbedding(
			const AudioBuffer& refWav,
			std::vector<float>& outEmbedding) override;

	private:
		std::string m_ApiKey;
		std::string m_Endpoint;
	};

	class WLD_API AudioCurationPipeline
	{
	public:
		static bool TrimSilence(AudioBuffer& buf, float thresholdDb = -45.0f);
		static bool ZeroCrossingFadeout(AudioBuffer& buf, uint32_t fadeFrames = 64);
		static bool NormalizeLoudness(AudioBuffer& buf, float targetLufs = -16.0f);
		static bool AutoCurate(AudioBuffer& buf, float targetLufs = -16.0f);
	};

	class WLD_API PhonemeDictionary
	{
	public:
		static bool LoadDictionary(const std::string& filepath = "assets/audio/voices/phoneme_dict.yaml");
		static bool SaveDictionary(const std::string& filepath = "assets/audio/voices/phoneme_dict.yaml");
		static void SetWordOverride(const std::string& word, const std::string& pinyin);
		static std::string GetWordOverride(const std::string& word);
		static const std::unordered_map<std::string, std::string>& GetAllOverrides();

		static std::string ParsePronunciationMarkup(const std::string& markupText);
		static void ExtractEmotionAndPauses(
			const std::string& input,
			std::string& outCleanText,
			VoiceEmotion& outEmotion,
			float& outEmotionIntensity,
			float& outPauseSeconds);
	};

	class WLD_API LipSyncTrajectory
	{
	public:
		static bool SaveWlip(const std::string& filepath, const std::vector<PhonemeViseme>& track);
		static bool LoadWlip(const std::string& filepath, std::vector<PhonemeViseme>& outTrack);
		static std::unordered_map<std::string, float> EvaluateBlendShapes(const std::vector<PhonemeViseme>& track, float timeSeconds);
	};

	class WLD_API AiAudioStudioManager
	{
	public:
		AiAudioStudioManager();

		void SetActiveProvider(std::shared_ptr<IAiAudioProvider> provider) { m_Provider = std::move(provider); }
		IAiAudioProvider* GetActiveProvider() const noexcept { return m_Provider.get(); }

		AiStudioProviderMode ProbeBackend(const std::string& host = "127.0.0.1", uint16_t port = 8090);
		AiStudioProviderMode GetCurrentMode() const noexcept { return m_CurrentMode; }
		void SetMode(AiStudioProviderMode mode);
		bool IsSidecarAlive() const noexcept { return m_IsSidecarAlive; }

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

		std::vector<SfxVariation> GenerateSfxMulti(
			const std::string& prompt,
			float durationSeconds = 1.5f,
			uint32_t variationCount = 4);

		AudioBuffer GenerateMusicTrack(
			const std::string& moodPrompt,
			float bpm = 120.0f,
			float durationSeconds = 30.0f);

		BgmLoopPoints DetectBgmLoop(const AudioBuffer& buf, float minSec = 1.0f, float maxSec = 60.0f);
		bool CloneTimbreFromWav(const std::string& characterId, const AudioBuffer& wav, TimbreProfile& outProfile);

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
		std::shared_ptr<LocalEdgeOnnxProvider> m_LocalProvider;
		std::shared_ptr<SidecarAudioProvider> m_SidecarProvider;
		AiStudioProviderMode m_CurrentMode { AiStudioProviderMode::LocalProcedural };
		bool m_IsSidecarAlive { false };
		std::unordered_map<std::string, TimbreProfile> m_Timbres;
	};
}
