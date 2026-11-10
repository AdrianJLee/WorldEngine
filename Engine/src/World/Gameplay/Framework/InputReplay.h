#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputFrame.h"
#include "World/Gameplay/Framework/InputSource.h"
#include <filesystem>
#include <string>
#include <vector>

namespace World::Gameplay
{
#pragma pack(push, 1)
	struct WLD_API ReplayHeader
	{
		uint32_t Magic = 0x50455257; // 'WREP'
		uint32_t Version = 1;
		uint64_t RandomSeed = 0;
		float FixedDelta = 1.0f / 60.0f;
		uint32_t FrameCount = 0;
		uint32_t PlayerCount = 1;
		uint32_t InitialStateHash = 0;
		uint32_t FinalStateHash = 0;
	};
#pragma pack(pop)

	class WLD_API InputReplay
	{
	public:
		static bool Save(const std::filesystem::path& path, const ReplayHeader& header,
			const std::vector<InputFrame>& frames, std::string* error = nullptr);

		static bool Load(const std::filesystem::path& path, ReplayHeader* outHeader,
			std::vector<InputFrame>* outFrames, std::string* error = nullptr);

		static uint32_t ComputeStateHash(const std::vector<InputFrame>& frames);
	};

	// 回放输入源
	class WLD_API ReplayInputSource : public IInputSource
	{
	public:
		explicit ReplayInputSource(std::vector<InputFrame> frames);
		void Poll(uint32_t player, RawInputState& outState, float dt) override;
		bool IsFinished() const;
		uint32_t GetCurrentFrame() const { return m_Cursor; }

	private:
		std::vector<InputFrame> m_Frames;
		uint32_t m_Cursor = 0;
	};
}
