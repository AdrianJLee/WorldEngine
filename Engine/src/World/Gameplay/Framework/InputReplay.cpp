#include "wldpch.h"
#include "World/Gameplay/Framework/InputReplay.h"
#include <fstream>

namespace World::Gameplay
{
	void DeviceInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)player;
		(void)outState;
		(void)dt;
	}

	void AiInjectionInputSource::InjectAction(uint32_t player, NameId action, float value, uint32_t frames)
	{
		m_Injections[player].push_back({ action, value, frames > 0 ? frames : 1 });
	}

	void AiInjectionInputSource::Clear(uint32_t player)
	{
		m_Injections.erase(player);
	}

	bool AiInjectionInputSource::HasPendingInjections(uint32_t player) const
	{
		auto it = m_Injections.find(player);
		return it != m_Injections.end() && !it->second.empty();
	}

	void AiInjectionInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)dt;
		auto it = m_Injections.find(player);
		if (it == m_Injections.end())
			return;

		for (auto actionIt = it->second.begin(); actionIt != it->second.end(); )
		{
			if (actionIt->RemainingFrames > 0)
			{
				--actionIt->RemainingFrames;
				++actionIt;
			}
			else
			{
				actionIt = it->second.erase(actionIt);
			}
		}
	}

	bool InputReplay::Save(const std::filesystem::path& path, const ReplayHeader& header,
		const std::vector<InputFrame>& frames, std::string* error)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		if (!out)
		{
			if (error) *error = "Failed to open replay file for write: " + path.string();
			return false;
		}

		ReplayHeader h = header;
		h.FrameCount = static_cast<uint32_t>(frames.size());
		out.write(reinterpret_cast<const char*>(&h), sizeof(ReplayHeader));
		if (!frames.empty())
		{
			out.write(reinterpret_cast<const char*>(frames.data()), frames.size() * sizeof(InputFrame));
		}
		if (error) error->clear();
		return true;
	}

	bool InputReplay::Load(const std::filesystem::path& path, ReplayHeader* outHeader,
		std::vector<InputFrame>* outFrames, std::string* error)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			if (error) *error = "Failed to open replay file for read: " + path.string();
			return false;
		}

		ReplayHeader h;
		in.read(reinterpret_cast<char*>(&h), sizeof(ReplayHeader));
		if (h.Magic != 0x50455257)
		{
			if (error) *error = "Invalid replay magic header";
			return false;
		}

		if (outHeader)
			*outHeader = h;

		if (outFrames)
		{
			outFrames->resize(h.FrameCount);
			if (h.FrameCount > 0)
			{
				in.read(reinterpret_cast<char*>(outFrames->data()), h.FrameCount * sizeof(InputFrame));
			}
		}
		if (error) error->clear();
		return true;
	}

	uint32_t InputReplay::ComputeStateHash(const std::vector<InputFrame>& frames)
	{
		// FNV-1a 32位哈希
		uint32_t hash = 2166136261u;
		const uint8_t* data = reinterpret_cast<const uint8_t*>(frames.data());
		const std::size_t size = frames.size() * sizeof(InputFrame);
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= data[i];
			hash *= 16777619u;
		}
		return hash;
	}

	ReplayInputSource::ReplayInputSource(std::vector<InputFrame> frames)
		: m_Frames(std::move(frames)) {}

	void ReplayInputSource::Poll(uint32_t player, RawInputState& outState, float dt)
	{
		(void)dt;
		if (m_Cursor < m_Frames.size())
		{
			const InputFrame& f = m_Frames[m_Cursor];
			if (f.PlayerIndex == player)
			{
				outState.MousePosition.x = static_cast<float>(f.MouseX);
				outState.MousePosition.y = static_cast<float>(f.MouseY);
				outState.MouseDelta.x = static_cast<float>(f.MouseDeltaX);
				outState.MouseDelta.y = static_cast<float>(f.MouseDeltaY);
				outState.ScrollDelta.x = static_cast<float>(f.ScrollX);
				outState.ScrollDelta.y = static_cast<float>(f.ScrollY);
				++m_Cursor;
			}
		}
	}

	bool ReplayInputSource::IsFinished() const
	{
		return m_Cursor >= m_Frames.size();
	}
}
