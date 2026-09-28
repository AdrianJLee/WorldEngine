#include "wldpch.h"
#include "World/Schema/SchemaWriter.h"

#include <yaml-cpp/yaml.h>

namespace
{
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec2& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec3& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::vec4& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << value.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::ivec2& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::ivec3& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::ivec4& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << value.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::uvec2& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::uvec3& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::uvec4& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << value.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::quat& value)
	{
		out << YAML::Flow << YAML::BeginSeq << value.x << value.y << value.z << value.w << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::mat3& value)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				out << value[i][j];
		out << YAML::EndSeq;
		return out;
	}
	YAML::Emitter& operator<<(YAML::Emitter& out, const glm::mat4& value)
	{
		out << YAML::Flow << YAML::BeginSeq;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				out << value[i][j];
		out << YAML::EndSeq;
		return out;
	}

	template <typename F>
	bool Guard(const F& action)
	{
		try
		{
			action();
			return true;
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Schema YAML read failed: {0}", error.what());
			return false;
		}
	}
}

namespace World::Schema
{
	YamlSchemaWriter::YamlSchemaWriter(YAML::Emitter& out) : m_Out(&out) {}

	bool YamlSchemaWriter::BeginType(const TypeSchema&, void*)
	{
		*m_Out << YAML::BeginMap;
		return true;
	}

	bool YamlSchemaWriter::EndType(const TypeSchema&)
	{
		*m_Out << YAML::EndMap;
		return true;
	}

	bool YamlSchemaWriter::WriteField(const FieldSchema& field, void* instance)
	{
		if (field.Meta.Transient)
			return true;
		*m_Out << YAML::Key << field.Name << YAML::Value;
		// CPPT-6:容器字段(数组/映射)整值交给 WriteValue —— ValueList → flow seq、ValueMap → flow map;
		// 命名 struct 的元素/值同样是 ValueMap(键 = 字段名),递归同形。
		if (field.Collection != CollectionKind::None)
			return WriteValue(field, field.Get ? field.Get(instance) : Value {});
		if (field.K == Kind::Object)
		{
			const TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
			if (!nested || !nestedInstance)
			{
				*m_Out << YAML::Null;
				return true;
			}
			*m_Out << YAML::BeginMap;
			for (const FieldSchema& child : nested->Fields)
				WriteField(child, nestedInstance);
			*m_Out << YAML::EndMap;
			return true;
		}
		return WriteValue(field, field.Get(instance));
	}

	bool YamlSchemaWriter::WriteValue(const FieldSchema& field, const Value& value)
	{
		return WriteFieldValue(field, value);
	}

	bool YamlSchemaWriter::WriteFieldValue(const FieldSchema& field, const Value& value)
	{
		const TypeSchema* elementStruct = nullptr;
		if (field.ElementKind == Kind::Object && field.GetElementNested)
			elementStruct = field.GetElementNested();
		if (field.Collection == CollectionKind::Map)
			return WriteMapValue(value, field.ElementKind == Kind::Object ? elementStruct : nullptr);
		if (field.Collection == CollectionKind::Array)
			return WriteAnyValue(value, field.ElementKind == Kind::Object ? elementStruct : nullptr);
		if (field.K == Kind::Object)
			return WriteAnyValue(value, field.GetNested ? field.GetNested() : nullptr);
		return WriteAnyValue(value, nullptr);
	}

	bool YamlSchemaWriter::WriteMapValue(const Value& value, const TypeSchema* valueStructType)
	{
		const ValueMap* fields = std::get_if<ValueMap>(&value);
		if (!fields)
			return WriteAnyValue(value, nullptr);
		*m_Out << YAML::Flow << YAML::BeginMap;
		for (const auto& [key, item] : *fields)
		{
			*m_Out << YAML::Key << key << YAML::Value;
			WriteAnyValue(item, valueStructType);
		}
		*m_Out << YAML::EndMap;
		return true;
	}

