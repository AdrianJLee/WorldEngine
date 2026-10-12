#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World
{
	enum class AnimationLoopMode : uint8_t
	{
		Loop = 0,
		Once = 1,
		PingPong = 2
	};

	struct WLD_API SpriteAnimationFrame
	{
		NameId SubTextureName;
		std::string NameString;
		float Duration = 0.0833f; // Default 1/12s
		std::string EventTag;

		SpriteAnimationFrame() = default;
		SpriteAnimationFrame(std::string nameString, float duration = 0.0833f, std::string eventTag = "");
	};

	class WLD_API SpriteAnimation
	{
	public:
		SpriteAnimation() = default;
		explicit SpriteAnimation(std::string name, float fps = 12.0f, AnimationLoopMode loop = AnimationLoopMode::Loop);

		const std::string& GetName() const noexcept { return m_Name; }
		void SetName(std::string name) { m_Name = std::move(name); }

		const std::string& GetAtlasPath() const noexcept { return m_AtlasPath; }
		void SetAtlasPath(std::string path) { m_AtlasPath = std::move(path); }

		float GetFrameRate() const noexcept { return m_FrameRate; }
		void SetFrameRate(float fps);

		AnimationLoopMode GetLoopMode() const noexcept { return m_LoopMode; }
		void SetLoopMode(AnimationLoopMode mode) noexcept { m_LoopMode = mode; }

		void AddFrame(const SpriteAnimationFrame& frame);
		void ClearFrames();
		size_t GetFrameCount() const noexcept { return m_Frames.size(); }
		const SpriteAnimationFrame* GetFrame(size_t index) const;
		const std::vector<SpriteAnimationFrame>& GetAllFrames() const noexcept { return m_Frames; }

		float GetTotalDuration() const noexcept;

		uint32_t SampleFrame(float time, bool* outFinished = nullptr) const;

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);

		static std::shared_ptr<SpriteAnimation> LoadFromFile(const std::string& filepath);
		static std::shared_ptr<SpriteAnimation> GetOrLoad(const std::string& filepath);
		static void ClearCache();
		bool SaveToFile(const std::string& filepath) const;

	private:
		std::string m_Name;
		std::string m_AtlasPath;
		float m_FrameRate { 12.0f };
		AnimationLoopMode m_LoopMode { AnimationLoopMode::Loop };
		std::vector<SpriteAnimationFrame> m_Frames;
	};
}
