#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional>
#include <memory>
#include <mutex>

namespace World::Gameplay
{
	enum class BlackboardKeyType : uint8_t
	{
		Bool = 0,
		Int32 = 1,
		Float = 2,
		Vector3 = 3,
		Entity = 4,
		String = 5
	};

	using BlackboardValue = std::variant<bool, int32_t, float, glm::vec3, uint32_t, std::string>;

	struct BlackboardKeyDesc
	{
		std::string Name;
		BlackboardKeyType Type = BlackboardKeyType::Float;
		BlackboardValue DefaultValue;
	};

	using BlackboardObserverFn = std::function<void(uint32_t keyId, const BlackboardValue& newValue)>;

	class WLD_API Blackboard
	{
	public:
		Blackboard() = default;
		~Blackboard() = default;

		uint32_t RegisterKey(const std::string& name, BlackboardKeyType type, const BlackboardValue& defaultValue = {});
		uint32_t GetKeyId(const std::string& name) const;
		bool HasKey(uint32_t keyId) const;
		bool HasKey(const std::string& name) const;
		bool RemoveKey(const std::string& name);
		BlackboardKeyType GetKeyType(uint32_t keyId) const;
		const std::string& GetKeyName(uint32_t keyId) const;

		bool SetValue(uint32_t keyId, const BlackboardValue& value);
		bool SetValue(const std::string& name, const BlackboardValue& value);

		template<typename T>
		bool Set(const std::string& name, const T& val)
		{
			return SetValue(name, BlackboardValue(val));
		}

		template<typename T>
		bool Set(uint32_t keyId, const T& val)
		{
			return SetValue(keyId, BlackboardValue(val));
		}

		const BlackboardValue* GetValue(uint32_t keyId) const;
		const BlackboardValue* GetValue(const std::string& name) const;

		template<typename T>
		T Get(uint32_t keyId, const T& defaultVal = T{}) const
		{
			const auto* v = GetValue(keyId);
			if (v && std::holds_alternative<T>(*v))
				return std::get<T>(*v);
			return defaultVal;
		}

		template<typename T>
		T Get(const std::string& name, const T& defaultVal = T{}) const
		{
			const auto* v = GetValue(name);
			if (v && std::holds_alternative<T>(*v))
				return std::get<T>(*v);
			return defaultVal;
		}

		// 观察者机制供 Decorator Observer Aborts 即时打断
		uint32_t AddObserver(uint32_t keyId, BlackboardObserverFn callback);
		void RemoveObserver(uint32_t keyId, uint32_t observerId);
		void ClearObservers();

		uint64_t GetDirtyMask() const { return m_DirtyMask; }
		void ClearDirtyMask() { m_DirtyMask = 0; }
		bool IsKeyDirty(uint32_t keyId) const { return (keyId < 64) ? ((m_DirtyMask & (1ULL << keyId)) != 0) : false; }

		const std::unordered_map<std::string, uint32_t>& GetAllKeys() const { return m_NameToId; }
		const std::vector<BlackboardValue>& GetValues() const { return m_Values; }

	private:
		mutable std::recursive_mutex m_Mutex;
		std::unordered_map<std::string, uint32_t> m_NameToId;
		std::vector<std::string> m_IdToName;
		std::vector<BlackboardKeyType> m_Types;
		std::vector<BlackboardValue> m_Values;
		uint64_t m_DirtyMask = 0;

		struct ObserverEntry
		{
			uint32_t Id = 0;
			BlackboardObserverFn Callback;
		};
		std::unordered_map<uint32_t, std::vector<ObserverEntry>> m_Observers;
		uint32_t m_NextObserverId = 1;
	};

	class WLD_API BlackboardAsset
	{
	public:
		BlackboardAsset() = default;
		~BlackboardAsset() = default;

		void Clear() { m_KeyDescs.clear(); }
		void AddKey(const BlackboardKeyDesc& desc) { m_KeyDescs.push_back(desc); }
		const std::vector<BlackboardKeyDesc>& GetKeys() const { return m_KeyDescs; }
		std::vector<BlackboardKeyDesc>& GetKeys() { return m_KeyDescs; }

		std::shared_ptr<Blackboard> Instantiate() const;

		bool SaveToFile(const std::string& path, std::string* error = nullptr) const;
		bool LoadFromFile(const std::string& path, std::string* error = nullptr);

	private:
		std::vector<BlackboardKeyDesc> m_KeyDescs;
	};

	// 全局黑板注册表：为实体提供统一生命周期的运行时黑板总线
	class WLD_API BlackboardRegistry
	{
	public:
		static BlackboardRegistry& Get();

		std::shared_ptr<Blackboard> GetOrCreateBlackboard(uint32_t entityId, uint32_t assetId = 0);
		std::shared_ptr<Blackboard> GetBlackboard(uint32_t entityId) const;
		void RemoveBlackboard(uint32_t entityId);
		void Clear();

	private:
		BlackboardRegistry() = default;
		~BlackboardRegistry() = default;

		mutable std::recursive_mutex m_Mutex;
		std::unordered_map<uint32_t, std::shared_ptr<Blackboard>> m_Boards;
	};
}
