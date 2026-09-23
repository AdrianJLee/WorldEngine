#include "wldpch.h"
#include "World/WUI/WuiJson.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <sstream>

namespace World::Wui
{
	JsonValue JsonValue::Null() { return {}; }
	JsonValue JsonValue::MakeBool(bool value)
	{
		JsonValue node;
		node.type = Type::Bool;
		node.Bool = value;
		return node;
	}
	JsonValue JsonValue::MakeNumber(double value)
	{
		JsonValue node;
		node.type = Type::Number;
		node.Number = value;
		return node;
	}
	JsonValue JsonValue::MakeString(std::string value)
	{
		JsonValue node;
		node.type = Type::String;
		node.String = std::move(value);
		return node;
	}

	const JsonValue* JsonValue::Find(const std::string& key) const
	{
		if (type != Type::Object)
			return nullptr;
		for (const auto& [name, value] : Object)
			if (name == key)
				return &value;
		return nullptr;
	}

	bool JsonValue::AsBool(bool fallback) const
	{
		return type == Type::Bool ? Bool : fallback;
	}

	double JsonValue::AsNumber(double fallback) const
	{
		return type == Type::Number ? Number : fallback;
	}

	std::string JsonValue::AsString(const std::string& fallback) const
	{
		return type == Type::String ? String : fallback;
	}

