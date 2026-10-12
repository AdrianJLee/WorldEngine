#include "MixerBus.h"

namespace World::Audio
{
	MixerBus::MixerBus(BusId id, std::string name, BusId parentId)
		: m_Id(id), m_Name(std::move(name)), m_ParentId(parentId)
	{
	}

	void MixerBus::UpdateMeter(float lPeak, float rPeak, float lRms, float rRms) noexcept
	{
		m_LeftPeakDb.store(LinearToDb(lPeak), std::memory_order_relaxed);
		m_RightPeakDb.store(LinearToDb(rPeak), std::memory_order_relaxed);
		m_LeftRmsDb.store(LinearToDb(lRms), std::memory_order_relaxed);
		m_RightRmsDb.store(LinearToDb(rRms), std::memory_order_relaxed);
	}

	StereoMeter MixerBus::GetMeter() const noexcept
	{
		StereoMeter m;
		m.leftPeakDb = m_LeftPeakDb.load(std::memory_order_relaxed);
		m.rightPeakDb = m_RightPeakDb.load(std::memory_order_relaxed);
		m.leftRmsDb = m_LeftRmsDb.load(std::memory_order_relaxed);
		m.rightRmsDb = m_RightRmsDb.load(std::memory_order_relaxed);
		return m;
	}

	void MixerBus::ProcessEffects(float* left, float* right, uint32_t frameCount) noexcept
	{
		if (m_Effects.enableLowPass)
		{
			for (uint32_t i = 0; i < frameCount; ++i)
			{
				left[i] = m_Effects.lowPass.Process(left[i]);
				right[i] = m_Effects.lowPass.Process(right[i]);
			}
		}

		if (m_Effects.enableCompressor)
		{
			for (uint32_t i = 0; i < frameCount; ++i)
			{
				m_Effects.compressor.ProcessStereo(left[i], right[i]);
			}
		}
	}

	MixerGraph::MixerGraph()
	{
		ResetToDefaults();
	}

	void MixerGraph::ResetToDefaults()
	{
		m_Buses.clear();
		m_BusOrder.clear();

		// Default commercial bus hierarchy
		AddBus(kBusMaster, "Master", kInvalidBusId);
		AddBus(kBusMusic, "Music", kBusMaster);
		AddBus(kBusSfx, "SFX", kBusMaster);
		AddBus(kBusVoice, "Voice", kBusMaster);
		AddBus(kBusUi, "UI", kBusMaster);
		AddBus(kBusAmbience, "Ambience", kBusMaster);

		m_MasterLimiter.Configure(-0.1f, 50.0f, static_cast<float>(kDefaultSampleRate));

		// Built-in default snapshots
		MixerSnapshot pauseSnapshot;
		pauseSnapshot.name = "Snapshot_PauseMenu";
		pauseSnapshot.busVolumes[kBusMusic] = 0.4f;
		pauseSnapshot.busVolumes[kBusSfx] = 0.2f;
		pauseSnapshot.busVolumes[kBusAmbience] = 0.1f;
		pauseSnapshot.busVolumes[kBusVoice] = 0.0f;
		pauseSnapshot.busVolumes[kBusUi] = 1.0f; // UI bus remains unaffected
		pauseSnapshot.busLowPassCutoff[kBusMusic] = 800.0f; // low-pass muffled effect
		RegisterSnapshot(pauseSnapshot);
	}

	void MixerGraph::AddBus(BusId id, std::string name, BusId parentId)
	{
		m_Buses[id] = MixerBus(id, std::move(name), parentId);

		// Rebuild topological order: ensure child buses come before parent buses
		m_BusOrder.clear();
		for (const auto& [bid, bus] : m_Buses)
		{
			if (bus.GetParentId() != kInvalidBusId)
			{
				m_BusOrder.push_back(bid);
			}
		}
		// Master bus is at the very end
		m_BusOrder.push_back(kBusMaster);
	}

	MixerBus* MixerGraph::FindBus(BusId id)
	{
		auto it = m_Buses.find(id);
		return (it != m_Buses.end()) ? &it->second : nullptr;
	}

	const MixerBus* MixerGraph::FindBus(BusId id) const
	{
		auto it = m_Buses.find(id);
		return (it != m_Buses.end()) ? &it->second : nullptr;
	}

	void MixerGraph::SetBusVolume(BusId id, float volume)
	{
		if (auto* b = FindBus(id)) b->SetVolume(volume);
	}

	void MixerGraph::SetBusMute(BusId id, bool mute)
	{
		if (auto* b = FindBus(id)) b->SetMute(mute);
	}

	void MixerGraph::SetBusSolo(BusId id, bool solo)
	{
		if (auto* b = FindBus(id)) b->SetSolo(solo);
	}

