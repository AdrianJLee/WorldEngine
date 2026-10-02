#include "wldpch.h"
#include "World/Script/BehaviorRegistry.h"

#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/ScriptProperties.h"

#include <algorithm>
#include <utility>

namespace World
{
	namespace
	{
		// Lua 行为的模块/字段 id 命名空间。字段 id 与 schema-compiler 的
		// Fnv1a64("<Module>::<Type>.<FieldName>") 是同一条派生规则,只是把 Lua 行为
		// 视作 World::Script 上的字段;不引入第二套哈希。
		constexpr const char* kLuaModulePrefix = "Lua:";
		constexpr const char* kLuaFieldScope = "World::Script.";

		void SetError(std::string* error, std::string message)
		{
			if (error) *error = std::move(message);
		}

		void SortFields(std::vector<BehaviorFieldDesc>& fields)
		{
			std::sort(fields.begin(), fields.end(), [](const BehaviorFieldDesc& left, const BehaviorFieldDesc& right)
			{
				if (left.FieldId != right.FieldId) return left.FieldId < right.FieldId;
				return left.Name < right.Name;
			});
		}

		std::vector<BehaviorFieldDesc> CanonicalFields(const BehaviorDesc& desc)
		{
			std::vector<BehaviorFieldDesc> fields = desc.Fields;
			SortFields(fields);
			return fields;
		}

		bool SameLifecycle(const BehaviorLifecycleSlots& left, const BehaviorLifecycleSlots& right)
		{
			return left.OnCreate == right.OnCreate && left.OnUpdate == right.OnUpdate &&
				left.OnDestroy == right.OnDestroy && left.OnEvent == right.OnEvent;
		}

		bool SameFields(const std::vector<BehaviorFieldDesc>& left, const std::vector<BehaviorFieldDesc>& right)
		{
			if (left.size() != right.size()) return false;
			for (std::size_t index = 0; index < left.size(); ++index)
			{
				if (left[index].FieldId != right[index].FieldId) return false;
				if (left[index].Name != right[index].Name) return false;
				if (left[index].Type != right[index].Type) return false;
			}
			return true;
		}

		// 现有两类行为在前端上都能被三个生命周期入口调度;OnEvent 留到 W4。
		constexpr BehaviorLifecycleSlots kExistingFrontendSlots { true, true, true, false };

	}

	const char* BehaviorLanguageName(BehaviorLanguage language)
	{
		switch (language)
		{
			case BehaviorLanguage::Cpp: return "Cpp";
			case BehaviorLanguage::Luau: return "Luau";
			default: return "Unknown";
		}
	}

	bool BehaviorDescEquals(const BehaviorDesc& left, const BehaviorDesc& right)
	{
		if (left.ModuleId != right.ModuleId) return false;
		if (left.DisplayName != right.DisplayName) return false;
		if (left.Language != right.Language) return false;
		if (!SameLifecycle(left.Lifecycle, right.Lifecycle)) return false;
		return SameFields(CanonicalFields(left), CanonicalFields(right));
	}

	BehaviorRegistry& BehaviorRegistry::Instance()
	{
		static BehaviorRegistry s_Instance;
		return s_Instance;
	}

	bool BehaviorRegistry::Register(BehaviorDesc desc, std::string* error)
	{
		if (desc.ModuleId.empty())
		{
			SetError(error, "behavior module id must not be empty");
			return false;
		}
		if (m_Behaviors.find(desc.ModuleId) != m_Behaviors.end())
		{
			SetError(error, "behavior module '" + desc.ModuleId +
				"' is already registered; duplicate registration rejected (use Replace to refresh)");
			return false;
		}
		const std::string moduleId = desc.ModuleId; // 先留 key 再移动 desc,避免参数求值顺序问题
		SortFields(desc.Fields);
		m_Behaviors.emplace(moduleId, std::move(desc));
		return true;
	}

	bool BehaviorRegistry::Replace(BehaviorDesc desc, std::string* error)
	{
		if (desc.ModuleId.empty())
		{
			SetError(error, "behavior module id must not be empty");
			return false;
		}
		SortFields(desc.Fields);
		m_Behaviors[desc.ModuleId] = std::move(desc);
		return true;
	}

	bool BehaviorRegistry::Unregister(const std::string& moduleId)
	{
		return m_Behaviors.erase(moduleId) != 0;
	}

	const BehaviorDesc* BehaviorRegistry::Find(const std::string& moduleId) const
	{
		const auto it = m_Behaviors.find(moduleId);
		return it == m_Behaviors.end() ? nullptr : &it->second;
	}

	std::vector<const BehaviorDesc*> BehaviorRegistry::List() const
	{
		std::vector<const BehaviorDesc*> result;
		result.reserve(m_Behaviors.size());
		for (const auto& entry : m_Behaviors) // std::map 已按 ModuleId 升序
			result.push_back(&entry.second);
		return result;
	}

	std::size_t BehaviorRegistry::Size() const
	{
		return m_Behaviors.size();
	}

	void BehaviorRegistry::Clear()
	{
		m_Behaviors.clear();
	}

	std::string BehaviorRegistry::NativeModuleId(const Schema::TypeSchema& type)
	{
		return type.Id.Name;
	}

	std::string BehaviorRegistry::LuaModuleId(const std::string& scriptFilePath)
	{
		return std::string(kLuaModulePrefix) + scriptFilePath;
	}

	uint64_t BehaviorRegistry::LuaFieldId(const std::string& fieldName)
	{
		return Schema::Fnv1a64(std::string(kLuaFieldScope) + fieldName);
	}

	BehaviorDesc BehaviorRegistry::MakeNativeDesc(const Schema::TypeSchema& type)
	{
		BehaviorDesc desc;
		desc.ModuleId = NativeModuleId(type);
		desc.DisplayName = type.DisplayName.empty() ? type.Id.Name : type.DisplayName;
		desc.Language = BehaviorLanguage::Cpp;
		desc.Fields.reserve(type.Fields.size());
		for (const Schema::FieldSchema& field : type.Fields)
			desc.Fields.push_back(BehaviorFieldDesc{ field.Id.Value, field.Name, field.K });
		SortFields(desc.Fields);
		desc.Lifecycle = kExistingFrontendSlots;
		return desc;
	}



	std::vector<const BehaviorDesc*> BehaviorRegistry::DescribeEntity(const Scene&, entt::entity) const
	{
		return {};
	}

	std::vector<const BehaviorDesc*> BehaviorRegistry::DescribeEntity(const Entity& entity) const
	{
		if (!entity.IsValid()) return {};
		const Scene* scene = entity.GetScene();
		if (!scene) return {};
		return DescribeEntity(*scene, static_cast<entt::entity>(entity));
	}
}
