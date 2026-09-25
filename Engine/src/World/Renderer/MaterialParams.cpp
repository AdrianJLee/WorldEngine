#include "wldpch.h"

#include "World/Renderer/MaterialParams.h"

#include "World/Renderer/MaterialSurface.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr uint32_t kParamBlobAlignment = 16;
		constexpr uint32_t kScalarSize = 4;
		constexpr const char* kParamPermutationKey = "material-params";

		std::string Trim(const std::string& text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

		// MAT-UI7a:注解里的双引号字符串(unit/group/label/doc 与 Texture2D 默认值)写出时转义,
		// 与 AnnotationLine::Unquote 的读取规则一一对应:`\"` / `\\` / `\n`。
		// 不转义的话含引号/反斜杠的说明文本写出去就读不回来(既有字段同样的潜在缺陷,一并收口)。
		std::string QuoteAnnotationText(const std::string& value)
		{
			std::string out;
			out.reserve(value.size() + 2);
			out.push_back('"');
			for (const char ch : value)
			{
				if (ch == '\\' || ch == '"')
				{
					out.push_back('\\');
					out.push_back(ch);
				}
				else if (ch == '\n')
				{
					out += "\\n";
				}
				else
				{
					out.push_back(ch);
				}
			}
			out.push_back('"');
			return out;
		}

		// MAT-UI7a:doc(...) 文本超长时截断到 kMaxMaterialParamDocBytes;按 UTF-8 码点边界回退,
		// 不会留下半个多字节字符(解析与写出共用,保证"写出 → 读回"稳定)。
		std::string TruncateDocText(const std::string& text)
		{
			if (text.size() <= kMaxMaterialParamDocBytes)
				return text;
			size_t end = kMaxMaterialParamDocBytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u)
				--end;
			return text.substr(0, end);
		}

		// MAT-UI7a:注解行尾的 `// 注释` 起点(返回 npos = 没有注释)。只认**引号外**、且前面是
		// 空白(或 `//` 就在注解体段首)的双斜杠:
		//   - 字符串里的 `//`(如 `"textures//x.png"`、说明文本里的 URL)被跳过,不受影响;
		//   - 没有空白前缀的 `//`(如不带引号的路径 `textures//x.png`)也不是注释,保持原义。
		// 未闭合的字符串直接交给后面的解析器报"引号没有闭合",这里不猜。
		size_t FindAnnotationCommentStart(const std::string& text, size_t from)
		{
			bool inString = false;
			for (size_t index = from; index + 1 < text.size(); ++index)
			{
				const char ch = text[index];
				if (inString)
				{
					if (ch == '\\')
					{
						++index;
						continue;
					}
					if (ch == '"')
						inString = false;
					continue;
				}
				if (ch == '"')
				{
					inString = true;
					continue;
				}
				if (ch == '/' && text[index + 1] == '/'
					&& (index == from || text[index - 1] == ' ' || text[index - 1] == '\t'))
					return index;
			}
			return std::string::npos;
		}

		bool IsIdentifierStart(char ch)
		{
			return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
		}

		bool IsIdentifierBody(char ch)
		{
			return IsIdentifierStart(ch) || (ch >= '0' && ch <= '9');
		}

		bool IsValidIdentifier(const std::string& text)
		{
			if (text.empty() || !IsIdentifierStart(text.front()))
				return false;
			for (const char ch : text)
				if (!IsIdentifierBody(ch))
					return false;
			return true;
		}

		// 与引擎模板/uniform 冲突的参数名:这些名字进参数块后必然编译不过,提前给可读错误
		// (其余冲突仍由 Slang 的原始诊断兜底)。
		const char* const kReservedParamNames[] = {
			"Surface", "MaterialInputs", "SurfaceVSInput", "SurfaceVSOutput",
			"SurfaceInstancedInput", "SurfaceSkinnedInput", "SurfacePSOutput", "GpuLight",
			"VSMain", "VSMainInstanced", "VSMainSkinned", "PSMain", "Evaluate",
			"MakeDefaultSurface", "BuildVSOutput", "BuildMaterialInputs",
			"ResolveShadingNormal", "EvaluateEngineLighting", "ApplyEngineFog",
			"WeLinearizeColor", "WeDefaultNormal", "ComputeSkinPalette",
			"SampleDirectionalShadow", "MaterialParams",
			"input", "output", "surface", "materialInputs", "instance", "materialParams",
		};

		bool IsReservedParamName(const std::string& name)
		{
			// u_ 前缀 = 引擎 uniform(ObjectUniforms / LightUniforms / …):参数块里不得复用。
			if (name.rfind("u_", 0) == 0)
				return true;
			for (const char* reserved : kReservedParamNames)
				if (name == reserved)
					return true;
			return false;
		}

		std::string MakeError(uint32_t line, size_t column, const std::string& reason)
		{
			return std::to_string(line) + ":" + std::to_string(column) + ": " + reason;
		}

		std::string Join(const std::vector<std::string>& parts)
		{
			std::string out;
			for (const std::string& part : parts)
			{
				if (!out.empty())
					out.append("; ");
				out.append(part);
			}
			return out;
		}

		std::vector<std::string> SplitCommaList(const std::string& text)
		{
			std::vector<std::string> parts;
			std::string current;
			for (const char ch : text)
			{
				if (ch == ',')
				{
					parts.push_back(Trim(current));
					current.clear();
				}
				else
				{
					current.push_back(ch);
				}
			}
			parts.push_back(Trim(current));
			return parts;
		}

		// HLSL 风格数字字面量。接受尾缀 f/F(注解里写 0.5f 是常态),拒绝 NaN/Inf/空串。
		bool ParseNumber(const std::string& rawText, double* out)
		{
			std::string text = Trim(rawText);
			if (text.empty())
				return false;
			if (text.back() == 'f' || text.back() == 'F')
				text.pop_back();
			if (text.empty())
				return false;
			if (text.find_first_not_of("+-.0123456789eE") != std::string::npos)
				return false;
			char* end = nullptr;
			const double value = std::strtod(text.c_str(), &end);
			if (end == nullptr || *end != '\0' || !std::isfinite(value))
				return false;
			if (out) *out = value;
			return true;
		}

		std::string FormatFloat(float value)
		{
			// 与 Material.cpp 的 FormatFloat 同一口径(最短往返;非有限值走定点回退,
			// 让 ParseNumber 按原路径拒绝)。两处都要改的时候必须一起对齐 ——
			// "参数覆盖是否等于注解默认值"的比较依赖它。
			if (std::isfinite(value))
			{
				char buffer[64] = {};
				const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
				if (result.ec == std::errc())
					return std::string(buffer, result.ptr);
			}
			std::ostringstream stream;
			stream.precision(6);
			stream << std::fixed << value;
			std::string text = stream.str();
			while (text.size() > 3 && text.back() == '0' && text[text.size() - 2] != '.')
				text.pop_back();
			return text;
		}

		bool ParseIntText(const std::string& rawText, int* out)
		{
			const std::string text = Trim(rawText);
			if (text.empty() || text.back() == 'f' || text.back() == 'F')
				return false;
			int value = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			if (result.ec != std::errc() || result.ptr != text.data() + text.size())
				return false;
			if (out) *out = value;
			return true;
		}

		bool ParseBoolText(const std::string& rawText, bool* out)
		{
			const std::string text = Trim(rawText);
			if (text == "true" || text == "True" || text == "TRUE" || text == "1")
			{
				if (out) *out = true;
				return true;
			}
			if (text == "false" || text == "False" || text == "FALSE" || text == "0")
			{
				if (out) *out = false;
				return true;
			}
			return false;
		}

		int ComponentCount(ParamType type)
		{
			switch (type)
			{
				case ParamType::Vec2: return 2;
				case ParamType::Vec3: return 3;
				case ParamType::Vec4: case ParamType::Color: return 4;
				default: return 1;
			}
		}

		// 反射类型 ↔ 注解类型。HLSL 的 bool 在 SPIR-V 参数块里是 uint32;
		// Color 与 Vec4 都是 v4float(两者不能靠反射区分 —— 采用注解里的类型)。
		bool ReflectedTypeMatches(ParamType declared, const std::string& reflected)
		{
			switch (declared)
			{
				case ParamType::Float: return reflected == "float";
				case ParamType::Vec2: return reflected == "v2float" || reflected == "float2";
				case ParamType::Vec3: return reflected == "v3float" || reflected == "float3";
				case ParamType::Vec4: case ParamType::Color:
					return reflected == "v4float" || reflected == "float4";
				case ParamType::Int: return reflected == "int";
				case ParamType::Bool: return reflected == "uint" || reflected == "bool";
				case ParamType::Texture2D: return true;   // 贴图槽按 set/binding 单独校验
			}
			return false;
		}

		ParamType DeriveTypeFromReflected(const std::string& reflected)
		{
			if (reflected == "float") return ParamType::Float;
			if (reflected == "v2float" || reflected == "float2") return ParamType::Vec2;
			if (reflected == "v3float" || reflected == "float3") return ParamType::Vec3;
			if (reflected == "v4float" || reflected == "float4") return ParamType::Vec4;
			if (reflected == "int") return ParamType::Int;
			if (reflected == "uint" || reflected == "bool") return ParamType::Bool;
			return ParamType::Float;
		}

		// ---- 单行注解解析(带行列号) ----

		class AnnotationLine
		{
		public:
			AnnotationLine(const std::string& text, uint32_t lineNumber)
				: m_Text(text), m_Line(lineNumber)
			{
			}

			bool Parse(size_t startColumn)
			{
				m_Pos = startColumn;
				SkipSpaces();
				const size_t keywordColumn = m_Pos + 1;
				const std::string keyword = ReadIdentifier();
				if (keyword.empty())
					return Fail(keywordColumn, "缺少注解指令(应为 'param')");
				if (keyword != "param")
					return Fail(keywordColumn, "未知注解指令 '" + keyword + "'(只支持 'param')");

				SkipSpaces();
				const size_t typeColumn = m_Pos + 1;
				const std::string typeName = ReadIdentifier();
				if (!ParseParamTypeName(typeName, &m_Decl.Type))
				{
					return Fail(typeColumn, "未知参数类型 '" + typeName
						+ "'(可用:Float/Vec2/Vec3/Vec4/Color/Int/Bool/Texture2D)");
				}

				SkipSpaces();
				const size_t nameColumn = m_Pos + 1;
				m_Decl.Name = ReadIdentifier();
				m_NameColumn = nameColumn;
				if (m_Decl.Name.empty())
					return Fail(nameColumn, "缺少参数名");
				if (!IsUsableParamName(m_Decl.Name))
				{
					if (!IsValidIdentifier(m_Decl.Name))
						return Fail(nameColumn, "参数名 '" + m_Decl.Name + "' 不是合法的 HLSL 标识符");
					return Fail(nameColumn, "参数名 '" + m_Decl.Name
						+ "' 与引擎模板/uniform 保留名冲突(u_ 前缀与模板标识符不可用)");
				}

				SkipSpaces();
				const size_t equalColumn = m_Pos + 1;
				if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '=')
					return Fail(equalColumn, "缺少默认值(需要 '= <默认值>')");
				++m_Pos;
				SkipSpaces();
				const size_t defaultColumn = m_Pos + 1;
				const size_t defaultEnd = FindOptionStart();
				std::string defaultText = Trim(m_Text.substr(m_Pos, defaultEnd - m_Pos));
				m_Pos = defaultEnd;
				if (defaultText.empty())
					return Fail(equalColumn, "缺少默认值(需要 '= <默认值>')");
				if (defaultText.front() == '"')
				{
					std::string unquoted;
					if (!Unquote(defaultText, &unquoted))
						return Fail(defaultColumn, "默认值的引号没有闭合");
					defaultText = unquoted;
				}

				std::string normalized;
				std::string reason;
				if (!NormalizeParamValue(m_Decl.Type, defaultText, &normalized, &reason))
					return Fail(defaultColumn, "默认值 '" + defaultText + "' 不合法:" + reason);
				m_Decl.Default = normalized;

				if (!ParseOptions())
					return false;
				return ApplyRangeToDefault(defaultColumn);
			}

			const MaterialParamDecl& Decl() const { return m_Decl; }
			const std::string& Error() const { return m_Error; }
			size_t NameColumn() const { return m_NameColumn; }

		private:
			bool Fail(size_t column, const std::string& reason)
			{
				m_Error = MakeError(m_Line, column, reason);
				return false;
			}

			static bool IsSpace(char ch) { return ch == ' ' || ch == '\t'; }

			void SkipSpaces()
			{
				while (m_Pos < m_Text.size() && IsSpace(m_Text[m_Pos]))
					++m_Pos;
			}

			std::string ReadIdentifier()
			{
				const size_t begin = m_Pos;
				if (m_Pos < m_Text.size() && IsIdentifierStart(m_Text[m_Pos]))
				{
					++m_Pos;
					while (m_Pos < m_Text.size() && IsIdentifierBody(m_Text[m_Pos]))
						++m_Pos;
				}
				return m_Text.substr(begin, m_Pos - begin);
			}

			// 默认值区段的终点:下一个选项('[' 或 `unit(`/`group(`/`label(`)。
			size_t FindOptionStart() const
			{
				for (size_t index = m_Pos; index < m_Text.size(); ++index)
				{
					if (m_Text[index] == '"')
					{
						// 跳过字符串里的内容(默认值允许写成 "textures/x.png")。
						++index;
						while (index < m_Text.size() && m_Text[index] != '"')
							++index;
						continue;
					}
					if (m_Text[index] == '[' && (index == m_Pos || IsSpace(m_Text[index - 1])))
						return index;
					if (IsOptionBoundaryAt(index))
						return index;
				}
				return m_Text.size();
			}

			// 选项边界 = 词边界上的 `标识符(`。不限定 unit/group/label:未知字段也要
			// 在 ParseOptions 里报"无法识别的注解内容",而不是被吞进默认值里。
			bool IsOptionBoundaryAt(size_t index) const
			{
				if (index > m_Pos && !IsSpace(m_Text[index - 1]))
					return false;
				if (index >= m_Text.size() || !IsIdentifierStart(m_Text[index]))
					return false;
				size_t cursor = index;
				while (cursor < m_Text.size() && IsIdentifierBody(m_Text[cursor]))
					++cursor;
				return cursor < m_Text.size() && m_Text[cursor] == '(';
			}

			static bool Unquote(const std::string& text, std::string* out)
			{
				if (text.size() < 2 || text.front() != '"' || text.back() != '"')
					return false;
				std::string result;
				for (size_t index = 1; index + 1 < text.size(); ++index)
				{
					if (text[index] == '\\' && index + 2 < text.size())
					{
						++index;
						result.push_back(text[index] == 'n' ? '\n' : text[index]);
						continue;
					}
					if (text[index] == '"')
						return false;
					result.push_back(text[index]);
				}
				if (out) *out = result;
				return true;
			}

			size_t FindStringEnd() const
			{
				size_t index = m_Pos + 1;
				while (index < m_Text.size())
				{
					if (m_Text[index] == '\\')
					{
						index += 2;
						continue;
					}
					if (m_Text[index] == '"')
						return index;
					++index;
				}
				return std::string::npos;
			}

			bool ParseOptions()
			{
				for (;;)
				{
					SkipSpaces();
					if (m_Pos >= m_Text.size())
						break;
					const size_t optionColumn = m_Pos + 1;
					if (m_Text[m_Pos] == '[')
					{
						const size_t close = m_Text.find(']', m_Pos);
						if (close == std::string::npos)
							return Fail(optionColumn, "范围缺少 ']'");
						const std::string body = m_Text.substr(m_Pos + 1, close - m_Pos - 1);
						m_Pos = close + 1;
						if (m_HasRange)
							return Fail(optionColumn, "范围 [min,max] 重复");
						if (m_Decl.Type != ParamType::Float && m_Decl.Type != ParamType::Int)
							return Fail(optionColumn, "范围 [min,max] 只适用于 Float/Int");
						const std::vector<std::string> parts = SplitCommaList(body);
						double min = 0.0;
						double max = 0.0;
						if (parts.size() != 2 || !ParseNumber(parts[0], &min) || !ParseNumber(parts[1], &max))
							return Fail(optionColumn, "范围需要 '[min,max]' 两个数字");
						if (min > max)
							return Fail(optionColumn, "范围下界 " + parts[0] + " 大于上界 " + parts[1]);
						m_Decl.Min = static_cast<float>(min);
						m_Decl.Max = static_cast<float>(max);
						m_HasRange = true;
						continue;
					}

					std::string field;
					if (m_Text.compare(m_Pos, 5, "unit(") == 0) field = "unit";
					else if (m_Text.compare(m_Pos, 6, "group(") == 0) field = "group";
					else if (m_Text.compare(m_Pos, 6, "label(") == 0) field = "label";
					else if (m_Text.compare(m_Pos, 4, "doc(") == 0) field = "doc";
					else return Fail(optionColumn, "无法识别的注解内容 '" + Trim(m_Text.substr(m_Pos)) + "'");

					m_Pos += field.size() + 1;
					const size_t valueColumn = m_Pos + 1;
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
						return Fail(valueColumn, field + " 需要双引号字符串");
					const size_t close = FindStringEnd();
					if (close == std::string::npos)
						return Fail(valueColumn, field + " 的引号没有闭合");
					std::string value;
					if (!Unquote(m_Text.substr(m_Pos, close - m_Pos + 1), &value))
						return Fail(valueColumn, field + " 的字符串不合法");
					m_Pos = close + 1;
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != ')')
						return Fail(m_Pos + 1, field + " 缺少右括号 ')'");
					++m_Pos;
					if (m_SeenFields.count(field) != 0)
						return Fail(optionColumn, "字段 " + field + "(...) 重复");
					m_SeenFields.insert(field);
					if (field == "unit") m_Decl.Unit = value;
					else if (field == "group") m_Decl.Group = value;
					else if (field == "label") m_Decl.Label = value;
					else m_Decl.Doc = TruncateDocText(value);
				}
				return true;
			}

			// 写了 [min,max] 时默认值必须落在区间内(前后自洽,不留给运行期夹紧)。
			bool ApplyRangeToDefault(size_t defaultColumn)
			{
				if (!m_HasRange)
					return true;
				if (m_Decl.Type == ParamType::Float)
				{
					float value = 0.0f;
					if (!ParseParamFloat(m_Decl.Default, &value))
						return true;
					if (value < m_Decl.Min || value > m_Decl.Max)
					{
						return Fail(defaultColumn, "默认值 " + FormatFloat(value) + " 超出范围 ["
							+ FormatFloat(m_Decl.Min) + ", " + FormatFloat(m_Decl.Max) + "]");
					}
				}
				else if (m_Decl.Type == ParamType::Int)
				{
					int value = 0;
					if (!ParseIntText(m_Decl.Default, &value))
						return true;
					if (static_cast<float>(value) < m_Decl.Min || static_cast<float>(value) > m_Decl.Max)
					{
						return Fail(defaultColumn, "默认值 " + std::to_string(value) + " 超出范围 ["
							+ FormatFloat(m_Decl.Min) + ", " + FormatFloat(m_Decl.Max) + "]");
					}
				}
				return true;
			}

			const std::string& m_Text;
			uint32_t m_Line = 0;
			size_t m_Pos = 0;
			bool m_HasRange = false;
			size_t m_NameColumn = 0;
			std::string m_Error;
			MaterialParamDecl m_Decl;
			std::unordered_set<std::string> m_SeenFields;
		};

		// ---- Slang 反射 JSON(Slang-T3) ----
		//
		// 只用到 JSON 的一个子集(对象/数组/字符串/数字/布尔/null),键名与结构由 Slang 自己生成、
		// 跨版本稳定;因此这里自带一个小解析器:不引入第三方依赖,反射 JSON 只有几十 KB。
		struct JsonValue
		{
			enum class Kind { Null, Bool, Number, String, Array, Object };

			Kind ValueKind = Kind::Null;
			bool Boolean = false;
			double Number = 0.0;
			std::string Text;                                          // Kind::String
			std::vector<JsonValue> Items;                              // Kind::Array
			std::vector<std::pair<std::string, JsonValue>> Members;    // Kind::Object

			const JsonValue* Find(const char* key) const
			{
				if (ValueKind != Kind::Object)
					return nullptr;
				for (const auto& member : Members)
					if (member.first == key)
						return &member.second;
				return nullptr;
			}

			bool IsNumber() const { return ValueKind == Kind::Number; }
			bool IsString() const { return ValueKind == Kind::String; }
		};

		class JsonReader
		{
		public:
			explicit JsonReader(const std::string& text) : m_Text(text) {}

			bool Parse(JsonValue* out, std::string* error)
			{
				if (!ParseValue(out, 0))
				{
					if (error) *error = m_Error.empty() ? "反射 JSON 解析失败" : m_Error;
					return false;
				}
				SkipWhitespace();
				return true;
			}

		private:
			void SkipWhitespace()
			{
				while (m_Pos < m_Text.size())
				{
					const char ch = m_Text[m_Pos];
					if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
						++m_Pos;
					else
						break;
				}
			}

			bool Fail(const char* reason)
			{
				m_Error = std::string("反射 JSON 解析失败(") + reason + ",偏移 "
					+ std::to_string(m_Pos) + ")";
				return false;
			}

			bool ParseValue(JsonValue* out, int depth)
			{
				if (depth > 64)
					return Fail("嵌套过深");
				SkipWhitespace();
				if (m_Pos >= m_Text.size())
					return Fail("意外结束");
				const char ch = m_Text[m_Pos];
				if (ch == '{')
					return ParseObject(out, depth);
				if (ch == '[')
					return ParseArray(out, depth);
				if (ch == '"')
				{
					out->ValueKind = JsonValue::Kind::String;
					return ParseString(&out->Text);
				}
				if (ch == 't' || ch == 'f')
				{
					const bool value = ch == 't';
					const char* literal = value ? "true" : "false";
					const size_t length = value ? 4u : 5u;
					if (m_Text.compare(m_Pos, length, literal) != 0)
						return Fail("布尔字面量");
					m_Pos += length;
					out->ValueKind = JsonValue::Kind::Bool;
					out->Boolean = value;
					return true;
				}
				if (ch == 'n')
				{
					if (m_Text.compare(m_Pos, 4, "null") != 0)
						return Fail("null 字面量");
					m_Pos += 4;
					out->ValueKind = JsonValue::Kind::Null;
					return true;
				}
				return ParseNumber(out);
			}

			bool ParseObject(JsonValue* out, int depth)
			{
				out->ValueKind = JsonValue::Kind::Object;
				++m_Pos;   // '{'
				SkipWhitespace();
				if (m_Pos < m_Text.size() && m_Text[m_Pos] == '}')
				{
					++m_Pos;
					return true;
				}
				while (true)
				{
					SkipWhitespace();
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
						return Fail("对象的键");
					std::string key;
					if (!ParseString(&key))
						return false;
					SkipWhitespace();
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != ':')
						return Fail("对象的冒号");
					++m_Pos;
					JsonValue value;
					if (!ParseValue(&value, depth + 1))
						return false;
					out->Members.emplace_back(std::move(key), std::move(value));
					SkipWhitespace();
					if (m_Pos < m_Text.size() && m_Text[m_Pos] == ',')
					{
						++m_Pos;
						continue;
					}
					if (m_Pos < m_Text.size() && m_Text[m_Pos] == '}')
					{
						++m_Pos;
						return true;
					}
					return Fail("对象的分隔符");
				}
			}

			bool ParseArray(JsonValue* out, int depth)
			{
				out->ValueKind = JsonValue::Kind::Array;
				++m_Pos;   // '['
				SkipWhitespace();
				if (m_Pos < m_Text.size() && m_Text[m_Pos] == ']')
				{
					++m_Pos;
					return true;
				}
				while (true)
				{
					JsonValue item;
					if (!ParseValue(&item, depth + 1))
						return false;
					out->Items.push_back(std::move(item));
					SkipWhitespace();
					if (m_Pos < m_Text.size() && m_Text[m_Pos] == ',')
					{
						++m_Pos;
						continue;
					}
					if (m_Pos < m_Text.size() && m_Text[m_Pos] == ']')
					{
						++m_Pos;
						return true;
					}
					return Fail("数组的分隔符");
				}
			}

			bool ParseString(std::string* out)
			{
				++m_Pos;   // '"'
				out->clear();
				while (m_Pos < m_Text.size())
				{
					const char ch = m_Text[m_Pos++];
					if (ch == '"')
						return true;
					if (ch != '\\')
					{
						*out += ch;
						continue;
					}
					if (m_Pos >= m_Text.size())
						return Fail("转义序列");
					const char escape = m_Text[m_Pos++];
					switch (escape)
					{
						case '"': *out += '"'; break;
						case '\\': *out += '\\'; break;
						case '/': *out += '/'; break;
						case 'b': *out += '\b'; break;
						case 'f': *out += '\f'; break;
						case 'n': *out += '\n'; break;
						case 'r': *out += '\r'; break;
						case 't': *out += '\t'; break;
						case 'u':
						{
							if (m_Pos + 4 > m_Text.size())
								return Fail("\\u 转义");
							uint32_t code = 0;
							for (int index = 0; index < 4; ++index)
							{
								const char digit = m_Text[m_Pos + static_cast<size_t>(index)];
								code <<= 4;
								if (digit >= '0' && digit <= '9') code |= static_cast<uint32_t>(digit - '0');
								else if (digit >= 'a' && digit <= 'f') code |= static_cast<uint32_t>(digit - 'a' + 10);
								else if (digit >= 'A' && digit <= 'F') code |= static_cast<uint32_t>(digit - 'A' + 10);
								else return Fail("\\u 十六进制");
							}
							m_Pos += 4;
							// 参数名是 ASCII 标识符;非 ASCII 只做最小 UTF-8 编码(够编辑器显示)。
							if (code < 0x80)
								*out += static_cast<char>(code);
							else if (code < 0x800)
							{
								*out += static_cast<char>(0xC0 | (code >> 6));
								*out += static_cast<char>(0x80 | (code & 0x3F));
							}
							else
							{
								*out += static_cast<char>(0xE0 | (code >> 12));
								*out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
								*out += static_cast<char>(0x80 | (code & 0x3F));
							}
							break;
						}
						default:
							return Fail("未知转义");
					}
				}
				return Fail("字符串没有结束引号");
			}

			bool ParseNumber(JsonValue* out)
			{
				const size_t begin = m_Pos;
				while (m_Pos < m_Text.size())
				{
					const char ch = m_Text[m_Pos];
					const bool part = (ch >= '0' && ch <= '9') || ch == '-' || ch == '+'
						|| ch == '.' || ch == 'e' || ch == 'E';
					if (!part)
						break;
					++m_Pos;
				}
				if (begin == m_Pos)
					return Fail("数字");
				const std::string text = m_Text.substr(begin, m_Pos - begin);
				char* end = nullptr;
				const double value = std::strtod(text.c_str(), &end);
				if (end == nullptr || *end != '\0')
					return Fail("数字格式");
				out->ValueKind = JsonValue::Kind::Number;
				out->Number = value;
				return true;
			}

			const std::string& m_Text;
			size_t m_Pos = 0;
			std::string m_Error;
		};

		uint32_t JsonBindingNumber(const JsonValue* binding, const char* key)
		{
			if (!binding || binding->ValueKind != JsonValue::Kind::Object)
				return 0;
			const JsonValue* value = binding->Find(key);
			if (!value || !value->IsNumber() || value->Number < 0.0)
				return 0;
			return static_cast<uint32_t>(value->Number);
		}

		// 反射类型的显示名(测试与编辑器文案都按这套串):
		//   float32→float、int32→int、bool/uint32→uint(HLSL bool 在 SPIR-V 参数块里是 uint32)、
		//   vector→v4float、贴图→type.2d.image。
		std::string ReflectedTypeFromJson(const JsonValue& type)
		{
			const JsonValue* kind = type.Find("kind");
			if (!kind || !kind->IsString())
				return "unknown";
			const std::string& kindText = kind->Text;
			if (kindText == "scalar")
			{
				const JsonValue* scalar = type.Find("scalarType");
				const std::string scalarText = scalar && scalar->IsString() ? scalar->Text : std::string();
				if (scalarText == "float32") return "float";
				if (scalarText == "int32") return "int";
				if (scalarText == "uint32") return "uint";
				if (scalarText == "bool") return "uint";
				if (scalarText == "float16") return "half";
				return scalarText.empty() ? std::string("scalar") : scalarText;
			}
			if (kindText == "vector")
			{
				const JsonValue* count = type.Find("elementCount");
				const uint32_t elements = count && count->IsNumber()
					? static_cast<uint32_t>(count->Number) : 0u;
				const JsonValue* element = type.Find("elementType");
				const std::string inner = element ? ReflectedTypeFromJson(*element) : std::string("float");
				return elements == 0 ? std::string("vector") : ("v" + std::to_string(elements) + inner);
			}
			if (kindText == "matrix")
				return "matrix";
			if (kindText == "resource")
			{
				const JsonValue* shape = type.Find("baseShape");
				const std::string shapeText = shape && shape->IsString() ? shape->Text : std::string();
				if (shapeText == "texture2D") return "type.2d.image";
				if (shapeText == "textureCube") return "type.cube.image";
				if (shapeText == "texture2DArray") return "type.2d.image.array";
				return shapeText.empty() ? std::string("resource") : shapeText;
			}
			if (kindText == "samplerState")
				return "sampler";
			return kindText;
		}

		// ---- SPIR-V 二进制里的"成员真的被读"(Slang-T3) ----
		//
		// 判据:OpAccessChain 以参数块变量为 base、**第一个**下标是常量 → 该成员被读。
		// 输入是 SPIR-V 二进制:引擎运行时不依赖 spirv-dis。
		struct SpirvUsage
		{
			std::unordered_map<uint32_t, uint32_t> DescriptorSet;
			std::unordered_map<uint32_t, uint32_t> Binding;
			std::unordered_set<uint32_t> UniformVariables;
			std::unordered_map<uint32_t, uint64_t> Constants;
			std::vector<std::pair<uint32_t, std::vector<uint32_t>>> AccessChains;
		};

		bool ScanSpirvUsage(const std::vector<uint8_t>& spirv, SpirvUsage* out, std::string* error)
		{
			if (!out)
			{
				if (error) *error = "SPIR-V 使用情况输出为空";
				return false;
			}
			if (spirv.size() < 20 || (spirv.size() % 4) != 0)
			{
				if (error) *error = "SPIR-V 二进制长度不合法";
				return false;
			}
			const uint32_t* words = reinterpret_cast<const uint32_t*>(spirv.data());
			if (words[0] != 0x07230203u)
			{
				if (error) *error = "SPIR-V 魔数不匹配";
				return false;
			}
			const size_t wordCount = spirv.size() / 4;
			size_t index = 5;   // 跳过 5 个字的头部
			while (index < wordCount)
			{
				const uint32_t instruction = words[index];
				const uint32_t opcode = instruction & 0xFFFFu;
				const uint32_t length = instruction >> 16;
				if (length == 0 || index + length > wordCount)
				{
					if (error) *error = "SPIR-V 指令长度越界";
					return false;
				}
				const uint32_t* operands = words + index + 1;
				switch (opcode)
				{
					case 71:   // OpDecorate: target, decoration, ...
						if (length >= 3 && operands[1] == 33)        // Binding
							out->Binding[operands[0]] = operands[2];
						else if (length >= 3 && operands[1] == 34)   // DescriptorSet
							out->DescriptorSet[operands[0]] = operands[2];
						break;
					case 59:   // OpVariable: resultType, resultId, storageClass
						if (length >= 4 && operands[2] == 2)         // StorageClass Uniform
							out->UniformVariables.insert(operands[1]);
						break;
					case 43:   // OpConstant: resultType, resultId, value...
						if (length >= 4)
							out->Constants[operands[1]] = static_cast<uint64_t>(operands[2]);
						break;
					case 65:   // OpAccessChain: resultType, resultId, base, indexes...
						if (length >= 5)
						{
							std::vector<uint32_t> indexes;
							indexes.reserve(length - 4);
							for (uint32_t operand = 3; operand < length; ++operand)
								indexes.push_back(operands[operand]);
							out->AccessChains.emplace_back(operands[2], std::move(indexes));
						}
						break;
					default:
						break;
				}
				index += length;
			}
			return true;
		}
		uint32_t RoundUp16(uint32_t value)
		{
			return ((value + kParamBlobAlignment - 1) / kParamBlobAlignment) * kParamBlobAlignment;
		}

		bool ReadTextFile(const fs::path& path, std::string& out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			out = buffer.str();
			return true;
		}
	}

	const char* ParamTypeName(ParamType type)
	{
		switch (type)
		{
			case ParamType::Float: return "Float";
			case ParamType::Vec2: return "Vec2";
			case ParamType::Vec3: return "Vec3";
			case ParamType::Vec4: return "Vec4";
			case ParamType::Color: return "Color";
			case ParamType::Int: return "Int";
			case ParamType::Bool: return "Bool";
			case ParamType::Texture2D: return "Texture2D";
		}
		return "Float";
	}

	bool ParseParamTypeName(const std::string& text, ParamType* out)
	{
		static const ParamType kTypes[] = {
			ParamType::Float, ParamType::Vec2, ParamType::Vec3, ParamType::Vec4,
			ParamType::Color, ParamType::Int, ParamType::Bool, ParamType::Texture2D,
		};
		for (const ParamType type : kTypes)
		{
			if (text == ParamTypeName(type))
			{
				if (out) *out = type;
				return true;
			}
		}
		return false;
	}

	bool IsTextureParamType(ParamType type)
	{
		return type == ParamType::Texture2D;
	}

	bool IsUsableParamName(const std::string& name)
	{
		return IsValidIdentifier(name) && !IsReservedParamName(name);
	}

	std::string FormatParamFloatText(float value)
	{
		return FormatFloat(value);
	}

	bool ParseParamFloat(const std::string& text, float* out)
	{
		double value = 0.0;
		if (!ParseNumber(text, &value))
			return false;
		if (out) *out = static_cast<float>(value);
		return true;
	}

	bool ParseParamInt(const std::string& text, int* out)
	{
		if (ParseIntText(text, out))
			return true;
		// 允许写成 "3.0"(YAML / 数值框都可能给这种文本),但必须是整数值。
		double value = 0.0;
		if (!ParseNumber(text, &value))
			return false;
		if (value != std::floor(value) || std::fabs(value) > 2147483647.0)
			return false;
		if (out) *out = static_cast<int>(value);
		return true;
	}

	bool ParseParamBool(const std::string& text, bool* out)
	{
		return ParseBoolText(text, out);
	}

	bool ParseParamFloatComponents(const std::string& text, ParamType type, float* out, int count)
	{
		if (!out || count <= 0 || count > 4)
			return false;
		const std::vector<std::string> parts = SplitCommaList(text);
		const bool shortColor = type == ParamType::Color && parts.size() == 3 && count == 4;
		if (static_cast<int>(parts.size()) != count && !shortColor)
			return false;
		for (int index = 0; index < count; ++index)
		{
			if (index < static_cast<int>(parts.size()))
			{
				if (!ParseParamFloat(parts[index], &out[index]))
					return false;
			}
			else
			{
				out[index] = 1.0f;   // Color 写 3 个分量 → alpha = 1
			}
		}
		return true;
	}

	bool NormalizeParamValue(ParamType type, const std::string& text, std::string* normalized, std::string* error)
	{
		const std::string trimmed = Trim(text);
		if (error) error->clear();
		if (type == ParamType::Texture2D)
		{
			std::string path = trimmed;
			if (!path.empty() && path.front() == '"')
			{
				if (path.size() < 2 || path.back() != '"')
				{
					if (error) *error = "贴图路径的引号没有闭合";
					return false;
				}
				path = path.substr(1, path.size() - 2);
			}
			for (const char ch : path)
			{
				if (ch == '\n' || ch == '\r' || ch == '\t')
				{
					if (error) *error = "贴图路径不能包含换行/制表符";
					return false;
				}
			}
			if (normalized) *normalized = path;
			return true;
		}
		if (type == ParamType::Bool)
		{
			bool value = false;
			if (!ParseBoolText(trimmed, &value))
			{
				if (error) *error = "'" + trimmed + "' 不是 Bool(true/false)";
				return false;
			}
			if (normalized) *normalized = value ? "true" : "false";
			return true;
		}
		if (type == ParamType::Int)
		{
			int value = 0;
			if (!ParseParamInt(trimmed, &value))
			{
				if (error) *error = "'" + trimmed + "' 不是整数";
				return false;
			}
			if (normalized) *normalized = std::to_string(value);
			return true;
		}
		if (type == ParamType::Float)
		{
			float value = 0.0f;
			if (!ParseParamFloat(trimmed, &value))
			{
				if (error) *error = "'" + trimmed + "' 不是数字";
				return false;
			}
			if (normalized) *normalized = FormatFloat(value);
			return true;
		}

		const int count = ComponentCount(type);
		float components[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		if (!ParseParamFloatComponents(trimmed, type, components, count))
		{
			if (error)
			{
				*error = "'" + trimmed + "' 不是 " + std::to_string(count) + " 个数字"
					+ (type == ParamType::Color ? "(Color 允许 3 或 4 个)" : "");
			}
			return false;
		}
		std::string result;
		for (int index = 0; index < count; ++index)
		{
			if (index != 0)
				result.append(", ");
			result.append(FormatFloat(components[index]));
		}
		if (normalized) *normalized = result;
		return true;
	}

	bool IsParamValueCompatible(ParamType type, const std::string& text)
	{
		return NormalizeParamValue(type, text, nullptr, nullptr);
	}

	bool IsReflectedTypeCompatible(ParamType type, const std::string& reflectedType)
	{
		return ReflectedTypeMatches(type, reflectedType);
	}

	std::string FormatMaterialParamAnnotation(const MaterialParamDecl& decl)
	{
		std::string text = "param ";
		text += ParamTypeName(decl.Type);
		text += " ";
		text += decl.Name;
		text += " = ";
		std::string normalized;
		std::string reason;
		if (NormalizeParamValue(decl.Type, decl.Default, &normalized, &reason))
		{
			if (decl.Type == ParamType::Texture2D)
			{
				text += QuoteAnnotationText(normalized);
			}
			else
			{
				text += normalized;
			}
		}
		else
		{
			// 值本身不合法(编辑器里的半成品):照原样引号包住,读回来会报同一条错误。
			text += QuoteAnnotationText(decl.Default);
		}
		if (decl.Type == ParamType::Float || decl.Type == ParamType::Int)
		{
			if (decl.Min != 0.0f || decl.Max != 1.0f)
			{
				text += " [";
				text += FormatFloat(decl.Min);
				text += ",";
				text += FormatFloat(decl.Max);
				text += "]";
			}
		}
		if (!decl.Unit.empty())
			text += " unit(" + QuoteAnnotationText(decl.Unit) + ")";
		if (!decl.Group.empty())
			text += " group(" + QuoteAnnotationText(decl.Group) + ")";
		if (!decl.Label.empty())
			text += " label(" + QuoteAnnotationText(decl.Label) + ")";
		// MAT-UI7a:参数说明写在最后 —— 既有注解(没有 doc)的写出字节完全不变。
		const std::string doc = TruncateDocText(decl.Doc);
		if (!doc.empty())
			text += " doc(" + QuoteAnnotationText(doc) + ")";
		return text;
	}

	const char* ParamCbufferName()
	{
		return "MaterialParams";
	}

	uint32_t ParamCbufferSet()
	{
		return 1;
	}

	uint32_t ParamCbufferBinding()
	{
		// M4-S3(D1):2 被 set0 的灯光 UBO 占用(GL 的 UBO 单元 = binding,忽略 set)。
		return 4;
	}

	uint32_t ParamTextureBaseBinding()
	{
		// 与引擎现有着色器同 space(set 2):albedo = t1、normal = t2;参数贴图从 t4 起。
		return 4;
	}

	bool ParseMaterialParams(const std::string& hlslSource, std::vector<MaterialParamDecl>* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "输出表为空";
			return false;
		}
		out->clear();
		if (error) error->clear();

		std::vector<MaterialParamDecl> decls;
		std::unordered_map<std::string, uint32_t> firstLine;
		std::istringstream stream(hlslSource);
		std::string line;
		uint32_t lineNumber = 0;
		while (std::getline(stream, line))
		{
			++lineNumber;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			size_t index = 0;
			while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
				++index;
			if (line.compare(index, 3, "//!") != 0)
				continue;

			// MAT-UI7a:行尾注释(引号外的 `//`)先切掉再解析,列号仍按原行计算(只截尾,不动前缀);
			// 字符串里的 `//` 与没有空白前缀的双斜杠不受影响(见 FindAnnotationCommentStart)。
			std::string annotationText = line;
			const size_t commentStart = FindAnnotationCommentStart(annotationText, index + 3);
			if (commentStart != std::string::npos)
				annotationText.erase(commentStart);

			AnnotationLine annotation(annotationText, lineNumber);
			if (!annotation.Parse(index + 3))
			{
				if (error) *error = annotation.Error();
				return false;
			}
			const MaterialParamDecl& decl = annotation.Decl();
			const auto previous = firstLine.find(decl.Name);
			if (previous != firstLine.end())
			{
				if (error)
				{
					*error = MakeError(lineNumber, annotation.NameColumn(),
						"重复的参数名 '" + decl.Name + "'(第 " + std::to_string(previous->second)
							+ " 行已经声明)");
				}
				return false;
			}
			firstLine.emplace(decl.Name, lineNumber);
			decls.push_back(decl);
		}

		*out = std::move(decls);
		return true;
	}

	bool ReflectParamLayoutFromReflectionJson(const std::string& reflectionJson,
		const std::vector<uint8_t>& spirv, MaterialParamLayout* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "输出布局为空";
			return false;
		}
		*out = MaterialParamLayout {};
		out->CbufferSet = ParamCbufferSet();
		out->CbufferBinding = ParamCbufferBinding();

		JsonValue root;
		if (!JsonReader(reflectionJson).Parse(&root, error))
			return false;
		const JsonValue* parameters = root.Find("parameters");
		if (!parameters || parameters->ValueKind != JsonValue::Kind::Array)
		{
			if (error) *error = "反射 JSON 里没有 parameters[](不是 Slang 的 -reflection-json?)";
			return false;
		}

		// 参数块:优先按 (set,binding);找不到再按名字 MaterialParams(用户手写参数块的情况)。
		const JsonValue* block = nullptr;
		for (const JsonValue& parameter : parameters->Items)
		{
			const JsonValue* type = parameter.Find("type");
			const JsonValue* kind = type ? type->Find("kind") : nullptr;
			if (!kind || !kind->IsString() || kind->Text != "constantBuffer")
				continue;
			const JsonValue* binding = parameter.Find("binding");
			if (JsonBindingNumber(binding, "space") == ParamCbufferSet()
				&& JsonBindingNumber(binding, "index") == ParamCbufferBinding())
			{
				block = &parameter;
				break;
			}
		}
		if (!block)
		{
			for (const JsonValue& parameter : parameters->Items)
			{
				const JsonValue* name = parameter.Find("name");
				if (name && name->IsString() && name->Text == ParamCbufferName())
				{
					block = &parameter;
					break;
				}
			}
		}
		if (block)
		{
			const JsonValue* type = block->Find("type");
			const JsonValue* elementType = type ? type->Find("elementType") : nullptr;
			const JsonValue* fields = elementType ? elementType->Find("fields") : nullptr;
			if (!fields || fields->ValueKind != JsonValue::Kind::Array)
			{
				if (error)
					*error = "参数块 " + std::string(ParamCbufferName()) + " 在反射 JSON 里没有成员表";
				return false;
			}

			uint32_t end = 0;
			for (const JsonValue& field : fields->Items)
			{
				MaterialParamLayoutField layoutField;
				const JsonValue* name = field.Find("name");
				layoutField.Name = name && name->IsString() ? name->Text : std::string("member");
				const JsonValue* fieldType = field.Find("type");
				layoutField.ReflectedType = fieldType
					? ReflectedTypeFromJson(*fieldType) : std::string("unknown");
				layoutField.Type = DeriveTypeFromReflected(layoutField.ReflectedType);
				const JsonValue* binding = field.Find("binding");
				layoutField.Offset = JsonBindingNumber(binding, "offset");
				layoutField.Size = JsonBindingNumber(binding, "size");
				end = std::max(end, layoutField.Offset + layoutField.Size);
				out->Fields.push_back(std::move(layoutField));
			}

			// 块大小:优先用 Slang 报的 uniform size(已含尾部对齐);缺失时按成员末端 16 字节对齐。
			uint32_t blockSize = 0;
			const JsonValue* sizes = elementType->Find("sizes");
			if (sizes && sizes->ValueKind == JsonValue::Kind::Array)
			{
				for (const JsonValue& size : sizes->Items)
				{
					const JsonValue* kind = size.Find("kind");
					const JsonValue* value = size.Find("value");
					if (kind && kind->IsString() && kind->Text == "uniform"
						&& value && value->IsNumber())
					{
						blockSize = static_cast<uint32_t>(value->Number);
					}
				}
			}
			out->CbufferSize = blockSize != 0 ? blockSize : RoundUp16(end);

			// "真的被读":SPIR-V 里的 OpAccessChain 首下标。SPIR-V 缺失(纯 JSON 单测)时
			// 退化为"没有使用信息"(空 UsedMembers),不猜。
			SpirvUsage usage;
			std::string usageError;
			if (!spirv.empty() && ScanSpirvUsage(spirv, &usage, &usageError))
			{
				uint32_t blockVariable = 0;
				for (const uint32_t variable : usage.UniformVariables)
				{
					const auto set = usage.DescriptorSet.find(variable);
					const auto binding = usage.Binding.find(variable);
					if (set == usage.DescriptorSet.end() || binding == usage.Binding.end())
						continue;
					if (set->second == ParamCbufferSet() && binding->second == ParamCbufferBinding())
					{
						blockVariable = variable;
						break;
					}
				}
				if (blockVariable != 0)
				{
					bool dynamicIndex = false;
					std::unordered_set<uint32_t> usedIndices;
					for (const auto& chain : usage.AccessChains)
					{
						if (chain.first != blockVariable || chain.second.empty())
							continue;
						const auto constant = usage.Constants.find(chain.second.front());
						if (constant == usage.Constants.end())
						{
							dynamicIndex = true;
							continue;
						}
						usedIndices.insert(static_cast<uint32_t>(constant->second));
					}
					for (size_t index = 0; index < out->Fields.size(); ++index)
					{
						if (dynamicIndex || usedIndices.count(static_cast<uint32_t>(index)) != 0)
							out->UsedMembers.push_back(out->Fields[index].Name);
					}
					std::sort(out->UsedMembers.begin(), out->UsedMembers.end());
				}
			}
		}

		// 参数贴图槽:space = 2、binding >= 4 的 resource(引擎 albedo = t1 / normal = t2 不算)。
		for (const JsonValue& parameter : parameters->Items)
		{
			const JsonValue* type = parameter.Find("type");
			const JsonValue* kind = type ? type->Find("kind") : nullptr;
			if (!kind || !kind->IsString() || kind->Text != "resource")
				continue;
			const JsonValue* binding = parameter.Find("binding");
			const uint32_t space = JsonBindingNumber(binding, "space");
			const uint32_t index = JsonBindingNumber(binding, "index");
			if (space != 2 || index < ParamTextureBaseBinding())
				continue;
			MaterialParamTextureSlot slot;
			const JsonValue* name = parameter.Find("name");
			slot.Name = name && name->IsString() ? name->Text : std::string();
			slot.ReflectedType = type ? ReflectedTypeFromJson(*type) : std::string("type.2d.image");
			slot.Set = space;
			slot.Binding = index;
			out->Textures.push_back(std::move(slot));
		}
		std::sort(out->Textures.begin(), out->Textures.end(),
			[](const MaterialParamTextureSlot& a, const MaterialParamTextureSlot& b)
			{
				if (a.Binding != b.Binding) return a.Binding < b.Binding;
				return a.Name < b.Name;
			});
		return true;
	}

	// 端到端:按 table 编译包装源码(slangc)并反射出布局。工具缺失/源码错误 → false + error。
	bool BuildParamLayout(const std::string& hlslSource, const std::vector<MaterialParamDecl>& table,
		MaterialParamLayout* out, std::string* error,
		const std::vector<std::filesystem::path>& includeRoots)
	{
		if (!out)
		{
			if (error) *error = "输出布局为空";
			return false;
		}
		const SurfaceCompileResult compiled = MaterialSurfaceCompiler::CompileSurfaceWithParams(
			hlslSource, table, kParamPermutationKey, SurfaceShaderBackend::VulkanSpirV, includeRoots);
		if (!compiled.Success)
		{
			std::vector<std::string> messages;
			for (const SurfaceDiagnostic& diagnostic : compiled.Diagnostics)
			{
				if (diagnostic.Severity == "error")
					messages.push_back(diagnostic.Message);
			}
			if (messages.empty())
				messages.push_back("Slang 编译失败但没有诊断输出");
			if (error) *error = "参数表编译失败: " + Join(messages);
			return false;
		}
		const std::string reflectionPath = MaterialSurfaceCompiler::ReflectionPath(compiled.Artifact);
		if (reflectionPath.empty())
		{
			if (error)
				*error = "参数表编译产物缺少 Slang 反射 JSON(-reflection-json 输出),无法反射;"
					"请重新编译该着色器";
			return false;
		}
		std::string reflectionJson;
		if (!ReadTextFile(fs::path(reflectionPath), reflectionJson))
		{
			if (error) *error = "读不到反射用的 Slang JSON: " + reflectionPath;
			return false;
		}
		MaterialParamLayout layout;
		if (!ReflectParamLayoutFromReflectionJson(reflectionJson, compiled.Artifact.Bytecode,
			&layout, error))
			return false;

		// 注解类型是事实源:反射只能给出 v4float(Vec4 与 Color 同形)、uint(HLSL bool 的形态),
		// 所以在基类型相容时用注解里的类型覆盖推导结果。
		for (MaterialParamLayoutField& field : layout.Fields)
		{
			const MaterialParamDecl* decl = FindParamDecl(table, field.Name);
			if (decl && ReflectedTypeMatches(decl->Type, field.ReflectedType))
				field.Type = decl->Type;
		}
		*out = std::move(layout);
		return true;
	}

	bool ValidateParamsWithReflection(const std::string& hlslSource, const std::vector<MaterialParamDecl>& table,
		std::vector<std::string>* warnings, std::string* error)
	{
		if (warnings) warnings->clear();
		if (error) error->clear();

		// 表自身的基本合法性(调用方可能拿了编辑器里的手工表,不是 ParseMaterialParams 的结果)。
		std::unordered_set<std::string> names;
		for (const MaterialParamDecl& decl : table)
		{
			if (!IsUsableParamName(decl.Name))
			{
				if (error)
				{
					*error = IsValidIdentifier(decl.Name)
						? "参数名 '" + decl.Name + "' 与引擎模板/uniform 保留名冲突"
						: "参数名 '" + decl.Name + "' 不是合法的 HLSL 标识符";
				}
				return false;
			}
			if (!names.insert(decl.Name).second)
			{
				if (error) *error = "重复的参数名 '" + decl.Name + "'";
				return false;
			}
			std::string normalized;
			std::string reason;
			if (!IsTextureParamType(decl.Type))
			{
				if (!NormalizeParamValue(decl.Type, decl.Default, &normalized, &reason))
				{
					if (error)
						*error = "参数 '" + decl.Name + "' 的默认值 '" + decl.Default + "' 不合法:" + reason;
					return false;
				}
			}
		}

		MaterialParamLayout layout;
		if (!BuildParamLayout(hlslSource, table, &layout, error))
			return false;

		std::unordered_set<std::string> used(layout.UsedMembers.begin(), layout.UsedMembers.end());
		std::unordered_set<std::string> reflectedFields;
		for (const MaterialParamLayoutField& field : layout.Fields)
		{
			reflectedFields.insert(field.Name);
			const MaterialParamDecl* decl = FindParamDecl(table, field.Name);
			if (!decl)
			{
				if (error)
				{
					*error = "参数 '" + field.Name + "' 在参数块 " + ParamCbufferName()
						+ " 里被声明/使用,但注解里没有声明(请补 '//! param <类型> "
						+ field.Name + " = <默认值>' 或删掉该成员)";
				}
				return false;
			}
			if (!ReflectedTypeMatches(decl->Type, field.ReflectedType))
			{
				if (error)
				{
					*error = "参数 '" + field.Name + "' 注解声明为 " + ParamTypeName(decl->Type)
						+ ",反射到的是 " + field.ReflectedType;
				}
				return false;
			}
			if (used.count(field.Name) == 0)
			{
				if (warnings)
				{
					warnings->push_back("参数 '" + field.Name
						+ "' 声明了但着色器没有读它(声明未用;编辑器会显示该参数,运行期不参与着色)");
				}
			}
		}

		// 贴图:注解顺序 = 绑定顺序(t4、t5…);被采样的贴图会出现在反射里,没被采样 → 声明未用。
		uint32_t textureIndex = 0;
		for (const MaterialParamDecl& decl : table)
		{
			if (!IsTextureParamType(decl.Type))
				continue;
			// M4-S3:贴图槽位是**固定**的一段(t4..t11),超出上限必须结构化失败 ——
			// 静默继续会让运行时的描述符槽位与声明对不上。
			if (textureIndex >= kMaxMaterialTextureSlots)
			{
				if (error)
				{
					*error = "贴图参数 '" + decl.Name + "' 超出上限:参数块最多 "
						+ std::to_string(kMaxMaterialTextureSlots) + " 张贴图(t"
						+ std::to_string(ParamTextureBaseBinding()) + "..t"
						+ std::to_string(ParamTextureBaseBinding() + kMaxMaterialTextureSlots - 1) + ")";
				}
				return false;
			}
			const uint32_t expectedBinding = ParamTextureBaseBinding() + textureIndex;
			++textureIndex;
			const MaterialParamTextureSlot* slot = nullptr;
			for (const MaterialParamTextureSlot& candidate : layout.Textures)
				if (candidate.Name == decl.Name)
					slot = &candidate;
			if (!slot)
			{
				if (warnings)
				{
					warnings->push_back("贴图参数 '" + decl.Name
						+ "' 声明了但着色器没有采样它(声明未用)");
				}
				continue;
			}
			if (slot->Binding != expectedBinding)
			{
				if (error)
				{
					*error = "贴图参数 '" + decl.Name + "' 的绑定是 set " + std::to_string(slot->Binding)
						+ ", 期望 set " + std::to_string(expectedBinding)
						+ "(参数块按注解顺序分配 space2 的 t4、t5…)";
				}
				return false;
			}
		}
		for (const MaterialParamTextureSlot& slot : layout.Textures)
		{
			const MaterialParamDecl* decl = FindParamDecl(table, slot.Name);
			if (!decl || !IsTextureParamType(decl->Type))
			{
				if (error)
				{
					*error = "贴图 '" + slot.Name + "' 绑在 set " + std::to_string(slot.Set)
						+ " binding " + std::to_string(slot.Binding)
						+ ",但注解里没有声明为 Texture2D";
				}
				return false;
			}
		}
		return true;
	}

	std::string FormatParamLayout(const MaterialParamLayout& layout)
	{
		std::ostringstream out;
		out << ParamCbufferName() << " (set " << layout.CbufferSet << ", binding " << layout.CbufferBinding
			<< ") size " << layout.CbufferSize << " bytes\n";
		for (const MaterialParamLayoutField& field : layout.Fields)
		{
			out << "  " << field.Name << "  " << ParamTypeName(field.Type) << "  " << field.ReflectedType
				<< "  offset " << field.Offset << "  size " << field.Size << '\n';
		}
		for (const MaterialParamTextureSlot& slot : layout.Textures)
		{
			out << "  " << slot.Name << "  Texture2D  " << slot.ReflectedType
				<< "  set " << slot.Set << "  binding " << slot.Binding << '\n';
		}
		if (!layout.UsedMembers.empty())
		{
			out << "  used:";
			for (const std::string& name : layout.UsedMembers)
				out << ' ' << name;
			out << '\n';
		}
		return out.str();
	}

	const MaterialParamDecl* FindParamDecl(const std::vector<MaterialParamDecl>& table, const std::string& name)
	{
		for (const MaterialParamDecl& decl : table)
			if (decl.Name == name)
				return &decl;
		return nullptr;
	}

	std::vector<std::string> BuildParamWarnings(const std::vector<MaterialParamDecl>& table,
		const std::vector<MaterialParamOverride>& overrides, const std::string& shaderPath)
	{
		std::vector<std::string> warnings;
		const std::string shaderName = shaderPath.empty() ? std::string("<未指定 shader>") : shaderPath;
		for (const MaterialParamOverride& entry : overrides)
		{
			const MaterialParamDecl* decl = FindParamDecl(table, entry.Name);
			if (!decl)
			{
				warnings.push_back("参数 '" + entry.Name + "' 在 shader '" + shaderName
					+ "' 里没有声明(值保留在文件里,运行期忽略)");
				continue;
			}
			std::string normalized;
			std::string reason;
			if (!NormalizeParamValue(decl->Type, entry.Value, &normalized, &reason))
			{
				warnings.push_back("参数 '" + entry.Name + "' 的值 '" + entry.Value + "' 与声明的 "
					+ ParamTypeName(decl->Type) + " 不符(" + reason + ");运行期用 shader 默认 '"
					+ decl->Default + "'");
			}
		}
		return warnings;
	}

	bool ParamOverridesEquivalent(const std::vector<MaterialParamOverride>& actual,
		const std::vector<MaterialParamOverride>& expected,
		const std::vector<MaterialParamDecl>& table)
	{
		if (actual.size() != expected.size())
			return false;
		for (size_t index = 0; index < actual.size(); ++index)
		{
			if (actual[index].Name != expected[index].Name)
				return false;
			const MaterialParamDecl* decl = FindParamDecl(table, actual[index].Name);
			if (!decl)
				return actual[index].Value == expected[index].Value;
			std::string actualValue;
			std::string expectedValue;
			std::string reason;
			if (!NormalizeParamValue(decl->Type, actual[index].Value, &actualValue, &reason)
				|| !NormalizeParamValue(decl->Type, expected[index].Value, &expectedValue, &reason))
			{
				return actual[index].Value == expected[index].Value;
			}
			if (actualValue != expectedValue)
				return false;
		}
		return true;
	}

	bool PackParamValues(const MaterialParamLayout& layout, const std::vector<MaterialParamDecl>& table,
		const std::vector<MaterialParamOverride>& overrides, std::vector<uint8_t>* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "输出缓冲为空";
			return false;
		}
		if (error) error->clear();
		out->assign(layout.CbufferSize, 0);

		for (const MaterialParamLayoutField& field : layout.Fields)
		{
			const MaterialParamDecl* decl = FindParamDecl(table, field.Name);
			if (!decl)
			{
				if (error) *error = "参数块里的 '" + field.Name + "' 没有注解声明,无法打包";
				return false;
			}
			const MaterialParamOverride* override = nullptr;
			for (const MaterialParamOverride& candidate : overrides)
			{
				if (candidate.Name == field.Name)
				{
					override = &candidate;
					break;
				}
			}
			const std::string& text = override ? override->Value : decl->Default;
			std::string normalized;
			std::string reason;
			if (!NormalizeParamValue(decl->Type, text, &normalized, &reason))
			{
				if (error)
				{
					*error = "参数 '" + field.Name + "' 的值 '" + text + "' 与 " + ParamTypeName(decl->Type)
						+ " 不符: " + reason;
				}
				return false;
			}
			if (field.Offset + field.Size > out->size())
			{
				if (error)
				{
					*error = "参数 '" + field.Name + "' 的偏移 " + std::to_string(field.Offset)
						+ " + 大小 " + std::to_string(field.Size) + " 超出参数块 "
						+ std::to_string(out->size()) + " 字节";
				}
				return false;
			}
			uint8_t* target = out->data() + field.Offset;
			if (decl->Type == ParamType::Float)
			{
				const float value = std::strtof(normalized.c_str(), nullptr);
				std::memcpy(target, &value, sizeof(value));
			}
			else if (decl->Type == ParamType::Int)
			{
				const int value = static_cast<int>(std::strtol(normalized.c_str(), nullptr, 10));
				std::memcpy(target, &value, sizeof(value));
			}
			else if (decl->Type == ParamType::Bool)
			{
				const uint32_t value = normalized == "true" ? 1u : 0u;
				std::memcpy(target, &value, sizeof(value));
			}
			else
			{
				float components[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				const int count = ComponentCount(decl->Type);
				if (!ParseParamFloatComponents(normalized, decl->Type, components, count))
				{
					if (error) *error = "参数 '" + field.Name + "' 的向量分量解析失败";
					return false;
				}
				for (int index = 0; index < count; ++index)
					std::memcpy(target + index * kScalarSize, &components[index], sizeof(float));
			}
		}
		return true;
	}
}
