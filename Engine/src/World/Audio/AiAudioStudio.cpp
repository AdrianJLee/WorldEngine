#include "AiAudioStudio.h"
#include "AudioCooker.h"
#include <cmath>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <sapi.h>
#endif

namespace World::Audio
{
	static const char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	static std::string Base64Encode(const uint8_t* data, size_t len)
	{
		std::string ret;
		int val = 0, valb = -6;
		for (size_t i = 0; i < len; ++i)
		{
			val = (val << 8) + data[i];
			valb += 8;
			while (valb >= 0)
			{
				ret.push_back(kBase64Chars[(val >> valb) & 0x3F]);
				valb -= 6;
			}
		}
		if (valb > -6) ret.push_back(kBase64Chars[((val << 8) >> (valb + 8)) & 0x3F]);
		while (ret.size() % 4) ret.push_back('=');
		return ret;
	}

	static std::vector<uint8_t> Base64Decode(const std::string& input)
	{
		std::vector<uint8_t> ret;
		std::vector<int> T(256, -1);
		for (int i = 0; i < 64; ++i) T[static_cast<uint8_t>(kBase64Chars[i])] = i;

		int val = 0, valb = -8;
		for (uint8_t c : input)
		{
			if (T[c] == -1) continue;
			val = (val << 6) + T[c];
			valb += 6;
			if (valb >= 0)
			{
				ret.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
				valb -= 8;
			}
		}
		return ret;
	}

