#include "AudioAcoustics.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace World::Audio
{
	constexpr float kPi = 3.14159265358979323846f;

	bool CalculatePortalDiffraction(
		const glm::vec3& listenerPos,
		const glm::vec3& emitterPos,
		const AcousticPortal& portal,
		glm::vec3& outVirtualEmitterPos,
		float& outDiffractionFactor,
		float& outLowPassCutoff)
	{
		if (!portal.isOpen)
		{
			outVirtualEmitterPos = emitterPos;
			outDiffractionFactor = 0.0f;
			outLowPassCutoff = 350.0f;
			return false;
		}

		// Vector from portal center to emitter and listener
		glm::vec3 toEmitter = emitterPos - portal.position;
		glm::vec3 toListener = listenerPos - portal.position;

		float distEmitter = glm::length(toEmitter);
		float distListener = glm::length(toListener);

		if (distEmitter < 1e-4f || distListener < 1e-4f)
		{
			outVirtualEmitterPos = portal.position;
			outDiffractionFactor = 1.0f;
			outLowPassCutoff = 20000.0f;
			return true;
		}

		glm::vec3 dirToEmitter = toEmitter / distEmitter;
		glm::vec3 dirToListener = toListener / distListener;

		// Calculate virtual emitter location clamped on the portal aperture rectangle
		glm::vec3 portalRight = glm::normalize(glm::cross(portal.normal, glm::vec3(0.0f, 1.0f, 0.0f)));
		if (glm::length(portalRight) < 0.1f)
		{
			portalRight = glm::normalize(glm::cross(portal.normal, glm::vec3(1.0f, 0.0f, 0.0f)));
		}
		glm::vec3 portalUp = glm::normalize(glm::cross(portalRight, portal.normal));

		float halfW = std::max(0.1f, portal.width * 0.5f);
		float halfH = std::max(0.1f, portal.height * 0.5f);

		float localX = glm::clamp(glm::dot(toEmitter, portalRight), -halfW, halfW);
		float localY = glm::clamp(glm::dot(toEmitter, portalUp), -halfH, halfH);

		outVirtualEmitterPos = portal.position + portalRight * localX + portalUp * localY;

		// Calculate diffraction turning angle theta between path through portal
		glm::vec3 path1 = glm::normalize(outVirtualEmitterPos - listenerPos);
		glm::vec3 path2 = glm::normalize(emitterPos - outVirtualEmitterPos);

		float dotVal = glm::clamp(glm::dot(path1, path2), -1.0f, 1.0f);
		float theta = std::acos(dotVal); // 0 = straight line, pi = sharp 180 bend

		// Diffraction attenuation model: cos(theta/2)^2
		float halfAngle = theta * 0.5f;
		float cosHalf = std::cos(halfAngle);
		outDiffractionFactor = std::clamp(cosHalf * cosHalf, 0.05f, 1.0f);

		// Low-pass filter down to 600Hz as sound bends further around obstacle
		outLowPassCutoff = 600.0f + (20000.0f - 600.0f) * outDiffractionFactor;
		return true;
	}

	void CalculateVbapSurround(
		const glm::vec3& emitterRelPos,
		SpeakerLayout8& outGains)
	{
		float dist = glm::length(glm::vec2(emitterRelPos.x, emitterRelPos.z));
		if (dist < 1e-4f)
		{
			// Omnidirectional Center / Left / Right
			outGains.left = 0.5f;
			outGains.right = 0.5f;
			outGains.center = 0.7071f;
			outGains.lfe = 0.5f;
			outGains.sideLeft = 0.3f;
			outGains.sideRight = 0.3f;
			outGains.rearLeft = 0.3f;
			outGains.rearRight = 0.3f;
			return;
		}

		// Azimuth in radians: 0 is forward (-Z), positive is right (+X), negative is left (-X)
		float azimuth = std::atan2(emitterRelPos.x, -emitterRelPos.z);

		// Reset all
		outGains = SpeakerLayout8{};

		// Angular positions of speakers (radians)
		// Center: 0 rad
		// Front Left: -0.5236 rad (-30 deg), Front Right: +0.5236 rad (+30 deg)
		// Side Left: -1.5708 rad (-90 deg), Side Right: +1.5708 rad (+90 deg)
		// Rear Left: -2.3562 rad (-135 deg), Rear Right: +2.3562 rad (+135 deg)

		float cDist = std::abs(azimuth);
		if (cDist < 0.5236f)
		{
			float t = cDist / 0.5236f;
			outGains.center = std::cos(t * 1.570796f);
			if (azimuth < 0.0f) outGains.left = std::sin(t * 1.570796f);
			else outGains.right = std::sin(t * 1.570796f);
		}
		else if (cDist < 1.5708f)
		{
			float t = (cDist - 0.5236f) / (1.5708f - 0.5236f);
			float fGain = std::cos(t * 1.570796f);
			float sGain = std::sin(t * 1.570796f);
			if (azimuth < 0.0f)
			{
				outGains.left = fGain;
				outGains.sideLeft = sGain;
			}
			else
			{
				outGains.right = fGain;
				outGains.sideRight = sGain;
			}
		}
		else
		{
			float t = std::clamp((cDist - 1.5708f) / (kPi - 1.5708f), 0.0f, 1.0f);
			float sGain = std::cos(t * 1.570796f);
			float rGain = std::sin(t * 1.570796f);
			if (azimuth < 0.0f)
			{
				outGains.sideLeft = sGain;
				outGains.rearLeft = rGain;
			}
			else
			{
				outGains.sideRight = sGain;
				outGains.rearRight = rGain;
			}
		}

		// Subwoofer (LFE): low-frequency reinforcement
		outGains.lfe = std::clamp(1.0f / (1.0f + 0.1f * dist), 0.1f, 0.7f);
	}

	EarlyReflectionsResult CalculateEarlyReflections(
		const glm::vec3& listenerPos,
		const std::array<float, 6>& wallDistances)
	{
		EarlyReflectionsResult res;

		float width = std::max(1.0f, wallDistances[0] + wallDistances[1]);
		float height = std::max(1.0f, wallDistances[2] + wallDistances[3]);
		float length = std::max(1.0f, wallDistances[4] + wallDistances[5]);

		res.estimatedRoomVolume = width * height * length;

		float minDist = 1e9f;
		for (float d : wallDistances)
		{
			if (d > 0.1f && d < minDist) minDist = d;
		}
		if (minDist > 1e8f) minDist = 5.0f;

		// Delay: 2 * minDist / speedOfSound
		res.delayMs = (2.0f * minDist / 343.0f) * 1000.0f;
		res.delayMs = std::clamp(res.delayMs, 4.0f, 150.0f);

		// Gain drops with room volume and wall distance
		res.gain = std::clamp(0.40f / std::sqrt(1.0f + 0.04f * minDist), 0.05f, 0.50f);
		return res;
	}

	std::vector<ClusteredEmitterResult> ClusterDistantEmitters(
		const std::vector<AudioEmitterClusterItem>& emitters,
		const glm::vec3& listenerPos,
		float clusterThresholdDistance,
		float clusterRadius)
	{
		std::vector<ClusteredEmitterResult> output;
		if (emitters.empty()) return output;

		std::vector<AudioEmitterClusterItem> closeEmitters;
		std::unordered_map<std::string, std::vector<AudioEmitterClusterItem>> distantByType;

		for (const auto& em : emitters)
		{
			float dist = glm::distance(em.position, listenerPos);
			if (dist < clusterThresholdDistance)
			{
				closeEmitters.push_back(em);
			}
			else
			{
				distantByType[em.soundEvent].push_back(em);
			}
		}

		// 1. Close emitters pass through directly (unclustered)
		for (const auto& em : closeEmitters)
		{
			output.push_back({ em.position, em.volume, em.soundEvent, 1 });
		}

		// 2. Distant emitters of same event clustered by spatial proximity
		for (auto& [evt, list] : distantByType)
		{
			std::vector<bool> visited(list.size(), false);
			for (size_t i = 0; i < list.size(); ++i)
			{
				if (visited[i]) continue;
				visited[i] = true;

				glm::vec3 centroid = list[i].position;
				double sumSqVol = list[i].volume * list[i].volume;
				uint32_t count = 1;

				for (size_t j = i + 1; j < list.size(); ++j)
				{
					if (visited[j]) continue;
					if (glm::distance(list[i].position, list[j].position) <= clusterRadius)
					{
						visited[j] = true;
						centroid += list[j].position;
						sumSqVol += list[j].volume * list[j].volume;
						++count;
					}
				}

				centroid /= static_cast<float>(count);
				float combinedVol = std::min(2.5f, static_cast<float>(std::sqrt(sumSqVol)));
				output.push_back({ centroid, combinedVol, evt, count });
			}
		}

		return output;
	}

	void HdrAudioSystem::PostHdrEvent(float splDb) noexcept
	{
		if (splDb > m_TopSplDb)
		{
			m_TopSplDb = splDb;
		}
	}

	void HdrAudioSystem::Update(float dtSeconds) noexcept
	{
		float baseline = 60.0f; // Room ambient baseline
		if (m_TopSplDb > baseline)
		{
			m_TopSplDb = std::max(baseline, m_TopSplDb - m_ReleaseSpeed * dtSeconds);
		}
	}

	float HdrAudioSystem::GetCurrentCompressionGain() const noexcept
	{
		float headroom = std::max(0.0f, m_TopSplDb - 85.0f);
		if (headroom <= 0.0f) return 1.0f;
		return 1.0f / (1.0f + 0.05f * headroom);
	}

	std::vector<LipSyncFrame> GenerateLipSyncFrames(
		const AudioBuffer& buffer,
		float frameIntervalSeconds)
	{
		std::vector<LipSyncFrame> frames;
		uint32_t sampleRate = buffer.GetSampleRate();
		uint32_t totalSamples = buffer.GetFrameCount();
		if (sampleRate == 0 || totalSamples == 0) return frames;

		uint32_t stepFrames = std::max(64u, static_cast<uint32_t>(frameIntervalSeconds * static_cast<float>(sampleRate)));
		uint32_t windowFrames = stepFrames * 2;

		float smoothJaw = 0.0f;
		float smoothSmile = 0.0f;

		for (uint32_t start = 0; start < totalSamples; start += stepFrames)
		{
			uint32_t end = std::min(totalSamples, start + windowFrames);
			uint32_t count = end - start;
			if (count == 0) break;

			double sumSq = 0.0;
			uint32_t zeroCrossings = 0;
			float prevSample = buffer.GetSample(0, start);

			for (uint32_t i = start; i < end; ++i)
			{
				float s = buffer.GetSample(0, i);
				sumSq += s * s;
				if ((prevSample >= 0.0f && s < 0.0f) || (prevSample < 0.0f && s >= 0.0f))
				{
					++zeroCrossings;
				}
				prevSample = s;
			}

			float rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(count)));
			float zcr = static_cast<float>(zeroCrossings) / static_cast<float>(count);

			// Vowels (low ZCR, high RMS) drive jaw open
			float targetJaw = std::clamp((rms - 0.02f) * 4.5f, 0.0f, 1.0f);
			if (zcr > 0.25f) targetJaw *= 0.5f;

			// Fricatives/sibilants (high ZCR) drive mouth smile
			float targetSmile = (zcr > 0.20f) ? std::clamp(rms * 5.0f, 0.0f, 1.0f) : 0.0f;

			// 1-pole EMA smoothing
			smoothJaw = 0.65f * smoothJaw + 0.35f * targetJaw;
			smoothSmile = 0.65f * smoothSmile + 0.35f * targetSmile;

			LipSyncFrame kf;
			kf.timeSeconds = static_cast<float>(start) / static_cast<float>(sampleRate);
			kf.jawOpen = smoothJaw;
			kf.mouthSmile = smoothSmile;
			kf.mouthFunnel = smoothJaw * 0.6f;
			kf.mouthPucker = smoothSmile * 0.4f;

			frames.push_back(kf);
		}

		return frames;
	}

	float CalculateHeadRadiationFactor(
		const glm::vec3& speakerPos,
		const glm::vec3& speakerForward,
		const glm::vec3& listenerPos,
		float& outLowPassCutoff)
	{
		glm::vec3 toListener = listenerPos - speakerPos;
		float dist = glm::length(toListener);
		if (dist < 1e-4f)
		{
			outLowPassCutoff = 20000.0f;
			return 1.0f;
		}

		glm::vec3 dir = toListener / dist;
		float dot = glm::clamp(glm::dot(glm::normalize(speakerForward), dir), -1.0f, 1.0f);

		// Front (+1): full transmission 1.0, 20kHz
		// Back (-1): -6dB transmission (0.5), 3500Hz
		float t = (dot + 1.0f) * 0.5f; // 0.0 (behind) to 1.0 (in front)
		float gain = 0.5f + 0.5f * t;
		outLowPassCutoff = 3500.0f + (20000.0f - 3500.0f) * (t * t);
		return gain;
	}

	void NoiseGate::Configure(
		float thresholdDb,
		float attackMs,
		float releaseMs,
		float holdMs) noexcept
	{
		m_ThresholdLinear = std::pow(10.0f, thresholdDb / 20.0f);
		float sampleRate = 48000.0f;
		m_AttackCoeff = std::clamp(1.0f - std::exp(-1.0f / (attackMs * 0.001f * sampleRate)), 0.01f, 1.0f);
		m_ReleaseCoeff = std::clamp(1.0f - std::exp(-1.0f / (releaseMs * 0.001f * sampleRate)), 0.0001f, 0.1f);
		m_HoldFrames = holdMs * 0.001f * sampleRate;
	}

	void NoiseGate::ProcessStereo(float* left, float* right, uint32_t frameCount, float sampleRate) noexcept
	{
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			float level = std::max(std::abs(left[i]), std::abs(right[i]));
			if (level >= m_ThresholdLinear)
			{
				m_HoldCounter = m_HoldFrames;
				m_GateGain += (1.0f - m_GateGain) * m_AttackCoeff;
			}
			else
			{
				if (m_HoldCounter > 0.0f)
				{
					m_HoldCounter -= 1.0f;
				}
				else
				{
					m_GateGain += (0.0f - m_GateGain) * m_ReleaseCoeff;
				}
			}

			left[i] *= m_GateGain;
			right[i] *= m_GateGain;
		}
	}

	void ProceduralWeatherSystem::Update(
		float windSpeedKmH,
		float rainIntensity01,
		float stormProbability01,
		float dtSeconds) noexcept
	{
		float normWind = std::clamp(windSpeedKmH / 100.0f, 0.0f, 1.5f);
		m_WindGain = normWind * 0.8f;
		m_WindPitch = 0.8f + normWind * 0.4f;

		m_RainGain = std::clamp(rainIntensity01, 0.0f, 1.0f);

		m_ThunderTimer += dtSeconds;
		if (stormProbability01 > 0.2f && m_ThunderTimer >= m_ThunderCooldown)
		{
			// Thunder cooldown reset
			m_ThunderCooldown = 8.0f + (1.0f - stormProbability01) * 20.0f;
			m_ThunderTimer = 0.0f;
		}
	}

	bool ProceduralWeatherSystem::CheckThunderTrigger() noexcept
	{
		if (m_ThunderTimer == 0.0f && m_RainGain > 0.3f)
		{
			return true;
		}
		return false;
	}
}
