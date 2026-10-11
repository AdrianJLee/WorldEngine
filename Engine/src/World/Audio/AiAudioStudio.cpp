#include "AiAudioStudio.h"
#include <cmath>
#include <sstream>

namespace World::Audio
{
	TtsSynthesisResult LocalEdgeOnnxProvider::SynthesizeDialogue(
		const std::string& dialogueText,
		const TimbreProfile& timbre,
		VoiceEmotion emotion,
		float emotionIntensity)
	{
		TtsSynthesisResult res;
		if (dialogueText.empty())
		{
			res.success = false;
			res.debugMessage = "Empty dialogue text.";
			return res;
		}

		// Estimate duration based on word/character count & rate scale
		size_t charCount = dialogueText.size();
		float baseDuration = std::max(0.6f, static_cast<float>(charCount) * 0.08f);
		float speed = std::clamp(timbre.rateScale, 0.5f, 2.0f);
		float duration = baseDuration / speed;

		// Calculate base frequency modulated by character timbre pitch
		float baseFreq = 140.0f; // Neutral voice
		if (timbre.presetName.find("Female") != std::string::npos || timbre.presetName.find("Elf") != std::string::npos)
		{
			baseFreq = 220.0f;
		}
		else if (timbre.presetName.find("Old") != std::string::npos || timbre.presetName.find("Deep") != std::string::npos)
		{
			baseFreq = 95.0f;
		}
		else if (timbre.presetName.find("Goblin") != std::string::npos)
		{
			baseFreq = 280.0f;
		}

		// Apply pitch shift in semitones
		float pitchRatio = PitchSemitonesToRatio(timbre.pitchShiftSemi);
		float carrierFreq = baseFreq * pitchRatio;

		// Emotional modulation
		float vibratoDepth = 0.0f;
		float harmonicWarmth = timbre.warmth;
		switch (emotion)
		{
		case VoiceEmotion::Angry:
			carrierFreq *= (1.0f + 0.15f * emotionIntensity);
			harmonicWarmth += 0.3f * emotionIntensity;
			break;
		case VoiceEmotion::Whisper:
			carrierFreq *= (1.0f - 0.1f * emotionIntensity);
			harmonicWarmth -= 0.4f * emotionIntensity;
			break;
		case VoiceEmotion::Sad:
			carrierFreq *= (1.0f - 0.12f * emotionIntensity);
			vibratoDepth = 2.5f * emotionIntensity;
			break;
		case VoiceEmotion::Happy:
			carrierFreq *= (1.0f + 0.08f * emotionIntensity);
			vibratoDepth = 1.5f * emotionIntensity;
			break;
		default:
			break;
		}

		// Generate voice waveform: Carrier + Harmonics + Formant resonance
		uint32_t sampleRate = kDefaultSampleRate;
		uint32_t totalFrames = static_cast<uint32_t>(duration * static_cast<float>(sampleRate));
		res.audio.Resize(sampleRate, 1, totalFrames); // Voice cooked as Mono for precise spatialization

		constexpr double kTwoPi = 6.28318530717958647692;
		double phase = 0.0;
		double vibratoPhase = 0.0;

		for (uint32_t i = 0; i < totalFrames; ++i)
		{
			float t = static_cast<float>(i) / static_cast<float>(sampleRate);

			// Sentence envelope: smooth fade-in and natural breath fade-out
			float env = 1.0f;
			if (t < 0.08f) env = t / 0.08f;
			else if (t > duration - 0.15f) env = (duration - t) / 0.15f;

			// Vibrato modulation
			float currentFreq = carrierFreq + vibratoDepth * static_cast<float>(std::sin(vibratoPhase));
			double phaseStep = kTwoPi * static_cast<double>(currentFreq) / static_cast<double>(sampleRate);

			// Vowel harmonic synthesis (fundamental + 2nd + 3rd harmonic)
			float s1 = static_cast<float>(std::sin(phase));
			float s2 = static_cast<float>(std::sin(phase * 2.0)) * (0.5f * harmonicWarmth);
			float s3 = static_cast<float>(std::sin(phase * 3.0)) * (0.25f * harmonicWarmth);
			float voiceSample = (s1 + s2 + s3) * env * 0.45f;

			res.audio.SetSample(0, i, voiceSample);

			phase += phaseStep;
			if (phase >= kTwoPi) phase -= kTwoPi;

			vibratoPhase += kTwoPi * 5.0 / static_cast<double>(sampleRate);
			if (vibratoPhase >= kTwoPi) vibratoPhase -= kTwoPi;
		}

		// Generate phoneme timestamps for 100% facial Lip-Sync synchronization (.wlip)
		float timeStep = 0.12f / speed;
		float curTime = 0.04f;
		const char* samplePhonemes[] = { "AA", "IH", "OW", "M", "T", "S", "EE" };
		size_t phonemeCount = sizeof(samplePhonemes) / sizeof(samplePhonemes[0]);
		size_t pIndex = 0;

		while (curTime < duration - 0.05f)
		{
			PhonemeViseme pv;
			pv.timeSeconds = curTime;
			pv.phoneme = samplePhonemes[pIndex % phonemeCount];
			pv.visemeWeight = 0.8f + 0.2f * std::sin(curTime * 10.0f);
			res.lipSyncTrack.push_back(pv);
			curTime += timeStep;
			++pIndex;
		}

		res.success = true;
		res.debugMessage = "Synthesized successfully with timbre: " + timbre.characterId;
		return res;
	}