	void MixerGraph::RegisterSnapshot(const MixerSnapshot& snapshot)
	{
		m_Snapshots[snapshot.name] = snapshot;
	}

	void MixerGraph::TriggerSnapshot(const std::string& name, float transitionSeconds)
	{
		auto it = m_Snapshots.find(name);
		if (it != m_Snapshots.end())
		{
			m_CurrentSnapshot = it->second;
			m_SnapshotActive = true;
			m_SnapshotTargetBlend = 1.0f;
			float dt = std::max(0.01f, transitionSeconds);
			m_SnapshotSpeed = 1.0f / dt;
		}
	}

	void MixerGraph::ClearSnapshot(float transitionSeconds)
	{
		m_SnapshotTargetBlend = 0.0f;
		float dt = std::max(0.01f, transitionSeconds);
		m_SnapshotSpeed = 1.0f / dt;
	}

	void MixerGraph::ProcessGraph(
		std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>>& busBuffers,
		float* finalMasterLeft,
		float* finalMasterRight,
		uint32_t frameCount,
		float sampleRate)
	{
		// Step 1: Update snapshot blend factor
		float dtBlock = static_cast<float>(frameCount) / sampleRate;
		if (m_SnapshotBlend < m_SnapshotTargetBlend)
		{
			m_SnapshotBlend = std::min(m_SnapshotTargetBlend, m_SnapshotBlend + m_SnapshotSpeed * dtBlock);
		}
		else if (m_SnapshotBlend > m_SnapshotTargetBlend)
		{
			m_SnapshotBlend = std::max(m_SnapshotTargetBlend, m_SnapshotBlend - m_SnapshotSpeed * dtBlock);
			if (m_SnapshotBlend <= 0.001f) m_SnapshotActive = false;
		}

		// Step 2: Sidechain Ducking detection from Voice bus
		float voiceRms = 0.0f;
		auto itVoice = busBuffers.find(kBusVoice);
		if (itVoice != busBuffers.end() && !itVoice->second.first.empty())
		{
			double sumSq = 0.0;
			for (uint32_t i = 0; i < frameCount; ++i)
			{
				sumSq += itVoice->second.first[i] * itVoice->second.first[i];
			}
			voiceRms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(frameCount)));
		}

		// Smooth envelope follower for ducking
		float duckCoeff = (voiceRms > m_VoiceRmsSmoothed) ? 0.8f : 0.98f;
		m_VoiceRmsSmoothed = duckCoeff * m_VoiceRmsSmoothed + (1.0f - duckCoeff) * voiceRms;

		float duckGain = 1.0f;
		if (m_VoiceRmsSmoothed > 0.015f) // -36dB threshold
		{
			// Duck Music & Ambience by up to -8dB (~0.4 linear)
			duckGain = std::clamp(1.0f - (m_VoiceRmsSmoothed - 0.015f) * 8.0f, 0.4f, 1.0f);
		}

		if (auto* bMusic = FindBus(kBusMusic)) bMusic->SetDuckingFactor(duckGain);
		if (auto* bAmb = FindBus(kBusAmbience)) bAmb->SetDuckingFactor(duckGain);

		// Step 3: Check if any bus is in Solo mode
		bool anySolo = false;
		for (const auto& [bid, bus] : m_Buses)
		{
			if (bus.IsSolo()) { anySolo = true; break; }
		}

		// Ensure Master buffer exists
		auto& masterBuf = busBuffers[kBusMaster];
		if (masterBuf.first.size() < frameCount)
		{
			masterBuf.first.assign(frameCount, 0.0f);
			masterBuf.second.assign(frameCount, 0.0f);
		}

		// Step 4: Mix buses according to topological order
		for (BusId bid : m_BusOrder)
		{
			auto* bus = FindBus(bid);
			if (!bus) continue;

			auto& buf = busBuffers[bid];
			if (buf.first.size() < frameCount)
			{
				buf.first.assign(frameCount, 0.0f);
				buf.second.assign(frameCount, 0.0f);
			}

			// Apply DSP effect rack
			bus->ProcessEffects(buf.first.data(), buf.second.data(), frameCount);

			// Calculate bus gain including mute/solo/snapshot/ducking
			float busGain = bus->GetVolume();
			if (bus->IsMute()) busGain = 0.0f;
			if (anySolo && !bus->IsSolo() && bid != kBusMaster) busGain = 0.0f;

			busGain *= bus->GetDuckingFactor();

			// Apply Snapshot override if active
			if (m_SnapshotActive && m_SnapshotBlend > 0.001f)
			{
				auto itVol = m_CurrentSnapshot.busVolumes.find(bid);
				if (itVol != m_CurrentSnapshot.busVolumes.end())
				{
					busGain = busGain * (1.0f - m_SnapshotBlend) + itVol->second * m_SnapshotBlend;
				}
			}

			// Apply gain and accumulate meters
			float lPeak = 0.0f, rPeak = 0.0f;
			double lSumSq = 0.0, rSumSq = 0.0;

			for (uint32_t i = 0; i < frameCount; ++i)
			{
				buf.first[i] *= busGain;
				buf.second[i] *= busGain;

				float al = std::abs(buf.first[i]);
				float ar = std::abs(buf.second[i]);
				lPeak = std::max(lPeak, al);
				rPeak = std::max(rPeak, ar);
				lSumSq += al * al;
				rSumSq += ar * ar;
			}

			float lRms = static_cast<float>(std::sqrt(lSumSq / static_cast<double>(frameCount)));
			float rRms = static_cast<float>(std::sqrt(rSumSq / static_cast<double>(frameCount)));
			bus->UpdateMeter(lPeak, rPeak, lRms, rRms);

			// Route output into parent bus (if not Master)
			BusId parentId = bus->GetParentId();
			if (parentId != kInvalidBusId)
			{
				auto& parentBuf = busBuffers[parentId];
				if (parentBuf.first.size() < frameCount)
				{
					parentBuf.first.assign(frameCount, 0.0f);
					parentBuf.second.assign(frameCount, 0.0f);
				}
				for (uint32_t i = 0; i < frameCount; ++i)
				{
					parentBuf.first[i] += buf.first[i];
					parentBuf.second[i] += buf.second[i];
				}
			}
		}

		// Step 5: Final output from Master bus with True-Peak Limiter
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			float l = masterBuf.first[i];
			float r = masterBuf.second[i];
			m_MasterLimiter.ProcessStereo(l, r);
			finalMasterLeft[i] = l;
			finalMasterRight[i] = r;
		}
	}
}

