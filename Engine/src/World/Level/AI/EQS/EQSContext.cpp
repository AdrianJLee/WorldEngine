#include "wldpch.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace World::EQS
{
	bool EQSContext_Querier::Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
	{
		outData.Positions.push_back(querierPos);
		if (querierRaw != 0)
			outData.EntityHandles.push_back(querierRaw);
		return true;
	}

	bool EQSContext_Target::Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
	{
		if (scene)
		{
			glm::vec3 threatPos(0.0f);
			uint32_t resolvedThreatRaw = 0;
			bool resolved = EQSCoverSystem::ResolveSceneThreatPosition(
				const_cast<Scene&>(*scene),
				querierRaw,
				m_TargetHint,
				querierPos,
				threatPos,
				&resolvedThreatRaw);

			if (resolved)
			{
				outData.Positions.push_back(threatPos);
				if (resolvedThreatRaw != 0)
					outData.EntityHandles.push_back(resolvedThreatRaw);
				return true;
			}
		}

		if (glm::length(m_FallbackPos) > 0.001f)
		{
			outData.Positions.push_back(m_FallbackPos);
			return true;
		}

		outData.Positions.push_back(querierPos + glm::vec3(-15.0f, 0.0f, 0.0f));
		return false;
	}

	bool EQSContext_AllThreats::Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
	{
		if (scene)
		{
			const auto& reg = scene->GetRegistry();
			auto tagView = reg.view<TransformComponent, TagComponent>();
			for (auto entity : tagView)
			{
				if (static_cast<uint32_t>(entity) == querierRaw)
					continue;

				const std::string& tag = StringPool::Get().NameOf(tagView.get<TagComponent>(entity).Tag);
				auto containsSub = [](const std::string& str, const char* sub) {
					auto it = std::search(str.begin(), str.end(), sub, sub + std::strlen(sub),
						[](char c1, char c2) { return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2)); });
					return it != str.end();
				};

				if (containsSub(tag, "threat") || containsSub(tag, "sniper") || containsSub(tag, "enemy") || containsSub(tag, "hostile"))
				{
					const auto& tc = tagView.get<TransformComponent>(entity);
					outData.Positions.push_back(tc.Location);
					outData.EntityHandles.push_back(static_cast<uint32_t>(entity));
				}
			}
		}

		if (outData.Positions.empty())
		{
			if (glm::length(m_FallbackPos) > 0.001f)
				outData.Positions.push_back(m_FallbackPos);
			else
				outData.Positions.push_back(querierPos + glm::vec3(-15.0f, 0.0f, 0.0f));
		}

		return !outData.Positions.empty();
	}

	bool EQSContext_AllAllies::Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
	{
		if (scene)
		{
			const auto& reg = scene->GetRegistry();
			auto tagView = reg.view<TransformComponent, TagComponent>();
			for (auto entity : tagView)
			{
				if (static_cast<uint32_t>(entity) == querierRaw)
					continue;

				const std::string& tag = StringPool::Get().NameOf(tagView.get<TagComponent>(entity).Tag);
				auto containsSub = [](const std::string& str, const char* sub) {
					auto it = std::search(str.begin(), str.end(), sub, sub + std::strlen(sub),
						[](char c1, char c2) { return std::tolower(static_cast<unsigned char>(c1)) == std::tolower(static_cast<unsigned char>(c2)); });
					return it != str.end();
				};

				if (containsSub(tag, "ally") || containsSub(tag, "friend") || containsSub(tag, "player"))
				{
					const auto& tc = tagView.get<TransformComponent>(entity);
					outData.Positions.push_back(tc.Location);
					outData.EntityHandles.push_back(static_cast<uint32_t>(entity));
				}
			}
		}
		return !outData.Positions.empty();
	}

	bool EQSContext_CustomLocation::Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
	{
		outData.Positions.push_back(m_Location);
		return true;
	}

	EQSContextRegistry::EQSContextRegistry()
	{
		Register("Querier", std::make_shared<EQSContext_Querier>());
		Register("Target", std::make_shared<EQSContext_Target>());
		Register("AllThreats", std::make_shared<EQSContext_AllThreats>());
		Register("AllAllies", std::make_shared<EQSContext_AllAllies>());
		Register("CustomLocation", std::make_shared<EQSContext_CustomLocation>());
	}

	EQSContextRegistry& EQSContextRegistry::Get()
	{
		static EQSContextRegistry s_Instance;
		return s_Instance;
	}

	void EQSContextRegistry::Register(const std::string& name, std::shared_ptr<IEQSContext> context)
	{
		m_Contexts[name] = std::move(context);
	}

	std::shared_ptr<IEQSContext> EQSContextRegistry::Find(const std::string& name) const
	{
		auto it = m_Contexts.find(name);
		if (it != m_Contexts.end())
			return it->second;
		return nullptr;
	}

	std::shared_ptr<IEQSContext> EQSContextRegistry::CreateBuiltin(BuiltinContextType type, uint32_t targetHint, const glm::vec3& customPos)
	{
		switch (type)
		{
		case BuiltinContextType::Querier:
			return std::make_shared<EQSContext_Querier>();
		case BuiltinContextType::Target:
			return std::make_shared<EQSContext_Target>(targetHint, customPos);
		case BuiltinContextType::AllThreats:
			return std::make_shared<EQSContext_AllThreats>(customPos);
		case BuiltinContextType::AllAllies:
			return std::make_shared<EQSContext_AllAllies>();
		case BuiltinContextType::CustomLocation:
			return std::make_shared<EQSContext_CustomLocation>(customPos);
		}
		return nullptr;
	}
}