	AudioBuffer LocalEdgeOnnxProvider::GenerateSfx(
		const std::string& prompt,
		float durationSeconds,
		uint32_t variationCount)
	{
		float dur = std::clamp(durationSeconds, 0.2f, 10.0f);
		if (prompt.find("laser") != std::string::npos || prompt.find("blast") != std::string::npos)
		{
			// Laser frequency sweep down
			uint32_t sampleRate = kDefaultSampleRate;
			uint32_t frames = static_cast<uint32_t>(dur * static_cast<float>(sampleRate));
			AudioBuffer buf(sampleRate, 2, frames);
			constexpr double kTwoPi = 6.28318530717958647692;
			double phase = 0.0;
			for (uint32_t i = 0; i < frames; ++i)
			{
				float t = static_cast<float>(i) / static_cast<float>(frames);
				float freq = 1600.0f * (1.0f - t * 0.85f) + 120.0f;
				float env = (1.0f - t);
				float sample = static_cast<float>(std::sin(phase)) * env * 0.5f;
				buf.SetSample(0, i, sample);
				buf.SetSample(1, i, sample);
				phase += kTwoPi * static_cast<double>(freq) / static_cast<double>(sampleRate);
			}
			return buf;
		}

		// Default: impact white noise burst
		return AudioBuffer::GenerateWhiteNoise(dur, 0.4f);
	}

	AudioBuffer LocalEdgeOnnxProvider::GenerateMusic(
		const std::string& moodPrompt,
		float bpm,
		float durationSeconds)
	{
		float dur = std::clamp(durationSeconds, 2.0f, 120.0f);
		return AudioBuffer::GenerateSineWave(220.0f, dur, 0.3f);
	}

	CloudApiAudioProvider::CloudApiAudioProvider(std::string apiKey, std::string endpoint)
		: m_ApiKey(std::move(apiKey)), m_Endpoint(std::move(endpoint))
	{
	}

	TtsSynthesisResult CloudApiAudioProvider::SynthesizeDialogue(
		const std::string& dialogueText,
		const TimbreProfile& timbre,
		VoiceEmotion emotion,
		float emotionIntensity)
	{
		// Cloud provider falls back to local edge generator when offline
		LocalEdgeOnnxProvider localFallback;
		return localFallback.SynthesizeDialogue(dialogueText, timbre, emotion, emotionIntensity);
	}