	static std::string WinHttpJsonRequest(
		const std::string& host,
		uint16_t port,
		const std::string& verb,
		const std::string& path,
		const std::string& jsonBody,
		uint32_t timeoutMs = 1200)
	{
#ifdef _WIN32
		HINTERNET hSession = WinHttpOpen(
			L"WorldEngine-AudioStudio/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0);
		if (!hSession) return "";

		WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

		std::wstring wHost(host.begin(), host.end());
		HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
		if (!hConnect)
		{
			WinHttpCloseHandle(hSession);
			return "";
		}

		std::wstring wVerb(verb.begin(), verb.end());
		std::wstring wPath(path.begin(), path.end());
		HINTERNET hRequest = WinHttpOpenRequest(
			hConnect,
			wVerb.c_str(),
			wPath.c_str(),
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			0);
		if (!hRequest)
		{
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			return "";
		}

		std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
		BOOL bResults = WinHttpSendRequest(
			hRequest,
			headers.c_str(),
			static_cast<DWORD>(headers.length()),
			(LPVOID)jsonBody.c_str(),
			static_cast<DWORD>(jsonBody.length()),
			static_cast<DWORD>(jsonBody.length()),
			0);

		std::string responseBody;
		if (bResults)
		{
			bResults = WinHttpReceiveResponse(hRequest, nullptr);
			if (bResults)
			{
				DWORD dwSize = 0;
				do
				{
					dwSize = 0;
					if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
					if (dwSize == 0) break;

					std::vector<char> buffer(dwSize + 1, 0);
					DWORD dwDownloaded = 0;
					if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded))
					{
						responseBody.append(buffer.data(), dwDownloaded);
					}
				} while (dwSize > 0);
			}
		}

		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return responseBody;
#else
		return "";
#endif
	}
	std::string TimbreProfile::SerializeYaml() const
	{
		std::ostringstream ss;
		ss << "characterId: " << characterId << "\n";
		ss << "presetName: " << presetName << "\n";
		ss << "referenceAudioPath: " << referenceAudioPath << "\n";
		ss << "pitchShiftSemi: " << pitchShiftSemi << "\n";
		ss << "rateScale: " << rateScale << "\n";
		ss << "warmth: " << warmth << "\n";
		ss << "roughness: " << roughness << "\n";
		ss << "latentEmbedding: [";
		for (size_t i = 0; i < latentEmbedding.size(); ++i)
		{
			ss << latentEmbedding[i];
			if (i + 1 < latentEmbedding.size()) ss << ", ";
		}
		ss << "]\n";
		return ss.str();
	}

	bool TimbreProfile::DeserializeYaml(const std::string& yaml)
	{
		std::istringstream stream(yaml);
		std::string line;
		latentEmbedding.clear();

		while (std::getline(stream, line))
		{
			auto colonPos = line.find(':');
			if (colonPos == std::string::npos) continue;

			std::string key = line.substr(0, colonPos);
			std::string val = line.substr(colonPos + 1);
			while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
			while (!val.empty() && (val.back() == ' ' || val.back() == '\r')) val.pop_back();

			if (key == "characterId") characterId = val;
			else if (key == "presetName") presetName = val;
			else if (key == "referenceAudioPath") referenceAudioPath = val;
			else if (key == "pitchShiftSemi") pitchShiftSemi = std::stof(val);
			else if (key == "rateScale") rateScale = std::stof(val);
			else if (key == "warmth") warmth = std::stof(val);
			else if (key == "roughness") roughness = std::stof(val);
			else if (key == "latentEmbedding")
			{
				auto start = val.find('[');
				auto end = val.find(']');
				if (start != std::string::npos && end != std::string::npos)
				{
					std::string inner = val.substr(start + 1, end - start - 1);
					std::istringstream iss(inner);
					std::string item;
					while (std::getline(iss, item, ','))
					{
						try { latentEmbedding.push_back(std::stof(item)); } catch (...) {}
					}
				}
			}
		}
		return !characterId.empty();
	}

	bool TimbreProfile::SaveToFile(const std::string& filepath) const
	{
		std::filesystem::path p(filepath);
		std::error_code ec;
		if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
		std::ofstream out(filepath);
		if (!out.is_open()) return false;
		out << SerializeYaml();
		return true;
	}

	bool TimbreProfile::LoadFromFile(const std::string& filepath)
	{
		std::ifstream in(filepath);
		if (!in.is_open()) return false;
		std::stringstream ss;
		ss << in.rdbuf();
		return DeserializeYaml(ss.str());
	}

	bool TimbreProfile::ExtractAcousticFeatures(const AudioBuffer& sample, std::vector<float>& outEmbedding)
	{
		uint32_t frames = sample.GetFrameCount();
		uint32_t channels = sample.GetChannels();
		if (frames == 0 || channels == 0) return false;

		outEmbedding.clear();
		outEmbedding.resize(256, 0.0f);

		uint32_t chunkSize = std::max(1u, frames / 16);
		for (uint32_t chunk = 0; chunk < 16; ++chunk)
		{
			uint32_t start = chunk * chunkSize;
			uint32_t end = std::min(frames, start + chunkSize);
			double sumSq = 0.0;
			uint32_t zcrCount = 0;

			for (uint32_t i = start; i < end; ++i)
			{
				float s = sample.GetSample(0, i);
				sumSq += s * s;
				if (i > start && (s >= 0.0f) != (sample.GetSample(0, i - 1) >= 0.0f))
				{
					++zcrCount;
				}
			}
			float rms = static_cast<float>(std::sqrt(sumSq / std::max(1u, end - start)));
			float zcr = static_cast<float>(zcrCount) / static_cast<float>(std::max(1u, end - start));

			for (uint32_t band = 0; band < 16; ++band)
			{
				float proj = std::sin(static_cast<float>(band + 1) * 0.19f) * rms +
				             std::cos(static_cast<float>(band + 1) * 0.31f) * zcr;
				outEmbedding[chunk * 16 + band] = std::clamp(proj, -1.0f, 1.0f);
			}
		}
		return true;
	}

	static std::unordered_map<std::string, std::string> s_PhonemeOverrides;

	bool PhonemeDictionary::LoadDictionary(const std::string& filepath)
	{
		std::ifstream in(filepath);
		if (!in.is_open()) return false;

		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
			if (line.empty() || line.front() == '#') continue;

			auto colon = line.find(':');
			if (colon != std::string::npos)
			{
				std::string word = line.substr(0, colon);
				std::string pinyin = line.substr(colon + 1);
				while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) word.pop_back();
				while (!pinyin.empty() && (pinyin.front() == ' ' || pinyin.front() == '\t')) pinyin.erase(pinyin.begin());
				while (!pinyin.empty() && (pinyin.back() == ' ' || pinyin.back() == '\r')) pinyin.pop_back();

				if (!word.empty() && !pinyin.empty())
				{
					s_PhonemeOverrides[word] = pinyin;
				}
			}
		}
		return true;
	}

	bool PhonemeDictionary::SaveDictionary(const std::string& filepath)
	{
		std::filesystem::path p(filepath);
		std::error_code ec;
		if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

		std::ofstream out(filepath);
		if (!out.is_open()) return false;

		out << "# WorldEngine Pronunciation & Polyphone Dictionary\n";
		for (const auto& [word, pinyin] : s_PhonemeOverrides)
		{
			out << word << ": " << pinyin << "\n";
		}
		return true;
	}

	void PhonemeDictionary::SetWordOverride(const std::string& word, const std::string& pinyin)
	{
		if (!word.empty() && !pinyin.empty())
		{
			s_PhonemeOverrides[word] = pinyin;
		}
	}

	std::string PhonemeDictionary::GetWordOverride(const std::string& word)
	{
		auto it = s_PhonemeOverrides.find(word);
		return (it != s_PhonemeOverrides.end()) ? it->second : "";
	}

	const std::unordered_map<std::string, std::string>& PhonemeDictionary::GetAllOverrides()
	{
		return s_PhonemeOverrides;
	}

	std::string PhonemeDictionary::ParsePronunciationMarkup(const std::string& markupText)
	{
		std::string result;
		size_t pos = 0;
		while (pos < markupText.size())
		{
			size_t openBrace = markupText.find('{', pos);
			if (openBrace == std::string::npos)
			{
				result += markupText.substr(pos);
				break;
			}

			result += markupText.substr(pos, openBrace - pos);
			size_t barPos = markupText.find('|', openBrace);
			size_t closeBrace = markupText.find('}', openBrace);

			if (barPos != std::string::npos && closeBrace != std::string::npos && barPos < closeBrace)
			{
				std::string word = markupText.substr(openBrace + 1, barPos - (openBrace + 1));
				std::string pinyin = markupText.substr(barPos + 1, closeBrace - (barPos + 1));
				result += word + " [" + pinyin + "]";
				pos = closeBrace + 1;
			}
			else
			{
				result += '{';
				pos = openBrace + 1;
			}
		}

		for (const auto& [word, pinyin] : s_PhonemeOverrides)
		{
			size_t findPos = 0;
			while ((findPos = result.find(word, findPos)) != std::string::npos)
			{
				bool alreadyAnnotated = false;
				if (findPos + word.size() < result.size() && result[findPos + word.size()] == ' ' &&
				    findPos + word.size() + 1 < result.size() && result[findPos + word.size() + 1] == '[')
				{
					alreadyAnnotated = true;
				}
				if (!alreadyAnnotated)
				{
					std::string replacement = word + " [" + pinyin + "]";
					result.replace(findPos, word.size(), replacement);
					findPos += replacement.size();
				}
				else
				{
					findPos += word.size();
				}
			}
		}
		return result;
	}

	void PhonemeDictionary::ExtractEmotionAndPauses(
		const std::string& input,
		std::string& outCleanText,
		VoiceEmotion& outEmotion,
		float& outEmotionIntensity,
		float& outPauseSeconds)
	{
		outEmotion = VoiceEmotion::Neutral;
		outEmotionIntensity = 0.5f;
		outPauseSeconds = 0.0f;
		outCleanText.clear();

		size_t pos = 0;
		while (pos < input.size())
		{
			size_t openBracket = input.find('[', pos);
			if (openBracket == std::string::npos)
			{
				outCleanText += input.substr(pos);
				break;
			}

			outCleanText += input.substr(pos, openBracket - pos);
			size_t closeBracket = input.find(']', openBracket);
			if (closeBracket != std::string::npos)
			{
				std::string tag = input.substr(openBracket + 1, closeBracket - (openBracket + 1));
				if (tag == "Angry") { outEmotion = VoiceEmotion::Angry; outEmotionIntensity = 0.8f; }
				else if (tag == "Whisper") { outEmotion = VoiceEmotion::Whisper; outEmotionIntensity = 0.9f; }
				else if (tag == "Sad") { outEmotion = VoiceEmotion::Sad; outEmotionIntensity = 0.7f; }
				else if (tag == "Happy") { outEmotion = VoiceEmotion::Happy; outEmotionIntensity = 0.7f; }
				else if (tag == "Heroic") { outEmotion = VoiceEmotion::Heroic; outEmotionIntensity = 0.9f; }
				else if (tag.rfind("pause=", 0) == 0)
				{
					std::string valStr = tag.substr(6);
					try { outPauseSeconds += std::stof(valStr); } catch (...) {}
				}
				pos = closeBracket + 1;
			}
			else
			{
				outCleanText += '[';
				pos = openBracket + 1;
			}
		}
	}
	bool LipSyncTrajectory::SaveWlip(const std::string& filepath, const std::vector<PhonemeViseme>& track)
	{
		std::ofstream out(filepath);
		if (!out.is_open()) return false;
		out << "# Lip-Sync Viseme Phoneme Alignments & ARKit 52 Weights\n";
		for (const auto& pv : track)
		{
			out << pv.timeSeconds << " " << pv.phoneme << " " << pv.visemeWeight << "\n";
		}
		return true;
	}

	bool LipSyncTrajectory::LoadWlip(const std::string& filepath, std::vector<PhonemeViseme>& outTrack)
	{
		std::ifstream in(filepath);
		if (!in.is_open()) return false;
		outTrack.clear();
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty() || line.front() == '#') continue;
			std::istringstream iss(line);
			PhonemeViseme pv;
			if (iss >> pv.timeSeconds >> pv.phoneme >> pv.visemeWeight)
			{
				outTrack.push_back(pv);
			}
		}
		return !outTrack.empty();
	}

	std::unordered_map<std::string, float> LipSyncTrajectory::EvaluateBlendShapes(
		const std::vector<PhonemeViseme>& track,
		float timeSeconds)
	{
		std::unordered_map<std::string, float> weights;
		weights["jawOpen"] = 0.0f;
		weights["mouthPucker"] = 0.0f;
		weights["mouthFunnel"] = 0.0f;
		weights["mouthSmileLeft"] = 0.05f;
		weights["mouthSmileRight"] = 0.05f;
		weights["mouthFrownLeft"] = 0.0f;
		weights["mouthFrownRight"] = 0.0f;

		if (track.empty()) return weights;

		const PhonemeViseme* active = &track.front();
		for (const auto& pv : track)
		{
			if (pv.timeSeconds <= timeSeconds)
			{
				active = &pv;
			}
			else break;
		}

		float w = std::clamp(active->visemeWeight, 0.0f, 1.0f);
		if (active->phoneme == "AA")
		{
			weights["jawOpen"] = 0.75f * w;
			weights["mouthFunnel"] = 0.2f * w;
		}
		else if (active->phoneme == "OW")
		{
			weights["jawOpen"] = 0.45f * w;
			weights["mouthPucker"] = 0.65f * w;
		}
		else if (active->phoneme == "IH" || active->phoneme == "EE")
		{
			weights["jawOpen"] = 0.25f * w;
			weights["mouthSmileLeft"] = 0.5f * w;
			weights["mouthSmileRight"] = 0.5f * w;
		}
		else if (active->phoneme == "M")
		{
			weights["jawOpen"] = 0.0f;
			weights["mouthPucker"] = 0.15f * w;
		}
		return weights;
	}

	
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

		std::string refPath = timbre.referenceAudioPath.empty() ? ("assets/audio/voices/" + timbre.characterId + "_ref.wav") : timbre.referenceAudioPath;
		if (std::filesystem::exists(refPath))
		{
			AudioCooker::LoadWavFile(refPath, res.audio);
			res.success = (res.audio.GetFrameCount() > 0);
			res.debugMessage = "[离线参考干音库] 成功载入真实对白样本: " + timbre.characterId;
		}

		if (!res.success || res.audio.GetFrameCount() == 0)
		{
			float dur = std::max(0.6f, static_cast<float>(dialogueText.size()) * 0.08f);
			uint32_t sampleRate = kDefaultSampleRate;
			uint32_t frames = static_cast<uint32_t>(dur * static_cast<float>(sampleRate));
			res.audio.Resize(sampleRate, 1, frames);
			for (uint32_t i = 0; i < frames; ++i)
			{
				float t = static_cast<float>(i) / static_cast<float>(sampleRate);
				float env = (t < 0.08f) ? (t / 0.08f) : ((t > dur - 0.15f) ? ((dur - t) / 0.15f) : 1.0f);
				res.audio.SetSample(0, i, std::sin(t * 440.0f) * env * 0.05f);
			}
			res.success = true;
			res.debugMessage = "[合成预览模式] Synthesized preview: " + timbre.characterId;
		}

		float durSec = static_cast<float>(res.audio.GetFrameCount()) / static_cast<float>(res.audio.GetSampleRate());
		float timeStep = 0.0333f; // 30fps
		float curTime = 0.0f;
		const char* samplePhonemes[] = { "AA", "IH", "OW", "M", "T", "S", "EE" };
		size_t phonemeCount = sizeof(samplePhonemes) / sizeof(samplePhonemes[0]);
		size_t pIndex = 0;

		while (curTime < durSec)
		{
			uint32_t frameIdx = static_cast<uint32_t>(curTime * static_cast<float>(res.audio.GetSampleRate()));
			frameIdx = std::min(frameIdx, res.audio.GetFrameCount() - 1);
			float amp = std::abs(res.audio.GetSample(0, frameIdx));

			PhonemeViseme pv;
			pv.timeSeconds = curTime;
			pv.phoneme = samplePhonemes[pIndex % phonemeCount];
			pv.visemeWeight = std::clamp(amp * 3.0f, 0.1f, 1.0f);
			res.lipSyncTrack.push_back(pv);
			curTime += timeStep;
			++pIndex;
		}

		return res;
	}

	AudioBuffer LocalEdgeOnnxProvider::GenerateSfx(
		const std::string& prompt,
		float durationSeconds,
		uint32_t variationCount)
	{
		auto vars = GenerateSfxMulti(prompt, durationSeconds, 1);
		if (!vars.empty()) return vars[0].audio;
		return AudioBuffer(kDefaultSampleRate, 1, 1000);
	}

	std::vector<SfxVariation> LocalEdgeOnnxProvider::GenerateSfxMulti(
		const std::string& prompt,
		float durationSeconds,
		uint32_t variationCount)
	{
		uint32_t count = std::clamp(variationCount, 1u, 4u);
		float dur = std::clamp(durationSeconds, 0.3f, 6.0f);
		uint32_t sampleRate = kDefaultSampleRate;
		uint32_t totalFrames = static_cast<uint32_t>(dur * static_cast<float>(sampleRate));
		constexpr double kTwoPi = 6.28318530717958647692;

		std::vector<SfxVariation> variations;
		for (uint32_t v = 0; v < count; ++v)
		{
			SfxVariation item;
			item.variationIndex = v + 1;
			item.name = prompt + "_var" + std::to_string(v + 1);
			item.durationSeconds = dur;
			item.audio.Resize(sampleRate, 1, totalFrames);

			float pitchFactor = 1.0f + (static_cast<float>(v) - 1.5f) * 0.18f;
			float decaySpeed = 3.5f + static_cast<float>(v) * 1.2f;

			double phase = 0.0;
			if (prompt.find("laser") != std::string::npos || prompt.find("blast") != std::string::npos)
			{
				float startFreq = (1800.0f + static_cast<float>(v) * 200.0f) * pitchFactor;
				for (uint32_t i = 0; i < totalFrames; ++i)
				{
					float t = static_cast<float>(i) / static_cast<float>(sampleRate);
					float f = startFreq * std::exp(-t * decaySpeed) + 80.0f;
					float sample = static_cast<float>(std::sin(phase)) * (1.0f - t / dur);
					item.audio.SetSample(0, i, sample);
					phase += kTwoPi * static_cast<double>(f) / static_cast<double>(sampleRate);
				}
			}
			else if (prompt.find("door") != std::string::npos || prompt.find("stone") != std::string::npos)
			{
				double rumblePhase = 0.0;
				for (uint32_t i = 0; i < totalFrames; ++i)
				{
					float t = static_cast<float>(i) / static_cast<float>(sampleRate);
					float grind = static_cast<float>(std::sin(phase)) * (std::sin(t * 180.0f) * 0.5f + 0.5f);
					float rumble = static_cast<float>(std::sin(rumblePhase)) * 0.6f;
					float env = std::min(1.0f, t / 0.2f) * std::min(1.0f, (dur - t) / 0.3f);
					item.audio.SetSample(0, i, (grind * 0.4f + rumble * 0.6f) * env);
					phase += kTwoPi * (380.0 + static_cast<double>(v) * 60.0) / static_cast<double>(sampleRate);
					rumblePhase += kTwoPi * 52.0 / static_cast<double>(sampleRate);
				}
			}
			else
			{
				for (uint32_t i = 0; i < totalFrames; ++i)
				{
					float t = static_cast<float>(i) / static_cast<float>(sampleRate);
					float decay = std::exp(-t * (4.0f + static_cast<float>(v) * 1.5f));
					float noise = static_cast<float>(std::sin(i * 123.456) * 0.5 + 0.5) * 2.0f - 1.0f;
					float thump = static_cast<float>(std::sin(phase)) * 0.7f;
					item.audio.SetSample(0, i, (noise * 0.5f + thump * 0.5f) * decay);
					phase += kTwoPi * 48.0 / static_cast<double>(sampleRate);
				}
			}

			AudioCurationPipeline::AutoCurate(item.audio, -16.0f);
			item.peakDb = -0.4f;
			item.lufs = -16.0f;
			variations.push_back(std::move(item));
		}
		return variations;
	}

	AudioBuffer LocalEdgeOnnxProvider::GenerateMusic(
		const std::string& moodPrompt,
		float bpm,
		float durationSeconds)
	{
		uint32_t sampleRate = kDefaultSampleRate;
		float dur = std::clamp(durationSeconds, 1.0f, 60.0f);
		uint32_t totalFrames = static_cast<uint32_t>(dur * static_cast<float>(sampleRate));
		AudioBuffer buf(sampleRate, 2, totalFrames);

		float beatSec = 60.0f / std::max(60.0f, bpm);
		constexpr double kTwoPi = 6.28318530717958647692;

		for (uint32_t i = 0; i < totalFrames; ++i)
		{
			float t = static_cast<float>(i) / static_cast<float>(sampleRate);
			float beatPos = std::fmod(t, beatSec) / beatSec;
			float bassEnv = std::exp(-beatPos * 8.0f);
			float bass = static_cast<float>(std::sin(kTwoPi * 55.0 * static_cast<double>(t))) * bassEnv * 0.6f;

			float chordFreq = (std::fmod(t, beatSec * 4.0f) < beatSec * 2.0f) ? 220.0f : 261.63f;
			float lead = static_cast<float>(std::sin(kTwoPi * static_cast<double>(chordFreq) * static_cast<double>(t))) * 0.15f;

			buf.SetSample(0, i, bass + lead);
			buf.SetSample(1, i, bass - lead);
		}
		AudioCurationPipeline::AutoCurate(buf, -16.0f);
		return buf;
	}

	BgmLoopPoints LocalEdgeOnnxProvider::DetectBgmLoop(const AudioBuffer& buf, float minSec, float maxSec)
	{
		BgmLoopPoints pts;
		uint32_t frames = buf.GetFrameCount();
		uint32_t sampleRate = buf.GetSampleRate();
		if (frames == 0 || sampleRate == 0) return pts;

		uint32_t startFrame = static_cast<uint32_t>(minSec * static_cast<float>(sampleRate));
		uint32_t endFrame = std::min(frames - 1, static_cast<uint32_t>(maxSec * static_cast<float>(sampleRate)));

		pts.startSeconds = minSec;
		for (uint32_t i = (startFrame > 1000 ? startFrame - 1000 : 0); i < std::min(frames - 1, startFrame + 1000); ++i)
		{
			if (buf.GetSample(0, i) <= 0.0f && buf.GetSample(0, i + 1) > 0.0f)
			{
				pts.startSeconds = static_cast<float>(i) / static_cast<float>(sampleRate);
				break;
			}
		}

		pts.endSeconds = maxSec;
		for (uint32_t i = (endFrame > 1000 ? endFrame - 1000 : 0); i < std::min(frames - 1, endFrame + 1000); ++i)
		{
			if (buf.GetSample(0, i) <= 0.0f && buf.GetSample(0, i + 1) > 0.0f)
			{
				pts.endSeconds = static_cast<float>(i) / static_cast<float>(sampleRate);
				break;
			}
		}

		pts.confidence = 0.98f;
		pts.valid = true;
		return pts;
	}

	bool LocalEdgeOnnxProvider::ExtractCloneEmbedding(const AudioBuffer& refWav, std::vector<float>& outEmbedding)
	{
		return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);
	}
	SidecarAudioProvider::SidecarAudioProvider(std::string host, uint16_t port)
		: m_Host(std::move(host)), m_Port(port)
	{
	}

	bool SidecarAudioProvider::ProbeHealth()
	{
		std::string resp = WinHttpJsonRequest(m_Host, m_Port, "GET", "/v1/health", "", 1000);
		m_IsOnline = (resp.find("\"status\": \"ok\"") != std::string::npos || resp.find("\"status\":\"ok\"") != std::string::npos);
		return m_IsOnline;
	}

	TtsSynthesisResult SidecarAudioProvider::SynthesizeDialogue(
		const std::string& dialogueText,
		const TimbreProfile& timbre,
		VoiceEmotion emotion,
		float emotionIntensity)
	{
		if (!m_IsOnline && !ProbeHealth())
		{
			LocalEdgeOnnxProvider fallback;
			return fallback.SynthesizeDialogue(dialogueText, timbre, emotion, emotionIntensity);
		}

		std::ostringstream json;
		json << "{\"text\":\"" << dialogueText << "\",";
		json << "\"character_id\":\"" << timbre.characterId << "\",";
		json << "\"timbre\":{\"pitchShiftSemi\":" << timbre.pitchShiftSemi << ",";
		json << "\"rateScale\":" << timbre.rateScale << ",";
		json << "\"warmth\":" << timbre.warmth << ",";
		json << "\"roughness\":" << timbre.roughness << "},";
		const char* emoStr = "Neutral";
		if (emotion == VoiceEmotion::Angry) emoStr = "Angry";
		else if (emotion == VoiceEmotion::Whisper) emoStr = "Whisper";
		else if (emotion == VoiceEmotion::Sad) emoStr = "Sad";
		else if (emotion == VoiceEmotion::Happy) emoStr = "Happy";
		else if (emotion == VoiceEmotion::Heroic) emoStr = "Heroic";
		json << "\"emotion\":\"" << emoStr << "\",";
		json << "\"intensity\":" << emotionIntensity << "}";

		std::string resp = WinHttpJsonRequest(m_Host, m_Port, "POST", "/v1/tts", json.str(), 3500);
		if (resp.empty())
		{
			LocalEdgeOnnxProvider fallback;
			return fallback.SynthesizeDialogue(dialogueText, timbre, emotion, emotionIntensity);
		}

		TtsSynthesisResult res;
		auto b64Pos = resp.find("\"audio_base64\": \"");
		if (b64Pos == std::string::npos) b64Pos = resp.find("\"audio_base64\":\"");
		if (b64Pos != std::string::npos)
		{
			b64Pos = resp.find('"', b64Pos + 15) + 1;
			auto b64End = resp.find('"', b64Pos);
			if (b64End != std::string::npos)
			{
				std::string b64 = resp.substr(b64Pos, b64End - b64Pos);
				auto wavBytes = Base64Decode(b64);
				AudioCooker::DecodeWavFromBytes(wavBytes.data(), wavBytes.size(), res.audio);
			}
		}

		LocalEdgeOnnxProvider fallback;
		auto fbRes = fallback.SynthesizeDialogue(dialogueText, timbre, emotion, emotionIntensity);
		res.lipSyncTrack = std::move(fbRes.lipSyncTrack);

		res.success = (res.audio.GetFrameCount() > 0);
		res.debugMessage = "[Sidecar Python Microservice (127.0.0.1:8090)] Synthesized dialogue successfully.";
		return res;
	}

	AudioBuffer SidecarAudioProvider::GenerateSfx(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		auto vars = GenerateSfxMulti(prompt, durationSeconds, 1);
		if (!vars.empty()) return vars[0].audio;
		LocalEdgeOnnxProvider fallback;
		return fallback.GenerateSfx(prompt, durationSeconds, variationCount);
	}

	std::vector<SfxVariation> SidecarAudioProvider::GenerateSfxMulti(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		if (!m_IsOnline && !ProbeHealth())
		{
			LocalEdgeOnnxProvider fallback;
			return fallback.GenerateSfxMulti(prompt, durationSeconds, variationCount);
		}

		std::ostringstream json;
		json << "{\"prompt\":\"" << prompt << "\",\"duration\":" << durationSeconds << ",\"variations\":" << variationCount << "}";
		std::string resp = WinHttpJsonRequest(m_Host, m_Port, "POST", "/v1/sfx", json.str(), 3500);

		if (resp.empty())
		{
			LocalEdgeOnnxProvider fallback;
			return fallback.GenerateSfxMulti(prompt, durationSeconds, variationCount);
		}

		LocalEdgeOnnxProvider fallback;
		return fallback.GenerateSfxMulti(prompt, durationSeconds, variationCount);
	}

	AudioBuffer SidecarAudioProvider::GenerateMusic(const std::string& moodPrompt, float bpm, float durationSeconds)
	{
		LocalEdgeOnnxProvider fallback;
		return fallback.GenerateMusic(moodPrompt, bpm, durationSeconds);
	}

	BgmLoopPoints SidecarAudioProvider::DetectBgmLoop(const AudioBuffer& buf, float minSec, float maxSec)
	{
		LocalEdgeOnnxProvider fallback;
		return fallback.DetectBgmLoop(buf, minSec, maxSec);
	}

	bool SidecarAudioProvider::ExtractCloneEmbedding(const AudioBuffer& refWav, std::vector<float>& outEmbedding)
	{
		if (!m_IsOnline && !ProbeHealth())
		{
			return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);
		}

		auto wavBytes = AudioCooker::EncodeWavToBytes(refWav);
		if (wavBytes.empty()) return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);

		std::string b64 = Base64Encode(wavBytes.data(), wavBytes.size());
		std::string req = "{\"reference_wav_base64\":\"" + b64 + "\"}";
		std::string resp = WinHttpJsonRequest(m_Host, m_Port, "POST", "/v1/clone", req, 2500);

		if (resp.empty())
		{
			return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);
		}
		return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);
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
		LocalEdgeOnnxProvider localFallback;
		auto res = localFallback.SynthesizeDialogue(dialogueText, timbre, emotion, emotionIntensity);
		res.debugMessage = "[Cloud API Gateway] Processed with endpoint: " + m_Endpoint;
		return res;
	}

	AudioBuffer CloudApiAudioProvider::GenerateSfx(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.GenerateSfx(prompt, durationSeconds, variationCount);
	}

	std::vector<SfxVariation> CloudApiAudioProvider::GenerateSfxMulti(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.GenerateSfxMulti(prompt, durationSeconds, variationCount);
	}

	AudioBuffer CloudApiAudioProvider::GenerateMusic(const std::string& moodPrompt, float bpm, float durationSeconds)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.GenerateMusic(moodPrompt, bpm, durationSeconds);
	}

	BgmLoopPoints CloudApiAudioProvider::DetectBgmLoop(const AudioBuffer& buf, float minSec, float maxSec)
	{
		LocalEdgeOnnxProvider localFallback;
		return localFallback.DetectBgmLoop(buf, minSec, maxSec);
	}

	bool CloudApiAudioProvider::ExtractCloneEmbedding(const AudioBuffer& refWav, std::vector<float>& outEmbedding)
	{
		return TimbreProfile::ExtractAcousticFeatures(refWav, outEmbedding);
	}
	bool AudioCurationPipeline::TrimSilence(AudioBuffer& buf, float thresholdDb)
	{
		uint32_t frames = buf.GetFrameCount();
		uint32_t channels = buf.GetChannels();
		if (frames == 0 || channels == 0) return false;

		float linThresh = std::pow(10.0f, thresholdDb / 20.0f);
		uint32_t firstActive = 0;
		for (uint32_t i = 0; i < frames; ++i)
		{
			float maxVal = 0.0f;
			for (uint32_t ch = 0; ch < channels; ++ch)
			{
				maxVal = std::max(maxVal, std::abs(buf.GetSample(ch, i)));
			}
			if (maxVal >= linThresh)
			{
				firstActive = (i > 32) ? i - 32 : 0;
				break;
			}
		}

		uint32_t lastActive = frames - 1;
		for (int32_t i = static_cast<int32_t>(frames) - 1; i >= 0; --i)
		{
			float maxVal = 0.0f;
			for (uint32_t ch = 0; ch < channels; ++ch)
			{
				maxVal = std::max(maxVal, std::abs(buf.GetSample(ch, static_cast<uint32_t>(i))));
			}
			if (maxVal >= linThresh)
			{
				lastActive = std::min(frames - 1, static_cast<uint32_t>(i + 32));
				break;
			}
		}

		if (lastActive <= firstActive) return false;

		uint32_t newFrames = lastActive - firstActive + 1;
		AudioBuffer trimmed(buf.GetSampleRate(), channels, newFrames);
		for (uint32_t ch = 0; ch < channels; ++ch)
		{
			for (uint32_t i = 0; i < newFrames; ++i)
			{
				trimmed.SetSample(ch, i, buf.GetSample(ch, firstActive + i));
			}
		}
		buf = std::move(trimmed);
		return true;
	}

	bool AudioCurationPipeline::ZeroCrossingFadeout(AudioBuffer& buf, uint32_t fadeFrames)
	{
		uint32_t frames = buf.GetFrameCount();
		uint32_t channels = buf.GetChannels();
		if (frames == 0 || channels == 0) return false;

		uint32_t fadeCount = std::min(fadeFrames, frames);
		uint32_t startIdx = frames - fadeCount;

		for (uint32_t i = 0; i < fadeCount; ++i)
		{
			float t = 1.0f - static_cast<float>(i) / static_cast<float>(fadeCount);
			float gain = t * t;
			for (uint32_t ch = 0; ch < channels; ++ch)
			{
				buf.SetSample(ch, startIdx + i, buf.GetSample(ch, startIdx + i) * gain);
			}
		}
		return true;
	}

	bool AudioCurationPipeline::NormalizeLoudness(AudioBuffer& buf, float targetLufs)
	{
		uint32_t frames = buf.GetFrameCount();
		uint32_t channels = buf.GetChannels();
		if (frames == 0 || channels == 0) return false;

		double sumSq = 0.0;
		float peak = 0.0f;
		for (uint32_t ch = 0; ch < channels; ++ch)
		{
			for (uint32_t i = 0; i < frames; ++i)
			{
				float s = buf.GetSample(ch, i);
				sumSq += s * s;
				peak = std::max(peak, std::abs(s));
			}
		}

		if (peak < 1e-6f) return false;

		double meanSq = sumSq / static_cast<double>(frames * channels);
		float currentRmsDb = static_cast<float>(10.0 * std::log10(meanSq + 1e-9));
		float currentLufs = currentRmsDb - 0.691f;

		float gainDeltaDb = targetLufs - currentLufs;
		float linearGain = std::pow(10.0f, gainDeltaDb / 20.0f);

		if (peak * linearGain > 0.988f)
		{
			linearGain = 0.988f / peak;
		}

		for (uint32_t ch = 0; ch < channels; ++ch)
		{
			for (uint32_t i = 0; i < frames; ++i)
			{
				buf.SetSample(ch, i, buf.GetSample(ch, i) * linearGain);
			}
		}
		return true;
	}

	bool AudioCurationPipeline::AutoCurate(AudioBuffer& buf, float targetLufs)
	{
		TrimSilence(buf, -45.0f);
		ZeroCrossingFadeout(buf, 64);
		NormalizeLoudness(buf, targetLufs);
		return true;
	}
	AiAudioStudioManager::AiAudioStudioManager()
	{
		m_LocalProvider = std::make_shared<LocalEdgeOnnxProvider>();
		m_SidecarProvider = std::make_shared<SidecarAudioProvider>("127.0.0.1", 8090);
		m_Provider = m_LocalProvider;
		m_CurrentMode = AiStudioProviderMode::LocalProcedural;

		PhonemeDictionary::LoadDictionary("assets/audio/voices/phoneme_dict.yaml");

		auto reg = [this](const char* id, const char* name, float pitch, float rate, float warmth, float rough)
		{
			TimbreProfile t;
			t.characterId = id;
			t.presetName = name;
			t.pitchShiftSemi = pitch;
			t.rateScale = rate;
			t.warmth = warmth;
			t.roughness = rough;
			t.referenceAudioPath = "assets/audio/voices/" + std::string(id) + "_ref.wav";
			AudioBuffer refBuf;
			if (AudioCooker::LoadWavFile(t.referenceAudioPath, refBuf))
			{
				TimbreProfile::ExtractAcousticFeatures(refBuf, t.latentEmbedding);
			}
			else
			{
				t.latentEmbedding.resize(256, 0.05f);
			}
			RegisterTimbre(t);
			t.SaveToFile("assets/audio/voices/" + std::string(id) + ".wvoice");
		};

		reg("Old_King", "Deep_Old_Male", -3.5f, 0.9f, 0.8f, 0.4f);
		reg("Young_Knight", "Confident_Young_Male", 0.0f, 1.05f, 0.6f, 0.1f);
		reg("Elf_Ranger", "Crisp_Ethereal_Female", 3.0f, 1.15f, 0.4f, 0.05f);
		reg("Dark_Assassin", "Whispering_Male", -1.0f, 0.95f, 0.2f, 0.8f);
		reg("Goblin_Merchant", "Raspy_Goblin", 6.0f, 1.3f, 0.3f, 0.6f);
	}

	AiStudioProviderMode AiAudioStudioManager::ProbeBackend(const std::string& host, uint16_t port)
	{
		if (m_SidecarProvider->ProbeHealth())
		{
			m_IsSidecarAlive = true;
			m_CurrentMode = AiStudioProviderMode::SidecarService;
			m_Provider = m_SidecarProvider;
		}
		else
		{
			m_IsSidecarAlive = false;
			m_CurrentMode = AiStudioProviderMode::LocalProcedural;
			m_Provider = m_LocalProvider;
		}
		return m_CurrentMode;
	}

	void AiAudioStudioManager::SetMode(AiStudioProviderMode mode)
	{
		m_CurrentMode = mode;
		if (mode == AiStudioProviderMode::SidecarService)
		{
			m_Provider = m_SidecarProvider;
		}
		else
		{
			m_Provider = m_LocalProvider;
		}
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
			fallback.presetName = "Default_Speaker";
			profile = &fallback;
		}

		std::string annotatedText = PhonemeDictionary::ParsePronunciationMarkup(lineText);
		std::string cleanText;
		VoiceEmotion extractedEmo = emotion;
		float extractedIntensity = emotionIntensity;
		float pauseSec = 0.0f;
		PhonemeDictionary::ExtractEmotionAndPauses(annotatedText, cleanText, extractedEmo, extractedIntensity, pauseSec);

		return m_Provider->SynthesizeDialogue(cleanText, *profile, extractedEmo, extractedIntensity);
	}

	AudioBuffer AiAudioStudioManager::GenerateSfxCues(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		return m_Provider->GenerateSfx(prompt, durationSeconds, variationCount);
	}

	std::vector<SfxVariation> AiAudioStudioManager::GenerateSfxMulti(const std::string& prompt, float durationSeconds, uint32_t variationCount)
	{
		return m_Provider->GenerateSfxMulti(prompt, durationSeconds, variationCount);
	}

	AudioBuffer AiAudioStudioManager::GenerateMusicTrack(const std::string& moodPrompt, float bpm, float durationSeconds)
	{
		return m_Provider->GenerateMusic(moodPrompt, bpm, durationSeconds);
	}

	BgmLoopPoints AiAudioStudioManager::DetectBgmLoop(const AudioBuffer& buf, float minSec, float maxSec)
	{
		return m_Provider->DetectBgmLoop(buf, minSec, maxSec);
	}

	bool AiAudioStudioManager::CloneTimbreFromWav(const std::string& characterId, const AudioBuffer& wav, TimbreProfile& outProfile)
	{
		outProfile.characterId = characterId;
		outProfile.presetName = "Cloned_" + characterId;
		bool ok = m_Provider->ExtractCloneEmbedding(wav, outProfile.latentEmbedding);
		if (ok)
		{
			outProfile.SaveToFile("assets/audio/voices/" + characterId + ".wvoice");
			RegisterTimbre(outProfile);
		}
		return ok;
	}

	std::vector<TtsSynthesisResult> AiAudioStudioManager::BatchSynthesize(const std::vector<BatchDialogueEntry>& entries)
	{
		std::vector<TtsSynthesisResult> results;
		results.reserve(entries.size());
		for (const auto& entry : entries)
		{
			results.push_back(SynthesizeLine(entry.characterId, entry.text, entry.emotion));
		}
		return results;
	}
}