namespace World::Audio
{
	std::string MixerGraph::SerializeYaml() const
	{
		std::ostringstream ss;
		ss << "Version: 1\n";
		ss << "Buses:\n";
		for (BusId id : m_BusOrder)
		{
			const auto* bus = FindBus(id);
			if (!bus) continue;
			ss << "  - Id: " << static_cast<int>(id) << "\n";
			ss << "    Name: \"" << bus->GetName() << "\"\n";
			ss << "    ParentId: " << static_cast<int>(bus->GetParentId()) << "\n";
			ss << "    Volume: " << bus->GetVolume() << "\n";
		}
		ss << "Snapshots:\n";
		for (const auto& [name, snap] : m_Snapshots)
		{
			ss << "  - Name: \"" << name << "\"\n";
			ss << "    BusVolumes:\n";
			for (const auto& [bid, vol] : snap.busVolumes)
			{
				ss << "      " << static_cast<int>(bid) << ": " << vol << "\n";
			}
		}
		return ss.str();
	}

	bool MixerGraph::DeserializeYaml(const std::string& yaml)
	{
		std::istringstream stream(yaml);
		std::string line;
		BusId currentBusId = kInvalidBusId;
		std::string currentSnapName;

		while (std::getline(stream, line))
		{
			auto colonPos = line.find(':');
			if (colonPos == std::string::npos) continue;

			std::string key = line.substr(0, colonPos);
			while (!key.empty() && (key.front() == ' ' || key.front() == '\t' || key.front() == '-')) key.erase(key.begin());
			while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();

			if (key == "Id")
			{
				currentBusId = static_cast<BusId>(std::stoul(line.substr(colonPos + 1)));
			}
			else if (key == "Volume" && currentBusId != kInvalidBusId)
			{
				float vol = std::stof(line.substr(colonPos + 1));
				SetBusVolume(currentBusId, vol);
			}
			else if (key == "Name" && line.find("Snapshot") != std::string::npos)
			{
				auto q1 = line.find('"', colonPos);
				auto q2 = line.rfind('"');
				if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1)
					currentSnapName = line.substr(q1 + 1, q2 - q1 - 1);
			}
			else if (!currentSnapName.empty() && !key.empty() && std::isdigit(static_cast<unsigned char>(key.front())))
			{
				BusId bid = static_cast<BusId>(std::stoul(key));
				float vol = std::stof(line.substr(colonPos + 1));
				auto it = m_Snapshots.find(currentSnapName);
				if (it != m_Snapshots.end())
				{
					it->second.busVolumes[bid] = vol;
				}
				else
				{
					MixerSnapshot s;
					s.name = currentSnapName;
					s.busVolumes[bid] = vol;
					RegisterSnapshot(s);
				}
			}
		}
		return true;
	}
}
