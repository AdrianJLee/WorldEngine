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

	bool YamlSchemaWriter::WriteValue(const FieldSchema&, const Value& value)
	{
		if (value.valueless_by_exception())
			return false;
		std::visit([this](const auto& raw)
			{
				using T = std::decay_t<decltype(raw)>;
				if constexpr (std::is_same_v<T, std::monostate>)
					*m_Out << YAML::Null;
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
}
