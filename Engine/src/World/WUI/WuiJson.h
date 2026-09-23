#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace World::Wui
{
	// 最小 JSON 值/解析器/序列化器:为布局持久化与配置提供无外部依赖的数据格式。
	class JsonValue
	{
	public:
		enum class Type : uint8_t
		{
			Null,
			Bool,
			Number,
			String,
			Array,
			Object,
		};

		Type type = Type::Null;
		bool Bool = false;
		double Number = 0;
		std::string String;
		std::vector<JsonValue> Array;
		std::vector<std::pair<std::string, JsonValue>> Object;

		static JsonValue Null();
		static JsonValue MakeBool(bool value);
		static JsonValue MakeNumber(double value);
		static JsonValue MakeString(std::string value);

		const JsonValue* Find(const std::string& key) const;
		bool AsBool(bool fallback = false) const;
		double AsNumber(double fallback = 0) const;
		std::string AsString(const std::string& fallback = {}) const;

		std::string Dump() const;
		static std::optional<JsonValue> Parse(const std::string& text, std::string* error);

	private:
		void Dump(std::string& out, int depth) const;
	};
}
