#pragma once

#include "World/Schema/Schema.h"

namespace YAML
{
	class Emitter;
	class Node;
}

namespace World::Schema
{
	// 序列化 visitor:结构信息来自 schema,实现只负责输出格式。
	class SchemaWriter
	{
	public:
		virtual ~SchemaWriter() = default;
		virtual bool BeginType(const TypeSchema& schema, void* instance) = 0;
		virtual bool EndType(const TypeSchema& schema) = 0;
		virtual bool WriteField(const FieldSchema& field, void* instance) = 0;
	};

	class SchemaReader
	{
	public:
		virtual ~SchemaReader() = default;
		// container 是当前类型对应的容器节点(实现自解释;YAML 实现期望 YAML::Node*)。
		virtual bool ReadFields(const TypeSchema& schema, void* instance, void* container) = 0;
	};

	// YAML 实现:读/写基于已链接的 yaml-cpp。
	class YamlSchemaWriter final : public SchemaWriter
	{
	public:
		explicit YamlSchemaWriter(YAML::Emitter& out);
		bool BeginType(const TypeSchema& schema, void* instance) override;
		bool EndType(const TypeSchema& schema) override;
		bool WriteField(const FieldSchema& field, void* instance) override;
		bool WriteValue(const FieldSchema& field, const Value& value);
	private:
		YAML::Emitter* m_Out = nullptr;
	};

	class YamlSchemaReader final : public SchemaReader
	{
	public:
		bool ReadFields(const TypeSchema& schema, void* instance, void* container) override;
		bool ReadFields(const TypeSchema& schema, void* instance, const YAML::Node& node);
		bool ReadFieldValue(const FieldSchema& field, Value* outValue, const YAML::Node& node);
	};
}
