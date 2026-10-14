#include "AudioSystem.h"
#include <random>
#include "AudioCooker.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include "AudioNullBackend.h"
#include "MiniaudioBackend.h"
#include "AudioComponents.h"
#include "World/Core/StringPool.h"
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
		m_CommandQueue.Clear();
		m_EntityVoiceMap.clear();
		m_EntityLastPosition.clear();
		m_ActiveZoneSnapshotId = 0;
		m_ActiveZoneReverbSend = 0.0f;
		m_CurrentZoneReverbSend = 0.0f;
		m_ListenerVelocity = glm::vec3(0.0f);
		m_LastListenerPosition = glm::vec3(0.0f);
		m_HasLastListenerPos = false;
		m_TotalRenderedFrames.store(0, std::memory_order_relaxed);
		m_TotalTimeSeconds = 0.0f;
		m_ComboEscalators.clear();
		m_RapidFireTrackers.clear();

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

		// Pre-allocate fixed-capacity scratch buffers for zero-allocation mixing callback
		m_BusMixBuffers.clear();
		const uint32_t allocFrames = std::max(m_BufferFrames, kDefaultMixBlockFrames);
		const BusId standardBuses[] = { kBusMaster, kBusMusic, kBusSfx, kBusVoice, kBusUi, kBusAmbience };
		for (BusId bid : standardBuses)
		{
			m_BusMixBuffers[bid].first.assign(allocFrames, 0.0f);
			m_BusMixBuffers[bid].second.assign(allocFrames, 0.0f);
		}
		for (const auto& [bid, bus] : m_Mixer.GetBuses())
		{
			m_BusMixBuffers[bid].first.assign(allocFrames, 0.0f);
			m_BusMixBuffers[bid].second.assign(allocFrames, 0.0f);
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
		m_EntityVoiceMap.clear();
		m_EntityLastPosition.clear();
		m_CommandQueue.Clear();
		m_TotalTimeSeconds = 0.0f;
		m_ComboEscalators.clear();
		m_RapidFireTrackers.clear();
		m_Initialized = false;
	}

	void AudioSystem::ProcessCommandQueue()
	{
		AudioCommand cmd;
		while (m_CommandQueue.Pop(cmd))
		{
			ExecuteCommand(cmd);
		}
	}

	void AudioSystem::ExecuteCommand(const AudioCommand& cmd)
	{
		switch (cmd.type)
		{
		case AudioCommandType::StopVoice:
			m_VoicePool.StopVoice(cmd.voiceHandle, cmd.floatParam0);
			break;
		case AudioCommandType::PauseVoice:
			m_VoicePool.PauseVoice(cmd.voiceHandle);
			break;
		case AudioCommandType::ResumeVoice:
			m_VoicePool.ResumeVoice(cmd.voiceHandle);
			break;
		case AudioCommandType::SetVoiceVolume:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
				v->SetVolume(cmd.floatParam0);
			break;
		case AudioCommandType::SetVoicePitch:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
				v->SetPitch(cmd.floatParam0);
			break;
		case AudioCommandType::SetVoiceSpatialPosition:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
			{
				v->SetSpatialParameters(
					cmd.vecParam,
					glm::vec3(0.0f, 0.0f, -1.0f),
					cmd.floatParam0 > 0.0f ? cmd.floatParam0 : 1.0f,
					cmd.floatParam1 > 0.0f ? cmd.floatParam1 : 25.0f,
					AttenuationModel::InverseSquare);
			}
			break;
		case AudioCommandType::SetVoiceSpatial2DPosition:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
			{
				v->SetSpatial2DParameters(
					glm::vec2(cmd.vecParam.x, cmd.vecParam.y),
					glm::vec2(m_ListenerPosition.x, m_ListenerPosition.y),
					cmd.floatParam0 > 0.0f ? cmd.floatParam0 : 10.0f,
					cmd.floatParam1 > 0.0f ? cmd.floatParam1 : 1.0f,
					cmd.floatParam2 > 0.0f ? cmd.floatParam2 : 25.0f);
			}
			break;
		case AudioCommandType::SetVoiceOcclusion:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
				v->SetOcclusion(cmd.floatParam0);
			break;
		case AudioCommandType::SetVoiceVolumetricRadius:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
				v->SetVolumetricRadius(cmd.floatParam0);
			break;
		case AudioCommandType::SetVoiceVelocity:
			if (auto* v = m_VoicePool.FindVoice(cmd.voiceHandle))
				v->SetVelocity(cmd.vecParam);
			break;
		case AudioCommandType::SetBusVolume:
			m_Mixer.SetBusVolume(cmd.busId, cmd.floatParam0);
			break;
		case AudioCommandType::SetBusMute:
			m_Mixer.SetBusMute(cmd.busId, cmd.flags != 0);
			break;
		case AudioCommandType::SetBusSolo:
			m_Mixer.SetBusSolo(cmd.busId, cmd.flags != 0);
			break;
		case AudioCommandType::ClearSnapshot:
			m_Mixer.ClearSnapshot(cmd.floatParam0);
			break;
		case AudioCommandType::StopAll:
			m_VoicePool.StopAllVoices(cmd.floatParam0);
			break;
		default:
			break;
		}
	}

	bool AudioSystem::EnqueueCommand(const AudioCommand& cmd)
	{
		if (m_CommandQueue.Push(cmd))
		{
			return true;
		}
		// If queue is full, fallback execute immediately to avoid command drop
		ExecuteCommand(cmd);
		return false;
	}

	void AudioSystem::AudioRenderCallback(float* outLeft, float* outRight, uint32_t frameCount)
	{
		// 0. Drain command queue from main thread
		ProcessCommandQueue();
		m_TotalRenderedFrames.fetch_add(frameCount, std::memory_order_relaxed);

		// 1. Clear scratch bus buffers (zero-allocation: only std::fill, no assign/resize)
		for (auto& [bid, bufPair] : m_BusMixBuffers)
		{
			uint32_t count = std::min(static_cast<uint32_t>(bufPair.first.size()), frameCount);
			std::fill(bufPair.first.begin(), bufPair.first.begin() + count, 0.0f);
			std::fill(bufPair.second.begin(), bufPair.second.begin() + count, 0.0f);
		}

		// 2. Mix physical voices into their corresponding bus buffers
		m_VoicePool.MixVoicesIntoBuses(m_BusMixBuffers, frameCount, static_cast<float>(m_SampleRate));

		// 3. Process mixer bus graph, DSP chains, snapshots, and true-peak limiting
		m_Mixer.ProcessGraph(m_BusMixBuffers, outLeft, outRight, frameCount, static_cast<float>(m_SampleRate));

		// 4. Advance DSP sample clock for interactive music
		m_Music.AdvanceDspClock(frameCount, m_SampleRate);
	}

	void AudioSystem::Update(float deltaSeconds, Scene* activeScene)
	{
		if (!m_Initialized) return;

		m_TotalTimeSeconds += deltaSeconds;

		m_Music.Update(deltaSeconds);

		// Sync ECS Listener, Source, and Zone components if scene is provided
		if (activeScene)
		{
			const auto& reg = static_cast<const Scene*>(activeScene)->GetRegistry();

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

			// Track listener velocity
			if (deltaSeconds > 0.0001f && m_HasLastListenerPos)
			{
				m_ListenerVelocity = (m_ListenerPosition - m_LastListenerPosition) / deltaSeconds;
			}
			else
			{
				m_ListenerVelocity = glm::vec3(0.0f);
			}
			m_LastListenerPosition = m_ListenerPosition;
			m_HasLastListenerPos = true;

			// 2. Process AudioSourceComponent lifecycle and 3D transform sync
			auto sourceView = reg.view<AudioSourceComponent, TransformComponent>();
			std::unordered_set<uint32_t> activeEntities;
			activeEntities.reserve(sourceView.size_hint());

			for (auto entity : sourceView)
			{
				uint32_t entityId = static_cast<uint32_t>(entity);
				activeEntities.insert(entityId);

				const auto& source = sourceView.get<AudioSourceComponent>(entity);
				const auto& transform = sourceView.get<TransformComponent>(entity);

				auto it = m_EntityVoiceMap.find(entityId);
				if (it == m_EntityVoiceMap.end())
				{
					// Not yet playing: check PlayOnAwake
					if (source.PlayOnAwake)
					{
						std::string soundPath;
						if (source.Sound.HasPath())
						{
							soundPath = StringPool::Get().PathOf(source.Sound.Path);
						}

						if (!soundPath.empty())
						{
							VoiceHandle handle = kInvalidVoiceHandle;
							if (source.Spatial3D)
							{
								handle = PostEvent(soundPath, transform.Location, source.Volume, source.Pitch, source.Looping);
							}
							else
							{
								handle = PostEvent2D(soundPath, source.Volume, source.Pitch, source.Looping);
							}

							if (handle != kInvalidVoiceHandle)
							{
								if (auto* voice = m_VoicePool.FindVoice(handle))
								{
									if (source.Spatial3D)
									{
										voice->SetSpatialParameters(
											transform.Location,
											glm::vec3(0.0f, 0.0f, -1.0f),
											source.MinDistance,
											source.MaxDistance,
											AttenuationModel::InverseSquare);
									}
								}
								m_EntityVoiceMap[entityId] = handle;
							}
						}
					}
				}
				else
				{
					// Voice exists: check if voice is still alive
					VoiceHandle handle = it->second;
					Voice* voice = m_VoicePool.FindVoice(handle);
					if (!voice || !voice->IsActive())
					{
						// Finished or stopped
						m_EntityVoiceMap.erase(it);
					}
					else if (source.Spatial3D)
					{
						// Still playing and 3D: sync transform position and velocity
						glm::vec3 sourceVel(0.0f);
						auto lastPosIt = m_EntityLastPosition.find(entityId);
						if (lastPosIt != m_EntityLastPosition.end() && deltaSeconds > 0.0001f)
						{
							sourceVel = (transform.Location - lastPosIt->second) / deltaSeconds;
						}
						m_EntityLastPosition[entityId] = transform.Location;

						voice->SetVelocity(sourceVel);

						if (m_OcclusionRaycastCallback)
						{
							float occ = m_OcclusionRaycastCallback(m_ListenerPosition, transform.Location);
							voice->SetOcclusion(std::clamp(occ, 0.0f, 1.0f));
						}

						voice->SetSpatialParameters(
							transform.Location,
							glm::vec3(0.0f, 0.0f, -1.0f),
							source.MinDistance,
							source.MaxDistance,
							AttenuationModel::InverseSquare);

						AudioCommand cmd{};
						cmd.type = AudioCommandType::SetVoiceSpatialPosition;
						cmd.voiceHandle = handle;
						cmd.vecParam = transform.Location;
						cmd.floatParam0 = source.MinDistance;
						cmd.floatParam1 = source.MaxDistance;
						m_CommandQueue.Push(cmd);
					}
					else if (voice->GetSpatialMode() == SpatialMode::Spatial2D)
					{
						glm::vec2 entityPos2D(transform.Location.x, transform.Location.y);
						glm::vec2 listenerPos2D(m_ListenerPosition.x, m_ListenerPosition.y);
						voice->SetSpatial2DParameters(
							entityPos2D,
							listenerPos2D,
							10.0f,
							source.MinDistance,
							source.MaxDistance,
							0.75f);

						AudioCommand cmd{};
						cmd.type = AudioCommandType::SetVoiceSpatial2DPosition;
						cmd.voiceHandle = handle;
						cmd.vecParam = glm::vec3(entityPos2D.x, entityPos2D.y, 0.0f);
						cmd.floatParam0 = 10.0f;
						cmd.floatParam1 = source.MinDistance;
						cmd.floatParam2 = source.MaxDistance;
						m_CommandQueue.Push(cmd);
					}
				}
			}

			// Clean up entities removed from view or destroyed
			for (auto it = m_EntityVoiceMap.begin(); it != m_EntityVoiceMap.end(); )
			{
				if (activeEntities.find(it->first) == activeEntities.end())
				{
					StopVoice(it->second, 0.05f);
					m_EntityLastPosition.erase(it->first);
					it = m_EntityVoiceMap.erase(it);
				}
				else
				{
					++it;
				}
			}

			// 3. Process AudioZoneComponent
			auto zoneView = reg.view<AudioZoneComponent, TransformComponent>();
			bool inZone = false;
			float targetReverbSend = 0.0f;
			for (auto entity : zoneView)
			{
				const auto& zone = zoneView.get<AudioZoneComponent>(entity);
				const auto& transform = zoneView.get<TransformComponent>(entity);
				if (!zone.Active) continue;

				glm::vec3 zonePos = transform.Location;
				glm::vec3 halfExtents(zone.HalfExtents[0], zone.HalfExtents[1], zone.HalfExtents[2]);
				glm::vec3 diff = glm::abs(m_ListenerPosition - zonePos);

				if (diff.x <= halfExtents.x && diff.y <= halfExtents.y && diff.z <= halfExtents.z)
				{
					inZone = true;
					targetReverbSend = zone.ReverbSendLevel;
					if (zone.SnapshotId != 0 && zone.SnapshotId != m_ActiveZoneSnapshotId)
					{
						m_ActiveZoneSnapshotId = zone.SnapshotId;
						std::string snapName = "Snapshot_" + std::to_string(zone.SnapshotId);
						float snapTrans = (m_ZoneTransitionRate > 0.0f) ? (1.0f / m_ZoneTransitionRate) : 0.3f;
						TriggerSnapshot(snapName, snapTrans);
					}
					break;
				}
			}

			if (!inZone && m_ActiveZoneSnapshotId != 0)
			{
				m_ActiveZoneSnapshotId = 0;
				float snapTrans = (m_ZoneTransitionRate > 0.0f) ? (1.0f / m_ZoneTransitionRate) : 0.3f;
				ClearSnapshot(snapTrans);
			}

			m_ActiveZoneReverbSend = targetReverbSend;
			float blendFactor = std::clamp(deltaSeconds * m_ZoneTransitionRate, 0.0f, 1.0f);
			m_CurrentZoneReverbSend += (m_ActiveZoneReverbSend - m_CurrentZoneReverbSend) * blendFactor;

			// 4. Update all active voices spatial state relative to listener
			Voice::SetListenerContext(m_ListenerForward, m_ListenerVelocity);
			m_VoicePool.UpdateSpatialAll(m_ListenerPosition, m_ListenerRight, static_cast<float>(m_SampleRate));
		}
		else
		{
			// If no active scene, smooth reverb send towards 0 and update listener velocity if position changed
			if (deltaSeconds > 0.0001f && m_HasLastListenerPos)
			{
				m_ListenerVelocity = (m_ListenerPosition - m_LastListenerPosition) / deltaSeconds;
			}
			else
			{
				m_ListenerVelocity = glm::vec3(0.0f);
			}
			m_LastListenerPosition = m_ListenerPosition;
			m_HasLastListenerPos = true;

			float blendFactor = std::clamp(deltaSeconds * m_ZoneTransitionRate, 0.0f, 1.0f);
			m_CurrentZoneReverbSend += (m_ActiveZoneReverbSend - m_CurrentZoneReverbSend) * blendFactor;

			Voice::SetListenerContext(m_ListenerForward, m_ListenerVelocity);
			m_VoicePool.UpdateSpatialAll(m_ListenerPosition, m_ListenerRight, static_cast<float>(m_SampleRate));
		}
	}

	VoiceHandle AudioSystem::PostEvent(
		const std::string& eventId,
		const glm::vec3& worldPos,
		float volumeScale,
		float pitchScale,
		bool looping,
		bool allowThrottling)
	{
		// 1. Rapid-fire throttling check
		if (allowThrottling && m_RapidFireThrottlingEnabled && !looping)
		{
			auto& tracker = m_RapidFireTrackers[eventId];
			if (tracker.lastTriggerTime >= 0.0f && (m_TotalTimeSeconds - tracker.lastTriggerTime) < m_RapidFireWindowSeconds)
			{
				tracker.triggerCountInWindow++;
				if (tracker.triggerCountInWindow > m_RapidFireThreshold)
				{
					if (tracker.lastVoiceHandle != kInvalidVoiceHandle)
					{
						if (auto* activeVoice = m_VoicePool.FindVoice(tracker.lastVoiceHandle))
						{
							if (activeVoice->IsActive())
							{
								float stackMultiplier = 1.0f + 0.3f * std::log10(static_cast<float>(tracker.triggerCountInWindow));
								activeVoice->SetVolume(std::min(4.0f, activeVoice->GetAudibility() * stackMultiplier));
								return tracker.lastVoiceHandle;
							}
						}
					}
				}
			}
			else
			{
				tracker.lastTriggerTime = m_TotalTimeSeconds;
				tracker.triggerCountInWindow = 1;
			}
		}

		const SoundEvent* evt = FindSoundEvent(eventId);
		if (!evt)
		{
			// Fallback: check if eventId is directly an audio buffer path
			auto buffer = FindAudioBuffer(eventId);
			if (buffer)
			{
				VoiceHandle h = m_VoicePool.AllocateVoice(
					buffer,
					kBusSfx,
					volumeScale,
					pitchScale,
					looping,
					128,
					StealPolicy::StealOldest);
				if (h != kInvalidVoiceHandle)
				{
					if (allowThrottling && m_RapidFireThrottlingEnabled && !looping)
					{
						m_RapidFireTrackers[eventId].lastVoiceHandle = h;
					}
					if (auto* v = m_VoicePool.FindVoice(h))
					{
						if (m_PhaseJitterEnabled && !looping && buffer->GetFrameCount() > 512)
						{
							static std::mt19937 s_JitterRng(1337);
							uint32_t maxJitter = std::min(256u, buffer->GetFrameCount() / 4);
							if (maxJitter > 0)
							{
								std::uniform_int_distribution<uint32_t> dist(0, maxJitter);
								v->SetCursorFrame(static_cast<double>(dist(s_JitterRng)));
							}
						}
						if (worldPos != glm::vec3(0.0f))
						{
							v->SetSpatialParameters(
								worldPos,
								glm::vec3(0.0f, 0.0f, -1.0f),
								1.0f,
								25.0f,
								AttenuationModel::InverseSquare);
						}
					}
				}
				return h;
			}
			return kInvalidVoiceHandle;
		}

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
			if (allowThrottling && m_RapidFireThrottlingEnabled && !looping)
			{
				m_RapidFireTrackers[eventId].lastVoiceHandle = h;
			}
			if (auto* v = m_VoicePool.FindVoice(h))
			{
				if (m_PhaseJitterEnabled && !looping && buffer->GetFrameCount() > 512)
				{
					static std::mt19937 s_JitterRng(1337);
					uint32_t maxJitter = std::min(256u, buffer->GetFrameCount() / 4);
					if (maxJitter > 0)
					{
						std::uniform_int_distribution<uint32_t> dist(0, maxJitter);
						v->SetCursorFrame(static_cast<double>(dist(s_JitterRng)));
					}
				}

				const auto& sp = evt->GetSpatial();
				if (sp.mode == SpatialMode::Spatial2D)
				{
					glm::vec2 listener2D(m_ListenerPosition.x, m_ListenerPosition.y);
					v->SetSpatial2DParameters(
						glm::vec2(worldPos.x, worldPos.y),
						listener2D,
						sp.halfViewWidth > 0.0f ? sp.halfViewWidth : 10.0f,
						sp.minDistance,
						sp.maxDistance,
						sp.maxPan);
				}
				else if (sp.is3D || sp.mode == SpatialMode::Spatial3D)
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
		bool looping,
		bool allowThrottling)
	{
		return PostEvent(eventId, glm::vec3(0.0f), volumeScale, pitchScale, looping, allowThrottling);
	}

	VoiceHandle AudioSystem::PostEvent2D(
		const std::string& eventId,
		const glm::vec2& screenPos,
		float volumeScale,
		float pitchScale,
		bool looping,
		bool allowThrottling)
	{
		VoiceHandle h = PostEvent(eventId, glm::vec3(screenPos.x, screenPos.y, 0.0f), volumeScale, pitchScale, looping, allowThrottling);
		if (h != kInvalidVoiceHandle)
		{
			if (auto* v = m_VoicePool.FindVoice(h))
			{
				const SoundEvent* evt = FindSoundEvent(eventId);
				float halfViewWidth = 10.0f;
				float minDist = 1.0f;
				float maxDist = 25.0f;
				float maxPan = 0.75f;
				if (evt)
				{
					const auto& sp = evt->GetSpatial();
					if (sp.halfViewWidth > 0.0f) halfViewWidth = sp.halfViewWidth;
					minDist = sp.minDistance;
					maxDist = sp.maxDistance;
					maxPan = sp.maxPan;
				}
				glm::vec2 listener2D(m_ListenerPosition.x, m_ListenerPosition.y);
				v->SetSpatial2DParameters(screenPos, listener2D, halfViewWidth, minDist, maxDist, maxPan);
			}
		}
		return h;
	}

	VoiceHandle AudioSystem::PostComboEvent(
		const std::string& eventId,
		const std::string& comboGroup,
		const glm::vec2& screenPos,
		float volumeScale)
	{
		auto& escalator = m_ComboEscalators[comboGroup];
		if (escalator.currentCombo > 0 && (m_TotalTimeSeconds - escalator.lastTriggerTime) < escalator.timeoutSeconds)
		{
			escalator.currentCombo++;
		}
		else
		{
			escalator.currentCombo = 1;
		}

		escalator.lastTriggerTime = m_TotalTimeSeconds;

		float pitchScale = std::min(
			escalator.maxPitchRatio,
			std::pow(2.0f, (static_cast<float>(escalator.currentCombo) * escalator.stepSemitones) / 12.0f));

		if (screenPos != glm::vec2(0.0f))
		{
			return PostEvent2D(eventId, screenPos, volumeScale, pitchScale, false, false);
		}
		return PostEvent2D(eventId, volumeScale, pitchScale, false, false);
	}

	const ComboEscalator* AudioSystem::GetComboEscalator(const std::string& comboGroup) const
	{
		auto it = m_ComboEscalators.find(comboGroup);
		return (it != m_ComboEscalators.end()) ? &it->second : nullptr;
	}

	void AudioSystem::ResetComboEscalator(const std::string& comboGroup)
	{
		auto it = m_ComboEscalators.find(comboGroup);
		if (it != m_ComboEscalators.end())
		{
			it->second.currentCombo = 0;
			it->second.lastTriggerTime = -100.0f;
		}
	}

	void AudioSystem::ConfigureComboEscalator(
		const std::string& comboGroup,
		float stepSemitones,
		float maxPitchRatio,
		float timeoutSeconds)
	{
		auto& escalator = m_ComboEscalators[comboGroup];
		escalator.stepSemitones = stepSemitones;
		escalator.maxPitchRatio = maxPitchRatio;
		escalator.timeoutSeconds = timeoutSeconds;
	}

	void AudioSystem::SetVoiceVolumetricRadius(VoiceHandle handle, float radius)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetVoiceVolumetricRadius;
		cmd.voiceHandle = handle;
		cmd.floatParam0 = radius;
		if (!m_CommandQueue.Push(cmd))
		{
			if (auto* v = m_VoicePool.FindVoice(handle))
				v->SetVolumetricRadius(radius);
		}
	}

	void AudioSystem::SetVoiceVelocity(VoiceHandle handle, const glm::vec3& velocity)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetVoiceVelocity;
		cmd.voiceHandle = handle;
		cmd.vecParam = velocity;
		if (!m_CommandQueue.Push(cmd))
		{
			if (auto* v = m_VoicePool.FindVoice(handle))
				v->SetVelocity(velocity);
		}
	}

	void AudioSystem::SetVoiceOcclusion(VoiceHandle handle, float occlusion01)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetVoiceOcclusion;
		cmd.voiceHandle = handle;
		cmd.floatParam0 = occlusion01;
		if (!m_CommandQueue.Push(cmd))
		{
			if (auto* v = m_VoicePool.FindVoice(handle))
				v->SetOcclusion(occlusion01);
		}
	}

	double AudioSystem::GetDspTime() const noexcept
	{
		if (m_SampleRate == 0) return 0.0;
		return static_cast<double>(m_TotalRenderedFrames.load(std::memory_order_relaxed)) / static_cast<double>(m_SampleRate);
	}

	void AudioSystem::HandlePhysicsContact(
		const glm::vec3& contactPoint,
		float normalImpulse,
		const std::string& soundEvent)
	{
		if (normalImpulse < m_PhysicsContactThreshold)
		{
			return;
		}

		float excess = normalImpulse - m_PhysicsContactThreshold;
		float norm = excess / 20.0f;
		float volume = std::clamp(0.25f + 0.75f * std::log10(1.0f + 9.0f * std::min(norm, 1.0f)) + std::max(0.0f, norm - 1.0f) * 0.2f, 0.1f, 3.0f);
		float pitch = std::clamp(0.9f + 0.15f * std::min(norm, 1.0f), 0.7f, 1.4f);

		PostEvent(soundEvent, contactPoint, volume, pitch);
	}

	void AudioSystem::StopVoice(VoiceHandle handle, float fadeSeconds)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::StopVoice;
		cmd.voiceHandle = handle;
		cmd.floatParam0 = fadeSeconds;
		if (!m_CommandQueue.Push(cmd))
		{
			m_VoicePool.StopVoice(handle, fadeSeconds);
		}
	}

	void AudioSystem::PauseVoice(VoiceHandle handle)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::PauseVoice;
		cmd.voiceHandle = handle;
		if (!m_CommandQueue.Push(cmd))
		{
			m_VoicePool.PauseVoice(handle);
		}
	}

	void AudioSystem::SetVoiceLooping(VoiceHandle handle, bool loop)
	{
		m_VoicePool.SetVoiceLooping(handle, loop);
	}

	void AudioSystem::ResumeVoice(VoiceHandle handle)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::ResumeVoice;
		cmd.voiceHandle = handle;
		if (!m_CommandQueue.Push(cmd))
		{
			m_VoicePool.ResumeVoice(handle);
		}
	}

	void AudioSystem::StopAll(float fadeSeconds)
	{
		AudioCommand cmd{};
		cmd.type = AudioCommandType::StopAll;
		cmd.floatParam0 = fadeSeconds;
		if (!m_CommandQueue.Push(cmd))
		{
			m_VoicePool.StopAllVoices(fadeSeconds);
		}
		m_Music.Stop(fadeSeconds);
	}

	void AudioSystem::SetBusVolume(BusId busId, float volume)
	{
		m_Mixer.SetBusVolume(busId, volume);
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetBusVolume;
		cmd.busId = busId;
		cmd.floatParam0 = volume;
		m_CommandQueue.Push(cmd);
	}

	void AudioSystem::SetBusMute(BusId busId, bool mute)
	{
		m_Mixer.SetBusMute(busId, mute);
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetBusMute;
		cmd.busId = busId;
		cmd.flags = mute ? 1 : 0;
		m_CommandQueue.Push(cmd);
	}

	void AudioSystem::SetBusSolo(BusId busId, bool solo)
	{
		m_Mixer.SetBusSolo(busId, solo);
		AudioCommand cmd{};
		cmd.type = AudioCommandType::SetBusSolo;
		cmd.busId = busId;
		cmd.flags = solo ? 1 : 0;
		m_CommandQueue.Push(cmd);
	}

	void AudioSystem::TriggerSnapshot(const std::string& snapshotName, float transitionSeconds)
	{
		m_Mixer.TriggerSnapshot(snapshotName, transitionSeconds);
	}

	void AudioSystem::ClearSnapshot(float transitionSeconds)
	{
		m_Mixer.ClearSnapshot(transitionSeconds);
		AudioCommand cmd{};
		cmd.type = AudioCommandType::ClearSnapshot;
		cmd.floatParam0 = transitionSeconds;
		m_CommandQueue.Push(cmd);
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