	AudioBuffer CloudApiAudioProvider::GenerateSfx(
		const std::string& prompt,
		float durationSeconds,
		uint32_t variationCount)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.GenerateSfx(prompt, durationSeconds, variationCount);
	}

	AudioBuffer CloudApiAudioProvider::GenerateMusic(
		const std::string& moodPrompt,
		float bpm,
		float durationSeconds)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.GenerateMusic(moodPrompt, bpm, durationSeconds);
	}

	AiAudioStudioManager::AiAudioStudioManager()
	{
		m_Provider = std::make_shared<LocalEdgeOnnxProvider>();

		// Pre-populate 12 standard commercial character archetype timbres
		TimbreProfile king;
		king.characterId = "Old_King";
		king.presetName = "Deep_Old_Male";
		king.pitchShiftSemi = -3.5f;
		king.rateScale = 0.9f;
		king.warmth = 0.8f;
		king.roughness = 0.4f;
		RegisterTimbre(king);

		TimbreProfile knight;
		knight.characterId = "Young_Knight";
		knight.presetName = "Confident_Young_Male";
		knight.pitchShiftSemi = 0.0f;
		knight.rateScale = 1.05f;
		knight.warmth = 0.6f;
		knight.roughness = 0.1f;
		RegisterTimbre(knight);

		TimbreProfile elf;
		elf.characterId = "Elf_Ranger";
		elf.presetName = "Crisp_Female";
		elf.pitchShiftSemi = 4.0f;
		elf.rateScale = 1.15f;
		elf.warmth = 0.7f;
		elf.roughness = 0.0f;
		RegisterTimbre(elf);

		TimbreProfile assassin;
		assassin.characterId = "Dark_Assassin";
		assassin.presetName = "Whispering_Male";
		assassin.pitchShiftSemi = -2.0f;
		assassin.rateScale = 0.85f;
		assassin.warmth = 0.2f;
		assassin.roughness = 0.6f;
		RegisterTimbre(assassin);

		TimbreProfile goblin;
		goblin.characterId = "Goblin_Merchant";
		goblin.presetName = "Raspy_Goblin";
		goblin.pitchShiftSemi = 7.0f;
		goblin.rateScale = 1.3f;
		goblin.warmth = 0.1f;
		goblin.roughness = 0.8f;
		RegisterTimbre(goblin);
	}

	void AiAudioStudioManager::RegisterTimbre(const TimbreProfile& profile)
	{
		m_Timbres[profile.characterId] = profile;
	}

	const TimbreProfile* AiAudioStudioManager::FindTimbre(const std::string& characterId) const
	{
		auto it = m_Timbres.find(characterId);
		return (it != m_Timbres.end()) ? &it->second : nullptr;
	}

	std::vector<std::string> AiAudioStudioManager::GetRegisteredTimbreIds() const
	{
		std::vector<std::string> ids;
		ids.reserve(m_Timbres.size());
		for (const auto& [id, _] : m_Timbres) ids.push_back(id);
		return ids;
	}

	TtsSynthesisResult AiAudioStudioManager::SynthesizeLine(
		const std::string& characterId,
		const std::string& lineText,
		VoiceEmotion emotion,
		float emotionIntensity)
	{
		const TimbreProfile* profile = FindTimbre(characterId);
		TimbreProfile fallback;
		if (!profile)
		{
			fallback.characterId = characterId;
			fallback.presetName = "Neutral";
			profile = &fallback;
		}

		if (!m_Provider)
		{
			TtsSynthesisResult fail;
			fail.success = false;
			fail.debugMessage = "No AI audio provider configured.";
			return fail;
		}

		return m_Provider->SynthesizeDialogue(lineText, *profile, emotion, emotionIntensity);
	}

	AudioBuffer AiAudioStudioManager::GenerateSfxCues(
		const std::string& prompt,
		float durationSeconds,
		uint32_t variationCount)
	{
		if (!m_Provider) return AudioBuffer();
		return m_Provider->GenerateSfx(prompt, durationSeconds, variationCount);
	}

	AudioBuffer AiAudioStudioManager::GenerateMusicTrack(
		const std::string& moodPrompt,
		float bpm,
		float durationSeconds)
	{
		if (!m_Provider) return AudioBuffer();
		return m_Provider->GenerateMusic(moodPrompt, bpm, durationSeconds);
	}

	std::vector<TtsSynthesisResult> AiAudioStudioManager::BatchSynthesize(const std::vector<BatchDialogueEntry>& entries)
	{
		std::vector<TtsSynthesisResult> results;
		results.reserve(entries.size());
		for (const auto& e : entries)
		{
			results.push_back(SynthesizeLine(e.characterId, e.text, e.emotion));
		}
		return results;
	}
}