	bool YamlSchemaWriter::WriteAnyValue(const Value& value, const TypeSchema* structType)
	{
		if (value.valueless_by_exception())
			return false;
		// CPPT-6:容器值(以及命名 struct 作为元素/值时的同形 map)先于叶 visit 处理。
		if (const ValueList* items = std::get_if<ValueList>(&value))
		{
			*m_Out << YAML::Flow << YAML::BeginSeq;
			for (const Value& item : *items)
				WriteAnyValue(item, structType);
			*m_Out << YAML::EndSeq;
			return true;
		}
		if (const ValueMap* fields = std::get_if<ValueMap>(&value))
		{
			*m_Out << YAML::Flow << YAML::BeginMap;
			if (structType)
			{
				// 命名 struct 的值:按声明顺序写;schema 里没有的键丢弃(与 WriteStructValue 同口径)。
				for (const FieldSchema& child : structType->Fields)
				{
					if (child.Meta.Transient)
						continue;
					const auto found = fields->find(child.Name);
					if (found == fields->end())
						continue;
					*m_Out << YAML::Key << child.Name << YAML::Value;
					WriteFieldValue(child, found->second);
				}
				*m_Out << YAML::EndMap;
				return true;
			}
			for (const auto& [key, item] : *fields)
			{
				*m_Out << YAML::Key << key << YAML::Value;
				WriteAnyValue(item, nullptr);
			}
			*m_Out << YAML::EndMap;
			return true;
		}
		std::visit([this](const auto& raw)
			{
				using T = std::decay_t<decltype(raw)>;
				if constexpr (std::is_same_v<T, std::monostate>)
					*m_Out << YAML::Null;
				// 容器替代项在上面已处理(这里只为编译期覆盖全部替代项;yaml-cpp 的
				// std::map 重载会尝试直接写 Value,必须显式短路)。
				else if constexpr (std::is_same_v<T, ValueList> || std::is_same_v<T, ValueMap>)
					(void)raw;
				else if constexpr (std::is_integral_v<T> && std::is_signed_v<T> && !std::is_same_v<T, bool>)
					*m_Out << static_cast<int64_t>(raw);
				else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
					*m_Out << static_cast<uint64_t>(raw);
				else
					*m_Out << raw;
			}, value);
		return true;
	}

	bool YamlSchemaReader::ReadFields(const TypeSchema& schema, void* instance, void* container)
	{
		if (!container)
			return false;
		return ReadFields(schema, instance, *static_cast<const YAML::Node*>(container));
	}

	bool YamlSchemaReader::ReadFields(const TypeSchema& schema, void* instance, const YAML::Node& node)
	{
		for (const FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const YAML::Node fieldNode = node[field.Name];
			if (!fieldNode)
				continue;
			if (field.Collection != CollectionKind::None)
			{
				Value value;
				if (ReadFieldValue(field, &value, fieldNode) && field.Set)
					field.Set(instance, value);
				continue;
			}
			if (field.K == Kind::Object)
			{
				const TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				if (nested && nestedInstance && fieldNode.IsMap())
					ReadFields(*nested, nestedInstance, fieldNode);
				continue;
			}
			Value value;
			if (ReadFieldValue(field, &value, fieldNode) && field.Set)
				field.Set(instance, value);
		}
		return true;
	}

