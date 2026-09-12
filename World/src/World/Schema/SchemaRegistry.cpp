#include "wldpch.h"
#include "World/Schema/SchemaRegistry.h"

namespace World::Schema
{
	namespace
	{
		// 注册表可能在任何宿主服务之前工作:日志未初始化时静默,不崩溃。
		template <typename... Args>
		void RegistryWarn(const char* format, Args&&... args)
		{
			if (auto logger = Log::GetCoreLogger())
				logger->warn(format, std::forward<Args>(args)...);
		}

		template <typename... Args>
		void RegistryError(const char* format, Args&&... args)
		{
			if (auto logger = Log::GetCoreLogger())
				logger->error(format, std::forward<Args>(args)...);
		}
	}

	const char* SchemaRegistry::StatusName(Status status)
	{
		switch (status)
		{
			case Status::Ok: return "ok";
			case Status::DuplicateType: return "duplicate type in module";
			case Status::DuplicateComponentId: return "duplicate component storage id";
			case Status::Conflict: return "conflicting registration";
			case Status::AbiMismatch: return "schema abi version not supported";
			default: return "unknown";
		}
	}

	bool SchemaRegistry::ValidateNew(const ModuleId& module, const TypeSchema& schema, Status* outStatus) const
	{
		if (schema.AbiVersion > WE_SCHEMA_ABI_VERSION)
		{
			*outStatus = Status::AbiMismatch;
			return false;
		}

		auto byName = m_ByName.find(schema.Id.Name);
		if (byName != m_ByName.end())
		{
			for (size_t index : byName->second)
			{
				const TypeEntry& existing = m_Entries[index];
				if (existing.Module.Name == module.Name)
				{
					*outStatus = Status::DuplicateType;
					return false;
				}
				// 组件类型跨模块同名视为冲突;Struct/Script 允许共存。
				if (schema.Category == TypeCategory::Component && existing.Schema.Category == TypeCategory::Component)
				{
					*outStatus = Status::Conflict;
					return false;
				}
			}
		}

		if (schema.Storage && schema.Storage->ComponentId != 0)
		{
			auto byComponent = m_ByComponentId.find(schema.Storage->ComponentId);
			if (byComponent != m_ByComponentId.end())
			{
				*outStatus = Status::DuplicateComponentId;
				return false;
			}
		}

		*outStatus = Status::Ok;
		return true;
	}

	SchemaRegistry::Status SchemaRegistry::Register(const ModuleId& module, const TypeSchema& schema)
	{
		Status status = Status::Ok;
		if (!ValidateNew(module, schema, &status))
		{
			RegistryError("SchemaRegistry: reject module '{}' type '{}': {}",
				module.Name, schema.Id.Name, StatusName(status));
			return status;
		}

		// schema 可能引用本注册表内已存条目(调用方传 *Find(...));
		// 先拷贝再变更容器,避免 push_back 重分配期间源对象失效。
		TypeSchema copy = schema;
		const std::string typeName = copy.Id.Name;
		const uint32_t componentId = copy.Storage ? copy.Storage->ComponentId : 0;
		const size_t index = m_Entries.size();
		m_Entries.push_back({ module, std::move(copy) });
		m_ByName[typeName].push_back(index);
		if (componentId != 0)
			m_ByComponentId[componentId] = index;
		return Status::Ok;
	}

	SchemaRegistry::Status SchemaRegistry::RegisterEnum(const ModuleId& module, const EnumSchema& schema)
	{
		for (const auto& entry : m_EnumEntries)
		{
			if (entry.Schema.Name == schema.Name)
			{
				if (entry.Module.Name == module.Name)
				{
					RegistryError("SchemaRegistry: reject duplicate enum '{}' in module '{}'", schema.Name, module.Name);
					return Status::DuplicateType;
				}
				RegistryError("SchemaRegistry: reject enum '{}' conflicting between modules '{}' and '{}'",
					schema.Name, entry.Module.Name, module.Name);
				return Status::Conflict;
			}
		}
		EnumSchema copy = schema;
		const std::string enumName = copy.Name;
		const size_t index = m_EnumEntries.size();
		m_EnumEntries.push_back({ module, std::move(copy) });
		m_EnumByName[enumName].push_back(index);
		return Status::Ok;
	}

	SchemaRegistry::Status SchemaRegistry::RegisterModule(const ModuleId& module, const std::vector<TypeSchema>& schemas)
	{
		for (const TypeSchema& schema : schemas)
		{
			Status status = Status::Ok;
			if (!ValidateNew(module, schema, &status))
				return status; // 未提交任何条目
		}
		for (const TypeSchema& schema : schemas)
			Register(module, schema); // 已校验,必成功
		return Status::Ok;
	}

	void SchemaRegistry::UnregisterModule(const ModuleId& module)
	{
		// 线性规模足够(模块级操作,非热路径)。
		std::vector<TypeEntry> entries;
		entries.reserve(m_Entries.size());
		for (TypeEntry& entry : m_Entries)
			if (entry.Module.Name != module.Name)
				entries.push_back(std::move(entry));
		m_Entries.swap(entries);

		m_ByName.clear();
		m_ByComponentId.clear();
		for (size_t i = 0; i < m_Entries.size(); ++i)
		{
			m_ByName[m_Entries[i].Schema.Id.Name].push_back(i);
			if (m_Entries[i].Schema.Storage && m_Entries[i].Schema.Storage->ComponentId != 0)
				m_ByComponentId[m_Entries[i].Schema.Storage->ComponentId] = i;
		}

		std::vector<EnumEntry> enumEntries;
		enumEntries.reserve(m_EnumEntries.size());
		for (EnumEntry& entry : m_EnumEntries)
			if (entry.Module.Name != module.Name)
				enumEntries.push_back(std::move(entry));
		m_EnumEntries.swap(enumEntries);

		m_EnumByName.clear();
		for (size_t i = 0; i < m_EnumEntries.size(); ++i)
			m_EnumByName[m_EnumEntries[i].Schema.Name].push_back(i);
	}

	const TypeSchema* SchemaRegistry::Find(const std::string& typeIdName) const
	{
		const auto it = m_ByName.find(typeIdName);
		if (it == m_ByName.end() || it->second.empty())
			return nullptr;
		if (it->second.size() > 1)
		{
			RegistryWarn("SchemaRegistry: type '{}' is ambiguous across modules", typeIdName);
			return nullptr;
		}
		return &m_Entries[it->second.front()].Schema;
	}

	const TypeSchema* SchemaRegistry::FindByComponentId(uint32_t componentId) const
	{
		const auto it = m_ByComponentId.find(componentId);
		return it == m_ByComponentId.end() ? nullptr : &m_Entries[it->second].Schema;
	}

	const EnumSchema* SchemaRegistry::FindEnum(const std::string& name) const
	{
		const auto it = m_EnumByName.find(name);
		if (it == m_EnumByName.end() || it->second.empty())
			return nullptr;
		if (it->second.size() > 1)
		{
			RegistryWarn("SchemaRegistry: enum '{}' is ambiguous across modules", name);
			return nullptr;
		}
		return &m_EnumEntries[it->second.front()].Schema;
	}

	std::vector<const TypeSchema*> SchemaRegistry::List(TypeCategory category) const
	{
		std::vector<const TypeSchema*> result;
		for (const TypeEntry& entry : m_Entries)
			if (entry.Schema.Category == category)
				result.push_back(&entry.Schema);
		return result;
	}
}