	namespace
	{
		void AppendEscaped(std::string& out, const std::string& text)
		{
			for (unsigned char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (c < 0x20)
						{
							char buffer[8];
							std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
							out += buffer;
						}
						else
							out.push_back(static_cast<char>(c));
				}
			}
		}
	}

	void JsonValue::Dump(std::string& out, int depth) const
	{
		switch (type)
		{
			case Type::Null: out += "null"; break;
			case Type::Bool: out += Bool ? "true" : "false"; break;
			case Type::Number:
			{
				std::ostringstream stream;
				stream << Number;
				out += stream.str();
				break;
			}
			case Type::String:
				out += '"';
				AppendEscaped(out, String);
				out += '"';
				break;
			case Type::Array:
			{
				out += '[';
				for (size_t i = 0; i < Array.size(); ++i)
				{
					if (i) out += ',';
					Array[i].Dump(out, depth + 1);
				}
				out += ']';
				break;
			}
			case Type::Object:
			{
				out += '{';
				for (size_t i = 0; i < Object.size(); ++i)
				{
					if (i) out += ',';
					out += '"';
					AppendEscaped(out, Object[i].first);
					out += "\":";
					Object[i].second.Dump(out, depth + 1);
				}
				out += '}';
				break;
			}
		}
	}

	std::string JsonValue::Dump() const
	{
		std::string out;
		Dump(out, 0);
		return out;
	}

	namespace
	{
		class JsonParser
		{
		public:
			explicit JsonParser(const std::string& text) : m_Text(text) {}

			std::optional<JsonValue> Parse(std::string* error)
			{
				SkipWhitespace();
				auto value = ParseValue(error);
				if (!value)
					return std::nullopt;
				SkipWhitespace();
				if (!AtEnd())
				{
					*error = "trailing characters at offset " + std::to_string(m_Index);
					return std::nullopt;
				}
				return value;
			}

		private:
			bool AtEnd() const { return m_Index >= m_Text.size(); }
			char Peek() const { return m_Text[m_Index]; }
			char Take() { return m_Text[m_Index++]; }

			void SkipWhitespace()
			{
				while (!AtEnd())
				{
					const char c = Peek();
					if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { Take(); continue; }
					break;
				}
			}

			bool Expect(char expected, std::string* error)
			{
				if (AtEnd() || Peek() != expected)
				{
					*error = "expected '" + std::string(1, expected) + "' at offset " + std::to_string(m_Index);
					return false;
				}
				Take();
				return true;
			}

			std::optional<JsonValue> ParseValue(std::string* error)
			{
				if (AtEnd())
				{
					*error = "unexpected end of input";
					return std::nullopt;
				}
				switch (Peek())
				{
					case '{': return ParseObject(error);
					case '[': return ParseArray(error);
					case '"': return ParseString(error);
					case 't': return ParseLiteral("true", JsonValue::MakeBool(true), error);
					case 'f': return ParseLiteral("false", JsonValue::MakeBool(false), error);
					case 'n': return ParseLiteral("null", JsonValue::Null(), error);
					default: return ParseNumber(error);
				}
			}

			std::optional<JsonValue> ParseLiteral(const char* literal, JsonValue value, std::string* error)
			{
				for (const char* p = literal; *p; ++p)
				{
					if (AtEnd() || Take() != *p)
					{
						*error = "invalid literal at offset " + std::to_string(m_Index);
						return std::nullopt;
					}
				}
				return value;
			}

			std::optional<JsonValue> ParseObject(std::string* error)
			{
				Take(); // '{'
				JsonValue object;
				object.type = JsonValue::Type::Object;
				SkipWhitespace();
				if (!AtEnd() && Peek() == '}')
				{
					Take();
					return object;
				}
				while (true)
				{
					SkipWhitespace();
					auto key = ParseString(error);
					if (!key)
						return std::nullopt;
					SkipWhitespace();
					if (!Expect(':', error))
						return std::nullopt;
					SkipWhitespace();
					auto value = ParseValue(error);
					if (!value)
						return std::nullopt;
					object.Object.emplace_back(key->String, std::move(*value));
					SkipWhitespace();
					if (!AtEnd() && Peek() == ',')
					{
						Take();
						continue;
					}
					if (!Expect('}', error))
						return std::nullopt;
					return object;
				}
			}

			std::optional<JsonValue> ParseArray(std::string* error)
			{
				Take(); // '['
				JsonValue array;
				array.type = JsonValue::Type::Array;
				SkipWhitespace();
				if (!AtEnd() && Peek() == ']')
				{
					Take();
					return array;
				}
				while (true)
				{
					SkipWhitespace();
					auto value = ParseValue(error);
					if (!value)
						return std::nullopt;
					array.Array.push_back(std::move(*value));
					SkipWhitespace();
					if (!AtEnd() && Peek() == ',')
					{
						Take();
						continue;
					}
					if (!Expect(']', error))
						return std::nullopt;
					return array;
				}
			}

			std::optional<JsonValue> ParseString(std::string* error)
			{
				Take(); // '"'
				std::string text;
				while (!AtEnd())
				{
					const char c = Take();
					if (c == '"')
						return JsonValue::MakeString(std::move(text));
					if (c != '\\')
					{
						text.push_back(c);
						continue;
					}
					if (AtEnd())
						break;
					const char escape = Take();
					switch (escape)
					{
						case '"': text.push_back('"'); break;
						case '\\': text.push_back('\\'); break;
						case '/': text.push_back('/'); break;
						case 'n': text.push_back('\n'); break;
						case 'r': text.push_back('\r'); break;
						case 't': text.push_back('\t'); break;
						case 'u':
						{
							if (m_Index + 4 > m_Text.size())
							{
								*error = "truncated unicode escape";
								return std::nullopt;
							}
							unsigned int code = 0;
							for (int i = 0; i < 4; ++i)
							{
								const char digit = Take();
								code <<= 4;
								if (digit >= '0' && digit <= '9') code |= digit - '0';
								else if (digit >= 'a' && digit <= 'f') code |= digit - 'a' + 10;
								else if (digit >= 'A' && digit <= 'F') code |= digit - 'A' + 10;
								else
								{
									*error = "invalid unicode escape";
									return std::nullopt;
								}
							}
							// UTF-8 编码(仅 BMP,足够本用途)。
							if (code < 0x80) text.push_back(static_cast<char>(code));
							else if (code < 0x800)
							{
								text.push_back(static_cast<char>(0xC0 | (code >> 6)));
								text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
							}
							else
							{
								text.push_back(static_cast<char>(0xE0 | (code >> 12)));
								text.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
								text.push_back(static_cast<char>(0x80 | (code & 0x3F)));
							}
							break;
						}
						default:
							*error = "invalid escape character";
							return std::nullopt;
					}
				}
				*error = "unterminated string";
				return std::nullopt;
			}

			std::optional<JsonValue> ParseNumber(std::string* error)
			{
				const size_t start = m_Index;
				if (!AtEnd() && (Peek() == '-' || Peek() == '+'))
					Take();
				bool anyDigit = false;
				while (!AtEnd() && Peek() >= '0' && Peek() <= '9')
				{
					Take();
					anyDigit = true;
				}
				if (!AtEnd() && Peek() == '.')
				{
					Take();
					while (!AtEnd() && Peek() >= '0' && Peek() <= '9') { Take(); anyDigit = true; }
				}
				if (!AtEnd() && (Peek() == 'e' || Peek() == 'E'))
				{
					Take();
					if (!AtEnd() && (Peek() == '+' || Peek() == '-')) Take();
					while (!AtEnd() && Peek() >= '0' && Peek() <= '9') { Take(); anyDigit = true; }
				}
				if (!anyDigit)
				{
					*error = "invalid number at offset " + std::to_string(start);
					return std::nullopt;
				}
				return JsonValue::MakeNumber(std::strtod(m_Text.c_str() + start, nullptr));
			}

			const std::string& m_Text;
			size_t m_Index = 0;
		};
	}

	std::optional<JsonValue> JsonValue::Parse(const std::string& text, std::string* error)
	{
		JsonParser parser(text);
		return parser.Parse(error);
	}
}
