#include "AudioSystem.h"
#include "AudioCooker.h"
#include <algorithm>
#include <filesystem>
#include "AudioNullBackend.h"
#include "MiniaudioBackend.h"
#include "AudioComponents.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

namespace World::Audio
{
	static AudioSystem* s_AudioInstance = nullptr;

	AudioSystem& AudioSystem::Get()
	{
		static AudioSystem instance;
		return instance;
	}

	AudioSystem::AudioSystem()
	{
		s_AudioInstance = this;
	}

	AudioSystem::~AudioSystem()
	{
		Shutdown();
		if (s_AudioInstance == this) s_AudioInstance = nullptr;
	}

	bool AudioSystem::Initialize(bool useNullBackend, uint32_t sampleRate, uint32_t bufferFrames)
	{
		if (m_Initialized) return true;

		m_SampleRate = (sampleRate > 0) ? sampleRate : kDefaultSampleRate;
		m_BufferFrames = (bufferFrames > 0) ? bufferFrames : kDefaultMixBlockFrames;

		if (useNullBackend)
		{
			m_Backend = std::make_unique<AudioNullBackend>();
		}
		else
		{
			m_Backend = std::make_unique<MiniaudioBackend>();
		}

		if (!m_Backend->Initialize(m_SampleRate, m_BufferFrames))
		{
			return false;
		}

		m_Backend->SetRenderCallback([this](float* outL, float* outR, uint32_t frames) {
			this->AudioRenderCallback(outL, outR, frames);
		});

		m_VoicePool.Reset();
		m_Mixer.ResetToDefaults();

		// Pre-populate default synthetic buffers and standard sound events
		auto clickBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateSineWave(880.0f, 0.08f, 0.3f, m_SampleRate));
		RegisterAudioBuffer("assets/audio/sfx/ui_click.waudio", clickBuffer);

		SoundEvent uiClick("ui_click", kBusUi);
		uiClick.GetSpatial().is3D = false;
		uiClick.GetRootContainer()->variations.push_back({ "assets/audio/sfx/ui_click.waudio", 1.0f });
		RegisterSoundEvent(uiClick);

		auto footstepBuffer = std::make_shared<AudioBuffer>(AudioBuffer::GenerateWhiteNoise(0.12f, 0.25f, m_SampleRate));
		RegisterAudioBuffer("assets/audio/sfx/footstep_grass.waudio", footstepBuffer);

		SoundEvent footstep("footstep_player", kBusSfx);
		footstep.GetSpatial().is3D = true;
		footstep.GetSpatial().minDistance = 1.0f;
		footstep.GetSpatial().maxDistance = 20.0f;
		footstep.SetVolumeRange(0.8f, 1.0f);
		footstep.SetPitchRangeSemitones(-1.5f, 1.5f);
		footstep.GetRootContainer()->variations.push_back({ "assets/audio/sfx/footstep_grass.waudio", 1.0f });
		RegisterSoundEvent(footstep);

				// Load default mixer configuration and voice profiles from disk
		std::string mixerPath = "projects/assets/audio/main.wmixer";
		if (!std::filesystem::exists(mixerPath)) mixerPath = "assets/audio/main.wmixer";
		if (!std::filesystem::exists(mixerPath)) mixerPath = "templates/project-example/assets/audio/main.wmixer";
		if (std::filesystem::exists(mixerPath))
		{
			std::ifstream in(mixerPath);
			if (in.is_open())
			{
				std::stringstream ss;
				ss << in.rdbuf();
				m_Mixer.DeserializeYaml(ss.str());
			}
		}

		std::string voicePath = "projects/assets/audio/voices/narrator.wvoice";
		if (!std::filesystem::exists(voicePath)) voicePath = "templates/project-example/assets/audio/voices/narrator.wvoice";
		if (std::filesystem::exists(voicePath))
		{
			std::ifstream in(voicePath);
			if (in.is_open())
			{
				std::stringstream ss;
				ss << in.rdbuf();
				TimbreProfile tp;
				if (tp.DeserializeYaml(ss.str()))
				{
					m_AiStudio.RegisterTimbre(tp);
				}
			}
		}

