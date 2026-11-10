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
		// 字段值分派:容器(数组/映射)、命名 struct、叶值各走一条;命名 struct 的值按
		// schema 字段顺序写(std::map 的键顺序不能当文档顺序),映射的键按容器自身顺序写。
		bool WriteFieldValue(const FieldSchema& field, const Value& value);
		bool WriteMapValue(const Value& value, const TypeSchema* valueStructType);
		// 一个"任意值"(容器元素 / 命名 struct 字段):递归写 seq/map,叶值走 visit。
		// structType 非空 = 这个 ValueMap 是"命名 struct 的值"。
		bool WriteAnyValue(const Value& value, const TypeSchema* structType);
		YAML::Emitter* m_Out = nullptr;
	};

	class YamlSchemaReader final : public SchemaReader
	{
	public:
		bool ReadFields(const TypeSchema& schema, void* instance, void* container) override;
		bool ReadFields(const TypeSchema& schema, void* instance, const YAML::Node& node);
		bool ReadFieldValue(const FieldSchema& field, Value* outValue, const YAML::Node& node);
		// CPPT-6:容器(Array/Map)与命名 struct 值(字段名 → Value 的 map)。生成物/序列化的
		// 容器字段走这里:元素是 Object 时用 field.GetElementNested() 递归读字段。
		bool ReadContainerValue(const FieldSchema& field, const YAML::Node& node, Value* outValue);
		bool ReadStructValue(const TypeSchema& type, const YAML::Node& node, Value* outValue);
	private:
		// 一个"字段自己的值":容器 → 递归,Object → 递归读字段,其它 → 叶读写器。
		bool ReadChildValue(const FieldSchema& field, const YAML::Node& node, Value* outValue);
		// 容器的一个元素/值:按容器的 ElementKind 走(Object → GetElementNested,否则叶读写器)。
		bool ReadElementValue(const FieldSchema& container, const YAML::Node& node, Value* outValue);
	};
}
