#pragma once

#include "World/Schema/Schema.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Schema
{
	// 宿主唯一持有的注册表。模块只拿到引用,模块内部不存在注册表单例。
	class SchemaRegistry
	{
	public:
		enum class Status : uint8_t
		{
			Ok,
			DuplicateType,        // 同模块重复提交同一 TypeId
			DuplicateComponentId, // 不同模块共用同一组件存储 id
			Conflict,             // 跨模块同名组件等不允许的组合
			AbiMismatch,          // schema ABI 版本超出宿主支持
		};

		static const char* StatusName(Status status);

		Status Register(const ModuleId& module, const TypeSchema& schema);
		Status RegisterEnum(const ModuleId& module, const EnumSchema& schema);
		// 事务化:全部校验通过才提交;任一失败不留下任何条目。
		Status RegisterModule(const ModuleId& module, const std::vector<TypeSchema>& schemas);
		void UnregisterModule(const ModuleId& module);

		// 按 TypeId 全名查找;跨模块同名时歧义,返回 nullptr 并告警。
		const TypeSchema* Find(const std::string& typeIdName) const;
		const TypeSchema* FindByComponentId(uint32_t componentId) const;
		const EnumSchema* FindEnum(const std::string& name) const;

		std::vector<const TypeSchema*> List(TypeCategory category) const;

		size_t TypeCount() const { return m_Entries.size(); }
		size_t EnumCount() const { return m_EnumEntries.size(); }

	private:
		struct TypeEntry
		{
			ModuleId Module;
			TypeSchema Schema;
		};
		struct EnumEntry
		{
			ModuleId Module;
			EnumSchema Schema;
		};

		bool ValidateNew(const ModuleId& module, const TypeSchema& schema, Status* outStatus) const;

		std::vector<TypeEntry> m_Entries;                              // 稳定顺序
		std::unordered_map<std::string, std::vector<size_t>> m_ByName; // TypeId.Name -> 索引
		std::unordered_map<uint32_t, size_t> m_ByComponentId;

		std::vector<EnumEntry> m_EnumEntries;
		std::unordered_map<std::string, std::vector<size_t>> m_EnumByName;
	};
}