		m_Backend->Start();
		m_Initialized = true;
		return true;
	}

	void AudioSystem::Shutdown()
	{
		if (!m_Initialized) return;

		if (m_Backend)
		{
			m_Backend->Stop();
			m_Backend->Shutdown();
			m_Backend.reset();
		}

		m_VoicePool.Reset();
		m_Initialized = false;
	}

	void AudioSystem::AudioRenderCallback(float* outLeft, float* outRight, uint32_t frameCount)
	{
		// 1. Clear scratch bus buffers
		for (auto& [bid, bufPair] : m_BusMixBuffers)
		{
			if (bufPair.first.size() < frameCount)
			{
				bufPair.first.assign(frameCount, 0.0f);
				bufPair.second.assign(frameCount, 0.0f);
			}
			else
			{
				std::fill(bufPair.first.begin(), bufPair.first.begin() + frameCount, 0.0f);
				std::fill(bufPair.second.begin(), bufPair.second.begin() + frameCount, 0.0f);
			}
		}

		// 2. Mix physical voices into their corresponding bus buffers
		m_VoicePool.MixVoicesIntoBuses(m_BusMixBuffers, frameCount, static_cast<float>(m_SampleRate));

		// 3. Process mixer bus graph, DSP chains, snapshots, and true-peak limiting
		m_Mixer.ProcessGraph(m_BusMixBuffers, outLeft, outRight, frameCount, static_cast<float>(m_SampleRate));
	}

	void AudioSystem::Update(float deltaSeconds, Scene* activeScene)
	{
		if (!m_Initialized) return;

		m_Music.Update(deltaSeconds);

		// Sync ECS Listener and Source components if scene is provided
		if (activeScene)
		{
			auto& reg = activeScene->GetRegistry();

			// 1. Find active AudioListenerComponent
			auto listenerView = reg.view<AudioListenerComponent, TransformComponent>();
			for (auto entity : listenerView)
			{
				const auto& listener = listenerView.get<AudioListenerComponent>(entity);
				const auto& transform = listenerView.get<TransformComponent>(entity);
				if (listener.Active)
				{
					m_ListenerPosition = transform.Location;
					// Standard forward (-Z) and right (+X) rotated by transform rotation
					glm::mat4 rot = glm::mat4_cast(transform.Rotation);
					m_ListenerForward = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
					m_ListenerRight = glm::vec3(rot * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
					break;
				}
			}

			// 2. Update all active voices spatial state relative to listener
			m_VoicePool.UpdateSpatialAll(m_ListenerPosition, m_ListenerRight, static_cast<float>(m_SampleRate));
		}
	}

	VoiceHandle AudioSystem::PostEvent(
		const std::string& eventId,
		const glm::vec3& worldPos,
		float volumeScale,
		float pitchScale,
		bool looping)
	{
		const SoundEvent* evt = FindSoundEvent(eventId);
		if (!evt) return kInvalidVoiceHandle;

		std::unordered_map<std::string, std::string> switchParams;
		std::unordered_map<std::string, float> blendParams;
		std::string chosenAsset = evt->GetRootContainer() ?
			evt->GetRootContainer()->EvaluateSelection(switchParams, blendParams) : "";

		if (chosenAsset.empty()) return kInvalidVoiceHandle;

		auto buffer = FindAudioBuffer(chosenAsset);
		if (!buffer) return kInvalidVoiceHandle;

		float finalVol = evt->EvaluateRandomVolume() * volumeScale;
		float finalPitch = evt->EvaluateRandomPitch() * pitchScale;

		VoiceHandle h = m_VoicePool.AllocateVoice(
			buffer,
			evt->GetBusId(),
			finalVol,
			finalPitch,
			looping,
			128,
			evt->GetStealPolicy());

		if (h != kInvalidVoiceHandle)
		{
			if (auto* v = m_VoicePool.FindVoice(h))
			{
				const auto& sp = evt->GetSpatial();
				if (sp.is3D)
				{
					v->SetSpatialParameters(
						worldPos,
						glm::vec3(0.0f, 0.0f, -1.0f),
						sp.minDistance,
						sp.maxDistance,
						sp.model,
						sp.innerAngle,
						sp.outerAngle,
						sp.outerGain);
				}
			}
		}

		return h;
	}

	VoiceHandle AudioSystem::PostEvent2D(
		const std::string& eventId,
		float volumeScale,
		float pitchScale,
		bool looping)
	{
		return PostEvent(eventId, glm::vec3(0.0f), volumeScale, pitchScale, looping);
	}

	void AudioSystem::StopVoice(VoiceHandle handle, float fadeSeconds)
	{
		m_VoicePool.StopVoice(handle, fadeSeconds);
	}

	void AudioSystem::PauseVoice(VoiceHandle handle)
	{
		m_VoicePool.PauseVoice(handle);
	}

	void AudioSystem::SetVoiceLooping(VoiceHandle handle, bool loop)
	{
		m_VoicePool.SetVoiceLooping(handle, loop);
	}

	void AudioSystem::ResumeVoice(VoiceHandle handle)
	{
		m_VoicePool.ResumeVoice(handle);
	}

	void AudioSystem::StopAll(float fadeSeconds)
	{
		m_VoicePool.StopAllVoices(fadeSeconds);
		m_Music.Stop(fadeSeconds);
	}

	void AudioSystem::SetBusVolume(BusId busId, float volume)
	{
		m_Mixer.SetBusVolume(busId, volume);
	}

	void AudioSystem::SetBusMute(BusId busId, bool mute)
	{
		m_Mixer.SetBusMute(busId, mute);
	}

	void AudioSystem::SetBusSolo(BusId busId, bool solo)
	{
		m_Mixer.SetBusSolo(busId, solo);
	}

	void AudioSystem::TriggerSnapshot(const std::string& snapshotName, float transitionSeconds)
	{
		m_Mixer.TriggerSnapshot(snapshotName, transitionSeconds);
	}

	void AudioSystem::ClearSnapshot(float transitionSeconds)
	{
		m_Mixer.ClearSnapshot(transitionSeconds);
	}

	void AudioSystem::RegisterSoundEvent(const SoundEvent& soundEvent)
	{
		std::lock_guard<std::mutex> lock(m_RegistryMutex);
		m_SoundEvents[soundEvent.GetId()] = soundEvent;
	}

	const SoundEvent* AudioSystem::FindSoundEvent(const std::string& eventId) const
	{
		std::lock_guard<std::mutex> lock(m_RegistryMutex);
		auto it = m_SoundEvents.find(eventId);
		return (it != m_SoundEvents.end()) ? &it->second : nullptr;
	}

	std::vector<std::string> AudioSystem::GetRegisteredSoundEventIds() const
	{
		std::lock_guard<std::mutex> lock(m_RegistryMutex);
		std::vector<std::string> ids;
		ids.reserve(m_SoundEvents.size());
		for (const auto& [id, _] : m_SoundEvents)
		{
			ids.push_back(id);
		}
		std::sort(ids.begin(), ids.end());
		return ids;
	}

	void AudioSystem::RegisterAudioBuffer(const std::string& path, std::shared_ptr<AudioBuffer> buffer)
	{
		std::lock_guard<std::mutex> lock(m_RegistryMutex);
		m_AudioBuffers[path] = std::move(buffer);
	}

	std::shared_ptr<AudioBuffer> AudioSystem::FindAudioBuffer(const std::string& path) const
	{
		{
			std::lock_guard<std::mutex> lock(m_RegistryMutex);
			auto it = m_AudioBuffers.find(path);
			if (it != m_AudioBuffers.end())
			{
				return it->second;
			}
		}

		std::string normalizedPath = path;
		while (!normalizedPath.empty() && (normalizedPath.front() == '/' || normalizedPath.front() == '\\'))
		{
			normalizedPath.erase(normalizedPath.begin());
		}

		const std::vector<std::string> candidates = {
			path,
			normalizedPath,
			"assets/" + normalizedPath,
			"assets/audio/" + normalizedPath,
			"Engine/assets/" + normalizedPath,
			"Editor/assets/" + normalizedPath,
			"projects/assets/" + normalizedPath,
			"templates/project-example/assets/" + normalizedPath,
			"templates/project-example/assets/audio/" + normalizedPath
		};

		std::error_code ec;
		for (const auto& candidate : candidates)
		{
			if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
			{
				auto loaded = AudioCooker::LoadCookedFile(candidate);
				if (loaded)
				{
					std::lock_guard<std::mutex> lock(m_RegistryMutex);
					m_AudioBuffers[path] = loaded;
					return loaded;
				}
			}
		}

		return nullptr;
	}
}
