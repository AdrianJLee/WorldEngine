#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World
{
	class Scene;
}

namespace World::EQS
{
	enum class BuiltinContextType : uint8_t
	{
		Querier = 0,
		Target = 1,
		AllThreats = 2,
		AllAllies = 3,
		CustomLocation = 4
	};

	struct WLD_API EQSContextData
	{
		std::vector<glm::vec3> Positions;
		std::vector<uint32_t> EntityHandles;
	};

	class WLD_API IEQSContext
	{
	public:
		virtual ~IEQSContext() = default;
		virtual std::string GetName() const = 0;
		virtual bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const = 0;
		bool Resolve(const Scene& scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const
		{
			return Resolve(&scene, querierRaw, querierPos, outData);
		}
	};

	class WLD_API EQSContextRegistry
	{
	public:
		static EQSContextRegistry& Get();
		void Register(const std::string& name, std::shared_ptr<IEQSContext> context);
		std::shared_ptr<IEQSContext> Find(const std::string& name) const;
		std::shared_ptr<IEQSContext> CreateBuiltin(BuiltinContextType type, uint32_t targetHint = 0, const glm::vec3& customPos = glm::vec3(0.0f));

		template<typename T, typename... Args>
		void RegisterContext(const std::string& name, Args&&... args)
		{
			Register(name, std::make_shared<T>(std::forward<Args>(args)...));
		}

	private:
		EQSContextRegistry();
		std::unordered_map<std::string, std::shared_ptr<IEQSContext>> m_Contexts;
	};

	class WLD_API EQSContext_Querier : public IEQSContext
	{
	public:
		std::string GetName() const override { return "Context_Querier"; }
		bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const override;
	};

	class WLD_API EQSContext_Target : public IEQSContext
	{
	public:
		explicit EQSContext_Target(uint32_t targetHint = 0, const glm::vec3& fallbackPos = glm::vec3(0.0f))
			: m_TargetHint(targetHint), m_FallbackPos(fallbackPos) {}
		std::string GetName() const override { return "Context_Target"; }
		void SetTargetHint(uint32_t hint) { m_TargetHint = hint; }
		void SetFallbackPos(const glm::vec3& pos) { m_FallbackPos = pos; }
		bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const override;
	private:
		uint32_t m_TargetHint { 0 };
		glm::vec3 m_FallbackPos { 0.0f };
	};

	class WLD_API EQSContext_AllThreats : public IEQSContext
	{
	public:
		explicit EQSContext_AllThreats(const glm::vec3& fallbackPos = glm::vec3(0.0f))
			: m_FallbackPos(fallbackPos) {}
		std::string GetName() const override { return "Context_AllThreats"; }
		void SetFallbackPos(const glm::vec3& pos) { m_FallbackPos = pos; }
		bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const override;
	private:
		glm::vec3 m_FallbackPos { 0.0f };
	};

	class WLD_API EQSContext_AllAllies : public IEQSContext
	{
	public:
		std::string GetName() const override { return "Context_AllAllies"; }
		bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const override;
	};

	class WLD_API EQSContext_CustomLocation : public IEQSContext
	{
	public:
		explicit EQSContext_CustomLocation(const glm::vec3& location = glm::vec3(0.0f))
			: m_Location(location) {}
		std::string GetName() const override { return "Context_CustomLocation"; }
		void SetLocation(const glm::vec3& loc) { m_Location = loc; }
		const glm::vec3& GetLocation() const { return m_Location; }
		bool Resolve(const Scene* scene, uint32_t querierRaw, const glm::vec3& querierPos, EQSContextData& outData) const override;
	private:
		glm::vec3 m_Location { 0.0f };
	};
}
