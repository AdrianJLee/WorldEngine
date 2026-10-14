#include "AudioProfiler.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace World::Audio
{
	AudioProfiler::AudioProfiler() = default;

	void AudioProfiler::RecordMixBlock(
		uint64_t cpuTimeUs,
		uint32_t activePhysical,
		uint32_t activeVirtual,
		uint64_t totalFrames,
		double dspTime,
		bool masterClipping)
	{
		m_LastMixCpuTimeUs.store(cpuTimeUs, std::memory_order_relaxed);

		uint64_t curMax = m_MaxMixCpuTimeUs.load(std::memory_order_relaxed);
		if (cpuTimeUs > curMax)
		{
			m_MaxMixCpuTimeUs.store(cpuTimeUs, std::memory_order_relaxed);
		}

		m_ActivePhysicalVoices.store(activePhysical, std::memory_order_relaxed);
		m_ActiveVirtualVoices.store(activeVirtual, std::memory_order_relaxed);
		m_TotalRenderedFrames.store(totalFrames, std::memory_order_relaxed);
		m_DspTimeSeconds = dspTime;

		if (masterClipping)
		{
			m_MasterClipping.store(true, std::memory_order_relaxed);
		}
	}

	void AudioProfiler::RecordBusMetric(
		BusId busId,
		const std::string& name,
		float peakDb,
		float rmsDb,
		float lufs,
		bool clipping)
	{
		std::lock_guard<std::mutex> lock(m_BusMetricsMutex);
		for (auto& bm : m_BusMetrics)
		{
			if (bm.busId == busId)
			{
				bm.busName = name;
				bm.peakDb = peakDb;
				bm.rmsDb = rmsDb;
				bm.lufs = lufs;
				if (clipping) bm.clipping = true;
				return;
			}
		}

		m_BusMetrics.push_back({ busId, name, peakDb, rmsDb, lufs, clipping });
	}

	AudioProfilerStats AudioProfiler::GetSnapshot() const
	{
		AudioProfilerStats stats;
		stats.activePhysicalVoices = m_ActivePhysicalVoices.load(std::memory_order_relaxed);
		stats.totalPhysicalVoices = kMaxPhysicalVoices;
		stats.activeVirtualVoices = m_ActiveVirtualVoices.load(std::memory_order_relaxed);
		stats.totalVirtualVoices = kMaxVirtualVoices;
		stats.mixCpuTimeUs = m_LastMixCpuTimeUs.load(std::memory_order_relaxed);
		stats.maxMixCpuTimeUs = m_MaxMixCpuTimeUs.load(std::memory_order_relaxed);
		stats.totalRenderedFrames = m_TotalRenderedFrames.load(std::memory_order_relaxed);
		stats.dspTimeSeconds = m_DspTimeSeconds;
		stats.masterClipping = m_MasterClipping.load(std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> lock(m_BusMetricsMutex);
			stats.busMetrics = m_BusMetrics;
		}

		return stats;
	}

	void AudioProfiler::ResetClippingFlags() noexcept
	{
		m_MasterClipping.store(false, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(m_BusMetricsMutex);
		for (auto& bm : m_BusMetrics)
		{
			bm.clipping = false;
		}
	}

	void AudioProfiler::Reset()
	{
		m_LastMixCpuTimeUs.store(0, std::memory_order_relaxed);
		m_MaxMixCpuTimeUs.store(0, std::memory_order_relaxed);
		m_ActivePhysicalVoices.store(0, std::memory_order_relaxed);
		m_ActiveVirtualVoices.store(0, std::memory_order_relaxed);
		m_TotalRenderedFrames.store(0, std::memory_order_relaxed);
		m_MasterClipping.store(false, std::memory_order_relaxed);
		m_DspTimeSeconds = 0.0;

		std::lock_guard<std::mutex> lock(m_BusMetricsMutex);
		m_BusMetrics.clear();
	}

	std::string AudioProfiler::ToJson() const
	{
		AudioProfilerStats snap = GetSnapshot();

		std::ostringstream ss;
		ss << std::fixed << std::setprecision(2);
		ss << "{\n";
		ss << "  \"active_physical_voices\": " << snap.activePhysicalVoices << ",\n";
		ss << "  \"total_physical_voices\": " << snap.totalPhysicalVoices << ",\n";
		ss << "  \"active_virtual_voices\": " << snap.activeVirtualVoices << ",\n";
		ss << "  \"total_virtual_voices\": " << snap.totalVirtualVoices << ",\n";
		ss << "  \"mix_cpu_time_us\": " << snap.mixCpuTimeUs << ",\n";
		ss << "  \"max_mix_cpu_time_us\": " << snap.maxMixCpuTimeUs << ",\n";
		ss << "  \"total_rendered_frames\": " << snap.totalRenderedFrames << ",\n";
		ss << "  \"dsp_time_seconds\": " << snap.dspTimeSeconds << ",\n";
		ss << "  \"master_clipping\": " << (snap.masterClipping ? "true" : "false") << ",\n";
		ss << "  \"buses\": [\n";

		for (size_t i = 0; i < snap.busMetrics.size(); ++i)
		{
			const auto& bm = snap.busMetrics[i];
			ss << "    {\n";
			ss << "      \"id\": " << bm.busId << ",\n";
			ss << "      \"name\": \"" << bm.busName << "\",\n";
			ss << "      \"peak_db\": " << bm.peakDb << ",\n";
			ss << "      \"rms_db\": " << bm.rmsDb << ",\n";
			ss << "      \"lufs\": " << bm.lufs << ",\n";
			ss << "      \"clipping\": " << (bm.clipping ? "true" : "false") << "\n";
			ss << "    }" << (i + 1 < snap.busMetrics.size() ? "," : "") << "\n";
		}

		ss << "  ]\n";
		ss << "}";
		return ss.str();
	}
}