	bool YamlSchemaReader::ReadFieldValue(const FieldSchema& field, Value* outValue, const YAML::Node& node)
	{
		if (!outValue)
			return false;
		if (field.Collection != CollectionKind::None)
			return ReadContainerValue(field, node, outValue);
		switch (field.K)
		{
			case Kind::Bool: return Guard([&] { *outValue = Value(node.as<bool>()); });
			case Kind::Int8: return Guard([&] { *outValue = Value(node.as<int8_t>()); });
			case Kind::Int16: return Guard([&] { *outValue = Value(node.as<int16_t>()); });
			case Kind::Int32: return Guard([&] { *outValue = Value(node.as<int32_t>()); });
			case Kind::Int64: return Guard([&] { *outValue = Value(node.as<int64_t>()); });
			case Kind::UInt8: return Guard([&] { *outValue = Value(node.as<uint8_t>()); });
			case Kind::UInt16: return Guard([&] { *outValue = Value(node.as<uint16_t>()); });
			case Kind::UInt32: return Guard([&] { *outValue = Value(node.as<uint32_t>()); });
			case Kind::UInt64: return Guard([&] { *outValue = Value(node.as<uint64_t>()); });
			case Kind::Float: return Guard([&] { *outValue = Value(node.as<float>()); });
			case Kind::Double: return Guard([&] { *outValue = Value(node.as<double>()); });
			case Kind::Vec2: return Guard([&] { glm::vec2 v; v.x = node[0].as<float>(); v.y = node[1].as<float>(); *outValue = Value(v); });
			case Kind::Vec3: return Guard([&] { glm::vec3 v; v.x = node[0].as<float>(); v.y = node[1].as<float>(); v.z = node[2].as<float>(); *outValue = Value(v); });
			case Kind::Vec4: return Guard([&] { glm::vec4 v; v.x = node[0].as<float>(); v.y = node[1].as<float>(); v.z = node[2].as<float>(); v.w = node[3].as<float>(); *outValue = Value(v); });
			case Kind::IVec2: return Guard([&] { glm::ivec2 v; v.x = node[0].as<int>(); v.y = node[1].as<int>(); *outValue = Value(v); });
			case Kind::IVec3: return Guard([&] { glm::ivec3 v; v.x = node[0].as<int>(); v.y = node[1].as<int>(); v.z = node[2].as<int>(); *outValue = Value(v); });
			case Kind::IVec4: return Guard([&] { glm::ivec4 v; v.x = node[0].as<int>(); v.y = node[1].as<int>(); v.z = node[2].as<int>(); v.w = node[3].as<int>(); *outValue = Value(v); });
			case Kind::UVec2: return Guard([&] { glm::uvec2 v; v.x = node[0].as<uint32_t>(); v.y = node[1].as<uint32_t>(); *outValue = Value(v); });
			case Kind::UVec3: return Guard([&] { glm::uvec3 v; v.x = node[0].as<uint32_t>(); v.y = node[1].as<uint32_t>(); v.z = node[2].as<uint32_t>(); *outValue = Value(v); });
			case Kind::UVec4: return Guard([&] { glm::uvec4 v; v.x = node[0].as<uint32_t>(); v.y = node[1].as<uint32_t>(); v.z = node[2].as<uint32_t>(); v.w = node[3].as<uint32_t>(); *outValue = Value(v); });
			case Kind::Quat: return Guard([&] { glm::quat v; v.x = node[0].as<float>(); v.y = node[1].as<float>(); v.z = node[2].as<float>(); v.w = node[3].as<float>(); *outValue = Value(v); });
			case Kind::Mat3: return Guard([&] { glm::mat3 v; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) v[i][j] = node[i * 3 + j].as<float>(); *outValue = Value(v); });
			case Kind::Mat4: return Guard([&] { glm::mat4 v; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) v[i][j] = node[i * 4 + j].as<float>(); *outValue = Value(v); });
			case Kind::String: return Guard([&] { *outValue = Value(node.as<std::string>()); });
			case Kind::Enum:
			{
				const EnumSchema* enumSchema = field.GetEnum ? field.GetEnum() : nullptr;
				if (!enumSchema)
					return false;
				if (enumSchema->IsSigned)
					return Guard([&] { *outValue = Value(node.as<int64_t>()); });
				return Guard([&] { *outValue = Value(node.as<uint64_t>()); });
			}
			case Kind::Asset: return Guard([&] { *outValue = Value(node.as<std::string>()); });
			default: return false;
		}
	}

	// CPPT-6:读"一个字段自己的值"(结构子字段 / 容器字段都用它分派)。
	bool YamlSchemaReader::ReadChildValue(const FieldSchema& field, const YAML::Node& node, Value* outValue)
	{
		if (!outValue || !node || node.IsNull())
			return false;
		if (field.Collection != CollectionKind::None)
			return ReadElementValue(field, node, outValue);
		if (field.K == Kind::Object)
		{
			const TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
			if (!nested)
				return false;
			return ReadStructValue(*nested, node, outValue);
		}
		return ReadFieldValue(field, outValue, node);
	}

	bool YamlSchemaReader::ReadElementValue(const FieldSchema& container, const YAML::Node& node, Value* outValue)
	{
		if (!outValue || !node || node.IsNull())
			return false;
		if (container.ElementKind == Kind::Object)
		{
			const TypeSchema* nested = container.GetElementNested ? container.GetElementNested() : nullptr;
			if (!nested)
				return false;
			return ReadStructValue(*nested, node, outValue);
		}
		// 叶元素:用一个形状为"元素类型"的 probe 复用既有叶读写器(Enum 用 GetEnum 判有符号,
		// Asset 读逻辑路径字符串)。
		FieldSchema probe;
		probe.K = container.ElementKind;
		probe.GetEnum = container.GetEnum;
		probe.AssetTypeName = container.AssetTypeName;
		return ReadFieldValue(probe, outValue, node);
	}

	bool YamlSchemaReader::ReadContainerValue(const FieldSchema& field, const YAML::Node& node, Value* outValue)
	{
		if (!outValue)
			return false;
		if (field.Collection == CollectionKind::Array)
		{
			if (!node.IsSequence())
				return false;
			ValueList items;
			items.reserve(node.size());
			for (const YAML::Node& element : node)
			{
				Value item;
				if (!ReadElementValue(field, element, &item))
					return false;
				items.push_back(std::move(item));
			}
			*outValue = Value(std::move(items));
			return true;
		}
		if (field.Collection == CollectionKind::Map)
		{
			if (!node.IsMap())
				return false;
			ValueMap fields;
			for (const auto& entry : node)
			{
				Value item;
				if (!ReadElementValue(field, entry.second, &item))
					return false;
				fields.emplace(entry.first.as<std::string>(), std::move(item));
			}
			*outValue = Value(std::move(fields));
			return true;
		}
		return false;
	}

	// 命名 struct 的值 = 字段名 → Value 的 map(容器元素的 ValueMap 同形);
	// 缺失/空字段留成 monostate(未设 → 写入时保留实例成员初值),子字段本身可以是
	// 嵌套 Object 或容器(命名 struct 允许再套容器),统一由 ReadChildValue 递归分派。
	bool YamlSchemaReader::ReadStructValue(const TypeSchema& type, const YAML::Node& node, Value* outValue)
	{
		if (!outValue || !node.IsMap())
			return false;
		ValueMap fields;
		for (const FieldSchema& child : type.Fields)
		{
			if (child.Meta.Transient)
				continue;
			const YAML::Node childNode = node[child.Name];
			if (!childNode || childNode.IsNull())
			{
				fields.emplace(child.Name, Value {});   // 未设:保留成员初值
				continue;
			}
			Value item;
			if (!ReadChildValue(child, childNode, &item))
				return false;
			fields.emplace(child.Name, std::move(item));
		}
		*outValue = Value(std::move(fields));
		return true;
	}
}
