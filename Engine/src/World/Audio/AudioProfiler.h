#pragma once

#include "AudioTypes.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <atomic>

namespace World::Audio
{
	struct BusProfileMetric
	{
		BusId busId { kInvalidBusId };
		std::string busName;
		float peakDb { -96.0f };
		float rmsDb { -96.0f };
		float lufs { -96.0f };
		bool clipping { false };
	};

	struct AudioProfilerStats
	{
		uint32_t activePhysicalVoices { 0 };
		uint32_t totalPhysicalVoices { kMaxPhysicalVoices };
		uint32_t activeVirtualVoices { 0 };
		uint32_t totalVirtualVoices { kMaxVirtualVoices };
		uint64_t mixCpuTimeUs { 0 };
		uint64_t maxMixCpuTimeUs { 0 };
		uint64_t totalRenderedFrames { 0 };
		double dspTimeSeconds { 0.0 };
		bool masterClipping { false };
		std::vector<BusProfileMetric> busMetrics;
	};

	class WLD_API AudioProfiler
	{
	public:
		AudioProfiler();
		~AudioProfiler() = default;

		// Called at end of mix block in AudioRenderCallback
		void RecordMixBlock(
			uint64_t cpuTimeUs,
			uint32_t activePhysical,
			uint32_t activeVirtual,
			uint64_t totalFrames,
			double dspTime,
			bool masterClipping);

		void RecordBusMetric(
			BusId busId,
			const std::string& name,
			float peakDb,
			float rmsDb,
			float lufs,
			bool clipping);

		AudioProfilerStats GetSnapshot() const;

		uint64_t GetLastMixCpuTimeUs() const noexcept { return m_LastMixCpuTimeUs.load(std::memory_order_relaxed); }
		uint64_t GetMaxMixCpuTimeUs() const noexcept { return m_MaxMixCpuTimeUs.load(std::memory_order_relaxed); }
		bool IsMasterClipping() const noexcept { return m_MasterClipping.load(std::memory_order_relaxed); }

		void ResetClippingFlags() noexcept;
		void Reset();

		// Export JSON formatted summary for telemetry and AI control channels
		std::string ToJson() const;

	private:
		std::atomic<uint64_t> m_LastMixCpuTimeUs { 0 };
		std::atomic<uint64_t> m_MaxMixCpuTimeUs { 0 };
		std::atomic<uint32_t> m_ActivePhysicalVoices { 0 };
		std::atomic<uint32_t> m_ActiveVirtualVoices { 0 };
		std::atomic<uint64_t> m_TotalRenderedFrames { 0 };
		std::atomic<bool> m_MasterClipping { false };
		double m_DspTimeSeconds { 0.0 };

		mutable std::mutex m_BusMetricsMutex;
		std::vector<BusProfileMetric> m_BusMetrics;
	};
}
