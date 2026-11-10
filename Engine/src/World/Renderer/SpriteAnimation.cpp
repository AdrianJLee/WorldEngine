#include "World/Utils/Paths.h"
#include "SpriteAnimation.h"
#include "World/Core/Log.h"

#include <filesystem>



#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <mutex>
#include <cmath>

namespace World
{
	namespace
	{
		std::filesystem::path ResolvePhysicalPath(const std::string& filepath)
		{
			if (filepath.empty())
				return {};
			std::error_code ec;
			std::filesystem::path p(filepath);
			if (std::filesystem::exists(p, ec))
				return p;
			std::filesystem::path fromProj = Paths::ProjectDir() / p;
			if (std::filesystem::exists(fromProj, ec))
				return fromProj;
			std::filesystem::path fromAsset = Paths::AssetRoot() / p;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;
			if (filepath.rfind("assets/", 0) == 0 || filepath.rfind("assets\\", 0) == 0)
			{
				std::filesystem::path stripped = Paths::AssetRoot() / filepath.substr(7);
				if (std::filesystem::exists(stripped, ec))
					return stripped;
			}
			return p;
		}
	}
	static std::unordered_map<std::string, std::shared_ptr<SpriteAnimation>> s_AnimCache;
	static std::mutex s_AnimCacheMutex;

	SpriteAnimationFrame::SpriteAnimationFrame(std::string nameString, float duration, std::string eventTag)
		: NameString(std::move(nameString)), Duration(duration > 0.0f ? duration : 0.0833f), EventTag(std::move(eventTag))
	{
		SubTextureName = StringPool::Get().InternName(NameString);
	}

	SpriteAnimation::SpriteAnimation(std::string name, float fps, AnimationLoopMode loop)
		: m_Name(std::move(name)), m_FrameRate(fps > 0.0f ? fps : 12.0f), m_LoopMode(loop)
	{
	}

	void SpriteAnimation::SetFrameRate(float fps)
	{
		m_FrameRate = fps > 0.0f ? fps : 12.0f;
		const float defaultDur = 1.0f / m_FrameRate;
		for (auto& frame : m_Frames)
		{
			frame.Duration = defaultDur;
		}
	}

	void SpriteAnimation::AddFrame(const SpriteAnimationFrame& frame)
	{
		m_Frames.push_back(frame);
	}

	void SpriteAnimation::ClearFrames()
	{
		m_Frames.clear();
	}

	const SpriteAnimationFrame* SpriteAnimation::GetFrame(size_t index) const
	{
		if (index < m_Frames.size())
			return &m_Frames[index];
		return nullptr;
	}

	float SpriteAnimation::GetTotalDuration() const noexcept
	{
		float total = 0.0f;
		for (const auto& frame : m_Frames)
			total += frame.Duration;
		return total;
	}

	uint32_t SpriteAnimation::SampleFrame(float time, bool* outFinished) const
	{
		if (outFinished) *outFinished = false;
		if (m_Frames.empty()) return 0;
		if (m_Frames.size() == 1) return 0;

		const float total = GetTotalDuration();
		if (total <= 0.0f) return 0;

		float evalTime = time;

		switch (m_LoopMode)
		{
		case AnimationLoopMode::Once:
			if (evalTime >= total)
			{
				if (outFinished) *outFinished = true;
				return static_cast<uint32_t>(m_Frames.size() - 1);
			}
			break;
		case AnimationLoopMode::Loop:
			evalTime = std::fmod(evalTime, total);
			if (evalTime < 0.0f) evalTime += total;
			break;
		case AnimationLoopMode::PingPong:
		{
			const float cycle = 2.0f * total;
			evalTime = std::fmod(evalTime, cycle);
			if (evalTime < 0.0f) evalTime += cycle;
			if (evalTime > total)
				evalTime = cycle - evalTime;
			break;
		}
		}

		float accumulated = 0.0f;
		for (size_t i = 0; i < m_Frames.size(); ++i)
		{
			accumulated += m_Frames[i].Duration;
			if (evalTime < accumulated)
				return static_cast<uint32_t>(i);
		}

		return static_cast<uint32_t>(m_Frames.size() - 1);
	}

	std::string SpriteAnimation::SerializeYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "Name" << YAML::Value << m_Name;
		out << YAML::Key << "AtlasPath" << YAML::Value << m_AtlasPath;
		out << YAML::Key << "FrameRate" << YAML::Value << m_FrameRate;
		out << YAML::Key << "LoopMode" << YAML::Value << static_cast<int>(m_LoopMode);

		out << YAML::Key << "Frames" << YAML::Value << YAML::BeginSeq;
		for (const auto& frame : m_Frames)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "SubTexture" << YAML::Value << frame.NameString;
			out << YAML::Key << "Duration" << YAML::Value << frame.Duration;
			if (!frame.EventTag.empty())
				out << YAML::Key << "EventTag" << YAML::Value << frame.EventTag;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	bool SpriteAnimation::DeserializeYaml(const std::string& yamlStr)
	{
		try
		{
			YAML::Node data = YAML::Load(yamlStr);
			if (!data || !data.IsMap()) return false;

			if (data["Name"]) m_Name = data["Name"].as<std::string>();
			if (data["AtlasPath"]) m_AtlasPath = data["AtlasPath"].as<std::string>();
			if (data["FrameRate"]) m_FrameRate = data["FrameRate"].as<float>();
			if (data["LoopMode"]) m_LoopMode = static_cast<AnimationLoopMode>(data["LoopMode"].as<int>());

			m_Frames.clear();
			auto frames = data["Frames"];
			if (frames && frames.IsSequence())
			{
				for (const auto& f : frames)
				{
					std::string name = f["SubTexture"] ? f["SubTexture"].as<std::string>() : "";
					float dur = f["Duration"] ? f["Duration"].as<float>() : (1.0f / m_FrameRate);
					std::string eventTag = f["EventTag"] ? f["EventTag"].as<std::string>() : "";
					m_Frames.emplace_back(name, dur, eventTag);
				}
			}
			return true;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_WARN("[SpriteAnimation] Failed to deserialize YAML: {}", e.what());
			return false;
		}
	}

	std::shared_ptr<SpriteAnimation> SpriteAnimation::LoadFromFile(const std::string& filepath)
	{
		const auto resolved = ResolvePhysicalPath(filepath);
		std::ifstream file(resolved.string());
		if (!file.is_open())
		{
			WLD_CORE_WARN("[SpriteAnimation] Failed to open file: {}", filepath);
			return nullptr;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		auto anim = std::make_shared<SpriteAnimation>();
		if (!anim->DeserializeYaml(ss.str()))
		{
			return nullptr;
		}
		return anim;
	}

	std::shared_ptr<SpriteAnimation> SpriteAnimation::GetOrLoad(const std::string& filepath)
	{
		std::lock_guard<std::mutex> lock(s_AnimCacheMutex);
		auto it = s_AnimCache.find(filepath);
		if (it != s_AnimCache.end())
			return it->second;

		auto anim = LoadFromFile(filepath);
		if (anim)
		{
			s_AnimCache[filepath] = anim;
		}
		return anim;
	}

	void SpriteAnimation::ClearCache()
	{
		std::lock_guard<std::mutex> lock(s_AnimCacheMutex);
		s_AnimCache.clear();
	}

	bool SpriteAnimation::SaveToFile(const std::string& filepath) const
	{
		std::ofstream file(filepath);
		if (!file.is_open())
		{
			WLD_CORE_WARN("[SpriteAnimation] Failed to write file: {}", filepath);
			return false;
		}
		file << SerializeYaml();
		return true;
	}
}
