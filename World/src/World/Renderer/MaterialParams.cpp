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
		// (其余冲突仍由 dxc 的原始诊断兜底)。
		const char* const kReservedParamNames[] = {
			"Surface", "MaterialInputs", "SurfaceVSInput", "SurfaceVSOutput",
			"SurfaceInstanceInput", "SurfaceSkinnedInput", "SurfacePSOutput", "GpuLight",
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
					else m_Decl.Label = value;
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

		// ---- SPIR-V 汇编解析(dxc -Fc) ----

		struct AsmModule
		{
			std::unordered_map<std::string, std::string> Names;
			std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> MemberNames;
			std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> Decorations;
			std::unordered_map<std::string, std::unordered_map<uint32_t, uint32_t>> MemberOffsets;
			std::unordered_map<std::string, std::string> ScalarKind;
			std::unordered_map<std::string, std::pair<std::string, int>> Vectors;
			std::unordered_map<std::string, std::vector<std::string>> StructMembers;
			std::unordered_map<std::string, std::pair<std::string, std::string>> Pointers;
			std::unordered_map<std::string, std::string> Images;
			std::unordered_map<std::string, std::string> SampledImages;
			std::unordered_map<std::string, std::pair<std::string, std::string>> Variables;
			std::unordered_map<std::string, int64_t> Constants;
			std::vector<std::pair<std::string, std::vector<std::string>>> AccessChains;
			size_t InstructionCount = 0;
		};

		std::vector<std::string> SplitTokens(const std::string& line)
		{
			std::vector<std::string> tokens;
			std::istringstream stream(line);
			std::string token;
			while (stream >> token)
				tokens.push_back(token);
			return tokens;
		}

		std::string QuotedName(const std::string& line)
		{
			const size_t begin = line.find('"');
			if (begin == std::string::npos)
				return {};
			const size_t end = line.rfind('"');
			if (end <= begin)
				return {};
			return line.substr(begin + 1, end - begin - 1);
		}

		bool ParseInt64(const std::string& text, int64_t* out)
		{
			if (text.empty())
				return false;
			char* end = nullptr;
			const long long value = std::strtoll(text.c_str(), &end, 10);
			if (end == nullptr || *end != '\0')
				return false;
			*out = static_cast<int64_t>(value);
			return true;
		}

		bool ParseAsmModule(const std::string& assembly, AsmModule* out, std::string* error)
		{
			std::istringstream stream(assembly);
			std::string line;
			while (std::getline(stream, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				const std::string trimmed = Trim(line);
				if (trimmed.empty() || trimmed.front() == ';')
					continue;
				const std::vector<std::string> tokens = SplitTokens(trimmed);
				if (tokens.size() < 2)
					continue;
				++out->InstructionCount;
				const bool assigned = tokens[1] == "=";
				const std::string& opcode = assigned ? tokens[2] : tokens[0];
				const size_t operandBase = assigned ? 3 : 1;
				if (assigned)
				{
					const std::string& id = tokens[0];
					if (opcode == "OpTypeFloat")
					{
						out->ScalarKind[id] = "float";
					}
					else if (opcode == "OpTypeInt")
					{
						const std::string signedness = tokens.size() > operandBase + 1
							? tokens[operandBase + 1] : std::string("1");
						out->ScalarKind[id] = signedness == "0" ? "uint" : "int";
					}
					else if (opcode == "OpTypeVector")
					{
						const int count = tokens.size() > operandBase + 1
							? std::atoi(tokens[operandBase + 1].c_str()) : 0;
						out->Vectors[id] = { tokens[operandBase], count };
					}
					else if (opcode == "OpTypeStruct")
					{
						std::vector<std::string> members;
						for (size_t index = operandBase; index < tokens.size(); ++index)
							members.push_back(tokens[index]);
						out->StructMembers[id] = std::move(members);
					}
					else if (opcode == "OpTypePointer")
					{
						if (tokens.size() > operandBase + 1)
							out->Pointers[id] = { tokens[operandBase], tokens[operandBase + 1] };
					}
					else if (opcode == "OpTypeImage")
					{
						if (tokens.size() > operandBase + 1)
							out->Images[id] = tokens[operandBase + 1];
					}
					else if (opcode == "OpTypeSampledImage")
					{
						if (tokens.size() > operandBase)
							out->SampledImages[id] = tokens[operandBase];
					}
					else if (opcode == "OpVariable")
					{
						if (tokens.size() > operandBase + 1)
							out->Variables[id] = { tokens[operandBase], tokens[operandBase + 1] };
					}
					else if (opcode == "OpConstant")
					{
						// `%int_0 = OpConstant %int 0`:operandBase 指向**类型 id**,值在下一个。
						int64_t value = 0;
						if (tokens.size() > operandBase + 1
							&& ParseInt64(tokens[operandBase + 1], &value))
							out->Constants[id] = value;
					}
					else if (opcode == "OpAccessChain" || opcode == "OpInBoundsAccessChain")
					{
						if (tokens.size() > operandBase + 1)
						{
							std::vector<std::string> indices;
							for (size_t index = operandBase + 2; index < tokens.size(); ++index)
								indices.push_back(tokens[index]);
							out->AccessChains.emplace_back(tokens[operandBase + 1], std::move(indices));
						}
					}
					continue;
				}

				if (opcode == "OpName" && tokens.size() >= 3)
				{
					out->Names[tokens[1]] = QuotedName(trimmed);
				}
				else if (opcode == "OpMemberName" && tokens.size() >= 4)
				{
					int64_t index = 0;
					if (ParseInt64(tokens[2], &index))
						out->MemberNames[tokens[1]][static_cast<uint32_t>(index)] = QuotedName(trimmed);
				}
				else if (opcode == "OpDecorate" && tokens.size() >= 4)
				{
					int64_t value = 0;
					if (ParseInt64(tokens[3], &value))
						out->Decorations[tokens[1]][tokens[2]] = value;
				}
				else if (opcode == "OpMemberDecorate" && tokens.size() >= 5 && tokens[3] == "Offset")
				{
					int64_t index = 0;
					int64_t value = 0;
					if (ParseInt64(tokens[2], &index) && ParseInt64(tokens[4], &value))
					{
						out->MemberOffsets[tokens[1]][static_cast<uint32_t>(index)]
							= static_cast<uint32_t>(value);
					}
				}
			}
			if (out->InstructionCount == 0)
			{
				if (error) *error = "SPIR-V 汇编为空或格式不认识";
				return false;
			}
			return true;
		}

		std::string TypeDisplayName(const AsmModule& module, const std::string& typeId, int depth = 0)
		{
			const auto named = module.Names.find(typeId);
			if (named != module.Names.end() && !named->second.empty())
				return named->second;
			const auto scalar = module.ScalarKind.find(typeId);
			if (scalar != module.ScalarKind.end())
				return scalar->second;
			const auto vector = module.Vectors.find(typeId);
			if (vector != module.Vectors.end())
			{
				if (depth < 4)
					return "v" + std::to_string(vector->second.second)
						+ TypeDisplayName(module, vector->second.first, depth + 1);
				return "vector";
			}
			if (module.StructMembers.count(typeId) != 0)
				return "struct";
			if (module.Images.count(typeId) != 0)
				return "image";
			if (module.SampledImages.count(typeId) != 0)
				return "sampledimage";
			if (module.Pointers.count(typeId) != 0)
				return "pointer";
			return "unknown";
		}

		uint32_t TypeSize(const AsmModule& module, const std::string& typeId, int depth = 0)
		{
			if (depth > 8)
				return 0;
			if (module.ScalarKind.count(typeId) != 0)
				return kScalarSize;
			const auto vector = module.Vectors.find(typeId);
			if (vector != module.Vectors.end())
				return static_cast<uint32_t>(vector->second.second) * kScalarSize;
			const auto structure = module.StructMembers.find(typeId);
			if (structure != module.StructMembers.end())
			{
				uint32_t end = 0;
				const auto offsets = module.MemberOffsets.find(typeId);
				for (size_t index = 0; index < structure->second.size(); ++index)
				{
					const uint32_t offset = offsets != module.MemberOffsets.end()
						&& offsets->second.count(static_cast<uint32_t>(index)) != 0
						? offsets->second.at(static_cast<uint32_t>(index)) : end;
					const uint32_t size = TypeSize(module, structure->second[index], depth + 1);
					end = std::max(end, offset + size);
				}
				return end;
			}
			return 0;
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
				text += "\"";
				text += normalized;
				text += "\"";
			}
			else
			{
				text += normalized;
			}
		}
		else
		{
			// 值本身不合法(编辑器里的半成品):照原样引号包住,读回来会报同一条错误。
			text += "\"" + decl.Default + "\"";
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
			text += " unit(\"" + decl.Unit + "\")";
		if (!decl.Group.empty())
			text += " group(\"" + decl.Group + "\")";
		if (!decl.Label.empty())
			text += " label(\"" + decl.Label + "\")";
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
		return 2;
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

			AnnotationLine annotation(line, lineNumber);
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

	bool ReflectParamLayoutFromAssembly(const std::string& assembly, MaterialParamLayout* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "输出布局为空";
			return false;
		}
		*out = MaterialParamLayout {};
		out->CbufferSet = ParamCbufferSet();
		out->CbufferBinding = ParamCbufferBinding();

		AsmModule module;
		if (!ParseAsmModule(assembly, &module, error))
			return false;

		// 参数块变量:优先按 (set,binding);找不到再按名字 MaterialParams(用户手写参数块的情况)。
		std::string blockId;
		for (const auto& entry : module.Variables)
		{
			const std::string& id = entry.first;
			if (entry.second.second != "Uniform")
				continue;
			const auto decorations = module.Decorations.find(id);
			if (decorations == module.Decorations.end())
				continue;
			const auto set = decorations->second.find("DescriptorSet");
			const auto binding = decorations->second.find("Binding");
			if (set != decorations->second.end() && binding != decorations->second.end()
				&& set->second == static_cast<int64_t>(ParamCbufferSet())
				&& binding->second == static_cast<int64_t>(ParamCbufferBinding()))
			{
				blockId = id;
				break;
			}
		}
		if (blockId.empty())
		{
			for (const auto& entry : module.Names)
			{
				if (entry.second == ParamCbufferName() && module.Variables.count(entry.first) != 0)
				{
					blockId = entry.first;
					break;
				}
			}
		}
		if (!blockId.empty())
		{
			const auto variable = module.Variables.find(blockId);
			const auto pointer = module.Pointers.find(variable->second.first);
			const std::string structId = pointer != module.Pointers.end()
				? pointer->second.second : std::string();
			const auto structure = module.StructMembers.find(structId);
			if (structure == module.StructMembers.end())
			{
				if (error)
					*error = std::string("参数块 ") + ParamCbufferName() + " 不是结构体,无法反射";
				return false;
			}

			const auto memberNames = module.MemberNames.find(structId);
			const auto offsets = module.MemberOffsets.find(structId);
			uint32_t end = 0;
			for (size_t index = 0; index < structure->second.size(); ++index)
			{
				MaterialParamLayoutField field;
				field.Name = memberNames != module.MemberNames.end()
					&& memberNames->second.count(static_cast<uint32_t>(index)) != 0
					? memberNames->second.at(static_cast<uint32_t>(index))
					: ("member" + std::to_string(index));
				field.ReflectedType = TypeDisplayName(module, structure->second[index]);
				field.Type = DeriveTypeFromReflected(field.ReflectedType);
				field.Offset = offsets != module.MemberOffsets.end()
					&& offsets->second.count(static_cast<uint32_t>(index)) != 0
					? offsets->second.at(static_cast<uint32_t>(index)) : 0;
				field.Size = TypeSize(module, structure->second[index]);
				end = std::max(end, field.Offset + field.Size);
				out->Fields.push_back(std::move(field));
			}
			out->CbufferSize = RoundUp16(end);

			// 使用情况:OpAccessChain 的**第一个**下标直接索引参数块成员。
			bool dynamicIndex = false;
			std::unordered_set<uint32_t> usedIndices;
			for (const auto& [base, indices] : module.AccessChains)
			{
				if (base != blockId || indices.empty())
					continue;
				const auto constant = module.Constants.find(indices.front());
				if (constant == module.Constants.end())
				{
					dynamicIndex = true;
					continue;
				}
				usedIndices.insert(static_cast<uint32_t>(constant->second));
			}
			for (size_t index = 0; index < structure->second.size(); ++index)
			{
				if (dynamicIndex || usedIndices.count(static_cast<uint32_t>(index)) != 0)
					out->UsedMembers.push_back(out->Fields[index].Name);
			}
			std::sort(out->UsedMembers.begin(), out->UsedMembers.end());
		}

		// 参数贴图槽:set = 2、binding >= 4 的 UniformConstant 变量
		// (引擎 albedo = t1 / normal = t2,不受影响)。
		for (const auto& entry : module.Variables)
		{
			const std::string& id = entry.first;
			if (entry.second.second != "UniformConstant")
				continue;
			const auto decorations = module.Decorations.find(id);
			if (decorations == module.Decorations.end())
				continue;
			const auto set = decorations->second.find("DescriptorSet");
			const auto binding = decorations->second.find("Binding");
			if (set == decorations->second.end() || binding == decorations->second.end())
				continue;
			const uint32_t setValue = static_cast<uint32_t>(set->second);
			const uint32_t bindingValue = static_cast<uint32_t>(binding->second);
			if (setValue != 2 || bindingValue < ParamTextureBaseBinding())
				continue;
			MaterialParamTextureSlot slot;
			slot.Name = module.Names.count(id) != 0 ? module.Names.at(id) : id;
			const auto pointer = module.Pointers.find(entry.second.first);
			std::string pointee = pointer != module.Pointers.end()
				? pointer->second.second : std::string();
			const auto sampled = module.SampledImages.find(pointee);
			if (sampled != module.SampledImages.end())
				pointee = sampled->second;
			slot.ReflectedType = TypeDisplayName(module, pointee);
			slot.Set = setValue;
			slot.Binding = bindingValue;
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

	bool BuildParamLayout(const std::string& hlslSource, const std::vector<MaterialParamDecl>& table,
		MaterialParamLayout* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "输出布局为空";
			return false;
		}
		const SurfaceCompileResult compiled = MaterialSurfaceCompiler::CompileSurfaceWithParams(
			hlslSource, table, kParamPermutationKey);
		if (!compiled.Success)
		{
			std::vector<std::string> messages;
			for (const SurfaceDiagnostic& diagnostic : compiled.Diagnostics)
			{
				if (diagnostic.Severity == "error")
					messages.push_back(diagnostic.Message);
			}
			if (messages.empty())
				messages.push_back("dxc 编译失败但没有诊断输出");
			if (error) *error = "参数表编译失败: " + Join(messages);
			return false;
		}
		const std::string assemblyPath = MaterialSurfaceCompiler::AssemblyPath(compiled.Artifact);
		if (assemblyPath.empty())
		{
			if (error)
				*error = "参数表编译产物缺少 SPIR-V 汇编(-Fc 输出),无法反射;请重新编译该着色器";
			return false;
		}
		std::string assembly;
		if (!ReadTextFile(fs::path(assemblyPath), assembly))
		{
			if (error) *error = "读不到反射用的 SPIR-V 汇编: " + assemblyPath;
			return false;
		}
		MaterialParamLayout layout;
		if (!ReflectParamLayoutFromAssembly(assembly, &layout, error))
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
