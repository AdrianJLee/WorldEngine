#include "wldpch.h"

#include "../EditorLayer.h"

#include "World/Core/Log.h"
#include "World/Plugins/PluginManager.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Scene/Components.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Script/ScriptProperties.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/Utils/Paths.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>

namespace World
{
	namespace
	{
		std::string JoinLines(const std::vector<std::string>& lines)
		{
			std::ostringstream out;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				if (i)
					out << "\n";
				out << lines[i];
			}
			return out.str();
		}

		// JSON 文本转义:面板 id / 材质路径 / 场景标签都可能含 '"' 或 '\'(Windows 路径),
		// 不转义会让 state.dump 变成非法 JSON(脚本侧 json.loads 直接失败)。
		std::string JsonEscape(const std::string& text)
		{
			std::string escaped;
			escaped.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': escaped += "\\\""; break;
					case '\\': escaped += "\\\\"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\t': escaped += "\\t"; break;
					default: escaped += c; break;
				}
			}
			return escaped;
		}

		bool IsUnder(const std::filesystem::path& child, const std::filesystem::path& parent)
		{
			std::error_code ec;
			const std::filesystem::path normalizedChild = std::filesystem::weakly_canonical(child, ec);
			const std::filesystem::path normalizedParent = std::filesystem::weakly_canonical(parent, ec);
			if (normalizedChild.empty() || normalizedParent.empty())
				return false;
			const auto mismatch = std::mismatch(normalizedParent.begin(), normalizedParent.end(),
				normalizedChild.begin(), normalizedChild.end());
			return mismatch.first == normalizedParent.end();
		}

		// P2 W5b:脚本实例状态名(script.status/script.reload 的 JSON 字段)。
		const char* ScriptStateLabel(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}

		// CPPT-3:脚本属性值的一行文本(script.status 的 properties 数组;只做断言用,
		// 不做本地化 —— 与属性面板的展示格式同量级:整数/浮点/向量/字符串)。
		std::string ScriptValueText(const Schema::Value& value)
		{
			char buffer[192] = {};
			return std::visit([&buffer](const auto& item) -> std::string
			{
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_same_v<T, std::monostate>)
					return std::string();
				else if constexpr (std::is_same_v<T, bool>)
					return item ? "true" : "false";
				else if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t>
					|| std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>)
					return std::to_string(static_cast<int64_t>(item));
				else if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t>
					|| std::is_same_v<T, uint32_t> || std::is_same_v<T, uint64_t>)
					return std::to_string(static_cast<uint64_t>(item));
				else if constexpr (std::is_same_v<T, float>)
				{
					std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(item));
					return buffer;
				}
				else if constexpr (std::is_same_v<T, double>)
				{
					std::snprintf(buffer, sizeof(buffer), "%.6g", item);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::vec2>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.4g, %.4g)", item.x, item.y);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::vec3>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.4g, %.4g, %.4g)", item.x, item.y, item.z);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::vec4>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%.4g, %.4g, %.4g, %.4g)",
						item.x, item.y, item.z, item.w);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::ivec2>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%d, %d)", item.x, item.y);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::ivec3>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%d, %d, %d)", item.x, item.y, item.z);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::ivec4>)
				{
					std::snprintf(buffer, sizeof(buffer), "(%d, %d, %d, %d)",
						item.x, item.y, item.z, item.w);
					return buffer;
				}
				else if constexpr (std::is_same_v<T, glm::uvec2> || std::is_same_v<T, glm::uvec3>
					|| std::is_same_v<T, glm::uvec4>)
				{
					return "(unsigned)";
				}
				else if constexpr (std::is_same_v<T, std::string>)
					return item;
				else
					return "(complex)";
			}, value);
		}

		// ui.type 的返回文案用"字符数":统计 UTF-8 码点数(中文按 1 个字符),与控件
		// 收到的 TextInput 数量一致。
		size_t Utf8CodepointCount(const std::string& text)
		{
			size_t count = 0;
			for (char byte : text)
				if ((static_cast<unsigned char>(byte) & 0xC0u) != 0x80u)
					++count;
			return count;
		}

		// 写文件类命令的路径白名单:相对路径落在构建输出目录,绝对路径必须位于
		// 临时目录或当前工作目录之内 —— 控制通道不接受"写到系统任意位置"。
		std::filesystem::path ResolveCapturePath(const std::string& raw, std::string* error)
		{
			if (raw.empty())
			{
				if (error)
					*error = "capture path is empty";
				return {};
			}
			std::error_code ec;
			std::filesystem::path path(raw);
			if (path.is_absolute())
			{
				const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
				const std::filesystem::path cwd = std::filesystem::current_path(ec);
				if (IsUnder(path, temp) || IsUnder(path, cwd))
					return path;
				if (error)
					*error = "absolute capture paths must live under the temp dir or the working dir";
				return {};
			}
			return std::filesystem::path(WLD_OUTPUT_DIR) / path;
		}

		bool ParseFloat3(const std::string& text, glm::vec3* out)
		{
			std::stringstream stream(text);
			std::string part;
			glm::vec3 value { 0.0f };
			int index = 0;
			while (std::getline(stream, part, ',') && index < 3)
				value[index++] = std::strtof(part.c_str(), nullptr);
			if (index < 3)
				return false;
			*out = value;
			return true;
		}

		// ---- PLUG-CLEAN-1:schema 字段访问器(场景命令读写插件组件) ----------------------
		//
		// `scene.get/set` 原先只认内建组件(Tag/Transform/MeshRenderer/…),T2c 起插件组件
		// 能挂到实体上但没有任何写入口(探针只能用 `.wd` 写值)。这里补一条**通用 schema
		// 通路**,与属性面板/序列化共用同一批字段访问器,不按偏移自己 memcpy:
		//   * 组件解析 = 类型全名(full id)/ 短名(最后一个 `::` 之后)/ 显示名;
		//   * 实例指针从 entt 存储直接取 —— 只读查询走 **const registry**,Play/Simulate 下
		//     不触发"活动场景禁止结构写"断言(与 scene.list 同一口径);
		//   * 字段读写 = Schema::ReadSchemaField / WriteSchemaField(FieldSchema::Get/Set 的
		//     宿主封装;monostate/Transient 语义与序列化一致)。
		//
		// 命令口径(扁平 JSON 参数,与既有命令一致):
		//   scene.get ... component=<类型> [property=<字段>]
		//   scene.set ... component=<类型> property=<字段> value=<文本>
		// 不带 component= 时输出/行为与旧版逐字节一致(内建属性回归口径)。
		std::string TrimAscii(const std::string& text)
		{
			const size_t first = text.find_first_not_of(" \t");
			if (first == std::string::npos)
				return {};
			const size_t last = text.find_last_not_of(" \t");
			return text.substr(first, last - first + 1);
		}

		bool ParseSignedNumber(const std::string& text, int64_t* out)
		{
			const std::string trimmed = TrimAscii(text);
			if (trimmed.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const long long value = std::strtoll(trimmed.c_str(), &end, 10);
			if (end == trimmed.c_str() || *end != '\0' || errno == ERANGE)
				return false;
			*out = static_cast<int64_t>(value);
			return true;
		}

		bool ParseUnsignedNumber(const std::string& text, uint64_t* out)
		{
			const std::string trimmed = TrimAscii(text);
			if (trimmed.empty() || trimmed.front() == '-')
				return false;
			errno = 0;
			char* end = nullptr;
			const unsigned long long value = std::strtoull(trimmed.c_str(), &end, 10);
			if (end == trimmed.c_str() || *end != '\0' || errno == ERANGE)
				return false;
			*out = static_cast<uint64_t>(value);
			return true;
		}

		bool ParseRealNumber(const std::string& text, double* out)
		{
			const std::string trimmed = TrimAscii(text);
			if (trimmed.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const double value = std::strtod(trimmed.c_str(), &end);
			if (end == trimmed.c_str() || *end != '\0' || errno == ERANGE)
				return false;
			*out = value;
			return true;
		}

		// "a,b,c" → 数量精确匹配的分量表(向量/四元数/矩阵用;空格容忍)。
		bool ParseNumberList(const std::string& text, size_t count, std::vector<double>* realOut,
			std::vector<int64_t>* signedOut, std::vector<uint64_t>* unsignedOut)
		{
			std::stringstream stream(text);
			std::string part;
			std::vector<std::string> parts;
			while (std::getline(stream, part, ','))
				parts.push_back(part);
			if (parts.size() != count || parts.empty())
				return false;
			if (realOut)
			{
				realOut->clear();
				for (const std::string& item : parts)
				{
					double value = 0.0;
					if (!ParseRealNumber(item, &value))
						return false;
					realOut->push_back(value);
				}
				return true;
			}
			if (signedOut)
			{
				signedOut->clear();
				for (const std::string& item : parts)
				{
					int64_t value = 0;
					if (!ParseSignedNumber(item, &value))
						return false;
					signedOut->push_back(value);
				}
				return true;
			}
			if (unsignedOut)
			{
				unsignedOut->clear();
				for (const std::string& item : parts)
				{
					uint64_t value = 0;
					if (!ParseUnsignedNumber(item, &value))
						return false;
					unsignedOut->push_back(value);
				}
				return true;
			}
			return false;
		}

		std::string SchemaValueToJson(const Schema::Value& value)
		{
			return std::visit([](const auto& item) -> std::string
			{
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_same_v<T, std::monostate>)
				{
					return "null";
				}
				else if constexpr (std::is_same_v<T, bool>)
				{
					return item ? "true" : "false";
				}
				else if constexpr (std::is_same_v<T, std::string>)
				{
					return "\"" + JsonEscape(item) + "\"";
				}
				else if constexpr (std::is_integral_v<T>)
				{
					if constexpr (std::is_signed_v<T>)
						return std::to_string(static_cast<int64_t>(item));
					else
						return std::to_string(static_cast<uint64_t>(item));
				}
				else if constexpr (std::is_floating_point_v<T>)
				{
					std::ostringstream stream;
					stream << std::setprecision(std::numeric_limits<T>::max_digits10) << item;
					return stream.str();
				}
				else if constexpr (std::is_same_v<T, glm::vec2> || std::is_same_v<T, glm::vec3>
					|| std::is_same_v<T, glm::vec4> || std::is_same_v<T, glm::ivec2>
					|| std::is_same_v<T, glm::ivec3> || std::is_same_v<T, glm::ivec4>
					|| std::is_same_v<T, glm::uvec2> || std::is_same_v<T, glm::uvec3>
					|| std::is_same_v<T, glm::uvec4>)
				{
					std::string text = "[";
					for (glm::length_t component = 0; component < item.length(); ++component)
					{
						if (component > 0)
							text += ",";
						std::ostringstream stream;
						stream << std::setprecision(std::numeric_limits<float>::max_digits10)
							<< static_cast<double>(item[component]);
						text += stream.str();
					}
					text += "]";
					return text;
				}
				else if constexpr (std::is_same_v<T, glm::quat>)
				{
					std::ostringstream stream;
					stream << "[" << std::setprecision(std::numeric_limits<float>::max_digits10)
						<< item.w << "," << item.x << "," << item.y << "," << item.z << "]";
					return stream.str();
				}
				else if constexpr (std::is_same_v<T, glm::mat3> || std::is_same_v<T, glm::mat4>)
				{
					std::string text = "[";
					const glm::length_t rows = item.length();
					for (glm::length_t column = 0; column < rows; ++column)
					{
						if (column > 0)
							text += ",";
						text += SchemaValueToJson(Schema::Value(item[column]));
					}
					text += "]";
					return text;
				}
				else if constexpr (std::is_same_v<T, Schema::ValueList> || std::is_same_v<T, Schema::ValueMap>)
				{
					if constexpr (std::is_same_v<T, Schema::ValueList>)
					{
						std::string text = "[";
						for (size_t index = 0; index < item.size(); ++index)
						{
							if (index > 0)
								text += ",";
							text += SchemaValueToJson(item[index]);
						}
						text += "]";
						return text;
					}
					else
					{
						std::string text = "{";
						bool first = true;
						for (const auto& [key, entry] : item)
						{
							if (!first)
								text += ",";
							first = false;
							text += "\"" + JsonEscape(key) + "\":" + SchemaValueToJson(entry);
						}
						text += "}";
						return text;
					}
				}
				else
				{
					return "null";
				}
			}, value);
		}

		std::string SchemaTypeShortName(const Schema::TypeSchema& schema)
		{
			const std::string& name = schema.Id.Name;
			const size_t separator = name.rfind("::");
			return separator == std::string::npos ? name : name.substr(separator + 2);
		}

		const Schema::TypeSchema* FindComponentSchema(const Schema::SchemaRegistry& schemas,
			const std::string& name)
		{
			if (const Schema::TypeSchema* exact = schemas.Find(name))
				return exact;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				if (schema && (schema->Id.Name == name || SchemaTypeShortName(*schema) == name
					|| schema->DisplayName == name))
					return schema;
			return nullptr;
		}

		const Schema::FieldSchema* FindComponentField(const Schema::TypeSchema& schema,
			const std::string& name)
		{
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == name
					|| (!field.Meta.DisplayName.empty() && field.Meta.DisplayName == name))
					return &field;
			return nullptr;
		}

		// 文本 → Schema::Value:只接受 schema 字段的**规范类型**(数值/向量/四元数/矩阵/字符串;
		// 插件组件的 POD 字段集合 = God 面里的固定尺寸子集,见 WeComponentKind*)。形状不支持 =
		// 可读错误,不猜、不静默转 0。
		bool ParseSchemaFieldValue(const Schema::FieldSchema& field, const std::string& text,
			Schema::Value* out, std::string* error)
		{
			const auto fail = [error](const std::string& reason)
			{
				if (error)
					*error = reason;
				return false;
			};
			std::vector<double> real;
			std::vector<int64_t> signedInts;
			std::vector<uint64_t> unsignedInts;
			int64_t signedValue = 0;
			uint64_t unsignedValue = 0;
			double realValue = 0.0;
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					std::string lowered = TrimAscii(text);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (lowered == "1" || lowered == "true")
					{
						*out = true;
						return true;
					}
					if (lowered == "0" || lowered == "false")
					{
						*out = false;
						return true;
					}
					return fail("value must be 0|1|true|false for Bool field '" + field.Name + "'");
				}
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
					if (!ParseSignedNumber(text, &signedValue))
						return fail("value must be an integer for field '" + field.Name + "'");
					if (field.K == Schema::Kind::Int8) *out = static_cast<int8_t>(signedValue);
					else if (field.K == Schema::Kind::Int16) *out = static_cast<int16_t>(signedValue);
					else if (field.K == Schema::Kind::Int32) *out = static_cast<int32_t>(signedValue);
					else *out = signedValue;
					return true;
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64:
					if (!ParseUnsignedNumber(text, &unsignedValue))
						return fail("value must be a non-negative integer for field '" + field.Name + "'");
					if (field.K == Schema::Kind::UInt8) *out = static_cast<uint8_t>(unsignedValue);
					else if (field.K == Schema::Kind::UInt16) *out = static_cast<uint16_t>(unsignedValue);
					else if (field.K == Schema::Kind::UInt32) *out = static_cast<uint32_t>(unsignedValue);
					else *out = unsignedValue;
					return true;
				case Schema::Kind::Float:
					if (!ParseRealNumber(text, &realValue))
						return fail("value must be a number for field '" + field.Name + "'");
					*out = static_cast<float>(realValue);
					return true;
				case Schema::Kind::Double:
					if (!ParseRealNumber(text, &realValue))
						return fail("value must be a number for field '" + field.Name + "'");
					*out = realValue;
					return true;
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					const size_t count = field.K == Schema::Kind::Vec2 ? 2
						: (field.K == Schema::Kind::Vec3 ? 3 : 4);
					if (!ParseNumberList(text, count, &real, nullptr, nullptr))
						return fail("value must be " + std::to_string(count)
							+ " comma-separated numbers for field '" + field.Name + "'");
					if (count == 2)
						*out = glm::vec2(static_cast<float>(real[0]), static_cast<float>(real[1]));
					else if (count == 3)
						*out = glm::vec3(static_cast<float>(real[0]), static_cast<float>(real[1]),
							static_cast<float>(real[2]));
					else
						*out = glm::vec4(static_cast<float>(real[0]), static_cast<float>(real[1]),
							static_cast<float>(real[2]), static_cast<float>(real[3]));
					return true;
				}
				case Schema::Kind::IVec2:
				case Schema::Kind::IVec3:
				case Schema::Kind::IVec4:
				{
					const size_t count = field.K == Schema::Kind::IVec2 ? 2
						: (field.K == Schema::Kind::IVec3 ? 3 : 4);
					if (!ParseNumberList(text, count, nullptr, &signedInts, nullptr))
						return fail("value must be " + std::to_string(count)
							+ " comma-separated integers for field '" + field.Name + "'");
					if (count == 2)
						*out = glm::ivec2(static_cast<int32_t>(signedInts[0]), static_cast<int32_t>(signedInts[1]));
					else if (count == 3)
						*out = glm::ivec3(static_cast<int32_t>(signedInts[0]), static_cast<int32_t>(signedInts[1]),
							static_cast<int32_t>(signedInts[2]));
					else
						*out = glm::ivec4(static_cast<int32_t>(signedInts[0]), static_cast<int32_t>(signedInts[1]),
							static_cast<int32_t>(signedInts[2]), static_cast<int32_t>(signedInts[3]));
					return true;
				}
				case Schema::Kind::UVec2:
				case Schema::Kind::UVec3:
				case Schema::Kind::UVec4:
				{
					const size_t count = field.K == Schema::Kind::UVec2 ? 2
						: (field.K == Schema::Kind::UVec3 ? 3 : 4);
					if (!ParseNumberList(text, count, nullptr, nullptr, &unsignedInts))
						return fail("value must be " + std::to_string(count)
							+ " comma-separated non-negative integers for field '" + field.Name + "'");
					if (count == 2)
						*out = glm::uvec2(static_cast<uint32_t>(unsignedInts[0]), static_cast<uint32_t>(unsignedInts[1]));
					else if (count == 3)
						*out = glm::uvec3(static_cast<uint32_t>(unsignedInts[0]), static_cast<uint32_t>(unsignedInts[1]),
							static_cast<uint32_t>(unsignedInts[2]));
					else
						*out = glm::uvec4(static_cast<uint32_t>(unsignedInts[0]), static_cast<uint32_t>(unsignedInts[1]),
							static_cast<uint32_t>(unsignedInts[2]), static_cast<uint32_t>(unsignedInts[3]));
					return true;
				}
				case Schema::Kind::Quat:
					if (!ParseNumberList(text, 4, &real, nullptr, nullptr))
						return fail("value must be 4 comma-separated numbers (w,x,y,z) for field '"
							+ field.Name + "'");
					*out = glm::quat(static_cast<float>(real[0]), static_cast<float>(real[1]),
						static_cast<float>(real[2]), static_cast<float>(real[3]));
					return true;
				case Schema::Kind::Mat3:
				case Schema::Kind::Mat4:
				{
					const size_t count = field.K == Schema::Kind::Mat3 ? 9 : 16;
					if (!ParseNumberList(text, count, &real, nullptr, nullptr))
						return fail("value must be " + std::to_string(count)
							+ " comma-separated numbers for field '" + field.Name + "'");
					if (count == 9)
					{
						glm::mat3 matrix { 0.0f };
						for (size_t index = 0; index < count; ++index)
							matrix[index / 3][index % 3] = static_cast<float>(real[index]);
						*out = matrix;
					}
					else
					{
						glm::mat4 matrix { 0.0f };
						for (size_t index = 0; index < count; ++index)
							matrix[index / 4][index % 4] = static_cast<float>(real[index]);
						*out = matrix;
					}
					return true;
				}
				case Schema::Kind::String:
				case Schema::Kind::Asset:
					*out = text;
					return true;
				default:
					return fail("field '" + field.Name
						+ "' has a kind this text channel does not support (Object/Enum/container)");
			}
		}

		// ---- U23:鼠标注入(跨帧按住 + 位移 = 真拖拽)----
		//
		// 为什么走 PostMessage 的 WM_* 鼠标消息,而不是直接改 WUI 输入态:
		//   * GLFW 会把 WM_* 变成正常的鼠标事件,交给**该窗口自己的**输入收集器:
		//     主窗口 = WuiRhiBackend::s_Input(停靠面板与主视口共用),
		//     独立窗口 = FloatWindowHost 里那份局部 collector;
		//   * 于是宿主自己跟踪的路径(EditorLayer 的主视口右键轨道 = GLFW 事件)与
		//     WUI 面板(材质/模型/预制体预览的左键轨道)走的是**用户真实操作的同一条路**;
		//   * "按住"跨帧保持由 WuiInputCollector::SyncButtonsWithSystem 的虚拟按键
		//     规则负责(按下那一刻系统按键是抬起的 → 不在下一帧被自动清掉)。
		//
		// 坐标 = **设计单位**(与无障碍节点 rect 同一坐标系,窗口客户区原点);
		// 注入时按 Wui::UiScale() 换算成物理像素,与 WuiInputCollector::OnMouseMove 的
		// 除法互为逆运算。
		struct ScriptedMouseState
		{
			bool Held[3] = { false, false, false };
			int LastButton = 0;
			glm::vec2 Last { 0.0f, 0.0f };
			std::string Window;
			void* Handle = nullptr;
		};

		ScriptedMouseState& ScriptedMouse()
		{
			static ScriptedMouseState state;
			return state;
		}

		std::wstring Utf8ToWide(const std::string& text)
		{
			if (text.empty())
				return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
				static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return {};
			std::wstring wide(static_cast<size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				wide.data(), length);
			return wide;
		}

		std::wstring ToLowerWide(std::wstring text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
			return text;
		}

		std::string WideToUtf8(const std::wstring& text)
		{
			if (text.empty())
				return {};
			const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
				static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			if (length <= 0)
				return {};
			std::string utf8(static_cast<size_t>(length), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				utf8.data(), length, nullptr, nullptr);
			return utf8;
		}

		// 面板 id → 标题提示:浮窗标题由面板自己的 Title() 决定(材质 = "Material - <stem>"),
		// 取面板 id 里最先出现的路径段文件名(去扩展名)做包含匹配,避免在这里复刻各面板标题。
		std::string TitleHintForPanel(const std::string& panel)
		{
			std::string hint = panel;
			const size_t colon = hint.find(':');
			if (colon != std::string::npos)
				hint = hint.substr(colon + 1);
			const size_t slash = hint.find_last_of("/\\");
			if (slash != std::string::npos)
				hint = hint.substr(slash + 1);
			const size_t dot = hint.find_last_of('.');
			if (dot != std::string::npos && dot > 0)
				hint = hint.substr(0, dot);
			return hint;
		}

		// 本进程的 GLFW 顶层窗口(主窗口 + 各独立窗口)。不依赖 GLFW 头:
		// 编辑器目标没有 GLFW 的 include 路径,而 Win32 枚举本来就能拿到 HWND、
		// 标题与客户区尺寸,足够把注入送到正确的窗口。
		struct ProcessWindow
		{
			HWND Handle = nullptr;
			std::wstring Title;
			int Width = 0;
			int Height = 0;
			bool Decorated = false;   // 有系统标题栏/边框 = 主窗口(独立窗口是无边框的)
		};

		BOOL CALLBACK CollectProcessWindow(HWND hwnd, LPARAM context)
		{
			auto* windows = reinterpret_cast<std::vector<ProcessWindow>*>(context);
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid != GetCurrentProcessId())
				return TRUE;
			wchar_t className[64] = {};
			if (GetClassNameW(hwnd, className, 64) <= 0 || wcsncmp(className, L"GLFW", 4) != 0)
				return TRUE;
			RECT client {};
			if (!GetClientRect(hwnd, &client))
				return TRUE;
			const int width = client.right - client.left;
			const int height = client.bottom - client.top;
			if (width < 8 || height < 8)
				return TRUE;   // GLFW 的 0×0 message window(不是用户窗口)
			wchar_t title[512] = {};
			GetWindowTextW(hwnd, title, 512);
			const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
			windows->push_back(ProcessWindow { hwnd, title, width, height, (style & WS_CAPTION) != 0 });
			return TRUE;
		}

		std::vector<ProcessWindow> CollectProcessWindows()
		{
			std::vector<ProcessWindow> windows;
			EnumWindows(&CollectProcessWindow, reinterpret_cast<LPARAM>(&windows));
			return windows;
		}

		// 主窗口 = 本进程**有系统标题栏**的 GLFW 窗口里客户区最大的那个(独立窗口一律无边框);
		// 万一都无边框,退回"客户区最大的 GLFW 窗口"。
		HWND MainWindowHandle()
		{
			if (!Application::HasInstance())
				return nullptr;
			const std::vector<ProcessWindow> windows = CollectProcessWindows();
			const ProcessWindow* best = nullptr;
			for (const ProcessWindow& window : windows)
			{
				if (!window.Decorated)
					continue;
				if (!best || 1ll * window.Width * window.Height > 1ll * best->Width * best->Height)
					best = &window;
			}
			if (!best)
				for (const ProcessWindow& window : windows)
					if (!best || 1ll * window.Width * window.Height > 1ll * best->Width * best->Height)
						best = &window;
			return best ? best->Handle : nullptr;
		}

		// 目标窗口解析:"main" 或 "float:<面板>"。独立窗口按标题提示(可被 title= 覆盖)
		// 匹配本进程的 GLFW 顶层窗口;只剩一扇浮窗时直接认它(隐藏启动的自动化常态)。
		HWND ResolveInjectionWindow(const std::string& windowKey, const std::string& titleOverride,
			std::string* error)
		{
			if (windowKey.empty() || windowKey == "main")
			{
				if (HWND main = MainWindowHandle())
					return main;
				if (error) *error = "main window is not available";
				return nullptr;
			}
			if (windowKey.rfind("float:", 0) != 0)
			{
				if (error) *error = "unknown window '" + windowKey + "' (expected main or float:<panel>)";
				return nullptr;
			}
			const HWND main = MainWindowHandle();
			std::vector<ProcessWindow> candidates;
			for (const ProcessWindow& window : CollectProcessWindows())
				if (window.Handle != main)
					candidates.push_back(window);
			if (candidates.empty())
			{
				if (error)
					*error = "no independent window is open for '" + windowKey
						+ "' (打开为独立窗口后才能注入;附加到主窗口时用 window=main)";
				return nullptr;
			}
			const std::wstring hint = ToLowerWide(Utf8ToWide(
				titleOverride.empty() ? TitleHintForPanel(windowKey.substr(6)) : titleOverride));
			std::vector<const ProcessWindow*> matched;
			if (!hint.empty())
				for (const ProcessWindow& candidate : candidates)
					if (ToLowerWide(candidate.Title).find(hint) != std::wstring::npos)
						matched.push_back(&candidate);
			if (matched.size() == 1)
				return matched.front()->Handle;
			// 面板 id 推不出提示、且当前只有一扇浮窗:认它(隐藏启动的自动化常态)。
			// 有提示但一扇都匹配不上时**不猜** —— 否则面板 id 写错会把事件注进别的窗口。
			if (hint.empty() && candidates.size() == 1)
				return candidates.front().Handle;
			if (error)
			{
				*error = matched.empty()
					? ("no independent window matches '" + windowKey + "'; open windows:")
					: ("cannot tell which independent window '" + windowKey
						+ "' is: pass title=<窗标题片段>; open windows:");
				for (const ProcessWindow& candidate : candidates)
					*error += " [" + WideToUtf8(candidate.Title) + "]";
			}
			return nullptr;
		}

		bool ParseMouseButton(const std::string& text, int fallback, int* out, std::string* error)
		{
			if (text.empty())
			{
				*out = fallback;
				return true;
			}
			if (text == "left" || text == "0") { *out = 0; return true; }
			if (text == "right" || text == "1") { *out = 1; return true; }
			if (text == "middle" || text == "2") { *out = 2; return true; }
			if (error) *error = "unknown button '" + text + "' (expected left/right/middle)";
			return false;
		}

		glm::vec2 ToPhysicalPixels(const glm::vec2& designUnits)
		{
			const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			return designUnits * scale;
		}

		// VEC-H4:鼠标注入改成**同步**投递(SendMessageW),并统一给失败理由。
		//
		// 为什么(证据在 tools/agents/reports/VEC-H3.md):
		// PostMessageW 的 WM_MOUSEMOVE 会被 Windows 的鼠标消息合并/延后 —— 实测约 10% 的拖动里,
		// "位移"在 WM_LBUTTONUP **之后**才被派发,控件在按住期间从没看到位移,松手那一帧被当成单击
		// (DragFloat 走 BeginNumericEdit,值不变 → 探针 20s 超时)。对照实验:全部 PostMessage 4/40 失败;
		// 全部 SendMessage 1/40、0/60。同步投递保证"位移在抬起之前、且在同一调用点就已落入窗口过程"。
		// 返回值:成功投递 = true;窗口已销毁 = false + 理由(旧实现忽略返回值,失败是静默的)。
		bool PostMouseMessage(HWND hwnd, UINT message, WPARAM wparam, const glm::vec2& physical,
			std::string* error = nullptr)
		{
			if (!hwnd || IsWindow(hwnd) == FALSE)
			{
				if (error)
					*error = "injection window is gone (was it closed?)";
				return false;
			}
			const LPARAM lparam = MAKELPARAM(static_cast<int>(physical.x) & 0xFFFF,
				static_cast<int>(physical.y) & 0xFFFF);
			// 同一个线程拥有窗口 ⇒ SendMessageW 直接调用窗口过程,不经过消息队列,不会死锁。
			SendMessageW(hwnd, message, wparam, lparam);
			return true;
		}

		WPARAM ButtonMaskFor(const ScriptedMouseState& state)
		{
			WPARAM mask = 0;
			if (state.Held[0]) mask |= MK_LBUTTON;
			if (state.Held[1]) mask |= MK_RBUTTON;
			if (state.Held[2]) mask |= MK_MBUTTON;
			return mask;
		}

		WPARAM ButtonMaskOf(int button)
		{
			return button == 1 ? MK_RBUTTON : (button == 2 ? MK_MBUTTON : MK_LBUTTON);
		}

		const char* ButtonName(int button)
		{
			return button == 1 ? "right" : (button == 2 ? "middle" : "left");
		}
	}

	void EditorLayer::StartAiRecording(const std::string& path)
	{
		m_AiRecordPath = path;
		m_AiRecordCount = 0;
		// 覆盖式开始:录制文件代表"从现在起的会话"。
		std::ofstream file(path, std::ios::trunc);
	}

	size_t EditorLayer::StopAiRecording()
	{
		const size_t count = m_AiRecordCount;
		m_AiRecordPath.clear();
		m_AiRecordCount = 0;
		return count;
	}

	void EditorLayer::AiRecordCommand(const std::string& cmd, const std::map<std::string, std::string>& args)
	{
		if (m_AiRecordPath.empty())
			return;
		// 录制/回放/退出类命令不进脚本:回放时会跳过它们,写进去只会让脚本难以阅读。
		if (cmd == "record.start" || cmd == "record.stop" || cmd == "replay" || cmd == "quit")
			return;
		std::ofstream file(m_AiRecordPath, std::ios::app);
		if (!file)
		{
			WLD_CORE_WARN("[ai] cannot append to record file '{0}'", m_AiRecordPath);
			m_AiRecordPath.clear();
			return;
		}
		file << "{\"cmd\":\"" << cmd << "\"";
		for (const auto& entry : args)
		{
			if (entry.first == "seq" || entry.first == "cmd")
				continue;
			file << ",\"" << entry.first << "\":\"" << entry.second << "\"";
		}
		file << "}\n";
		++m_AiRecordCount;
	}

	std::string EditorLayer::DescribeAiScene() const
	{
		std::ostringstream out;
		out << "{";
		out << "\"backend\":\"" << Renderer::GetBackendName() << "\"";
		out << ",\"window\":[" << Application::Get().GetWindow().GetWidth() << ","
			<< Application::Get().GetWindow().GetHeight() << "]";
		const std::filesystem::path scenePath = m_Document.GetPath();
		out << ",\"scene\":\"" << JsonEscape(scenePath.generic_string()) << "\"";
		out << ",\"dirty\":" << (m_Document.IsDirty() ? "true" : "false");
		out << ",\"playState\":" << static_cast<int>(m_SceneState)
			<< ",\"paused\":" << (m_ScenePaused ? "true" : "false");
		out << ",\"entities\":[";
		bool first = true;
		if (m_ActiveScene)
		{
			// 只读枚举必须走 const 视图:Play/Simulate 下活动场景的**非 const** GetRegistry()
			// 会触发"活动场景禁止结构写"断言(实测会让异常逃逸并终止进程)。
			const Scene& scene = *m_ActiveScene;
			const entt::registry& registry = scene.GetRegistry();
			for (auto handle : registry.view<TagComponent>())
			{
				if (!first)
					out << ",";
				first = false;
				const std::string& tag = registry.get<TagComponent>(handle).Tag;
				const uint32_t id = static_cast<uint32_t>(handle);
				out << "{\"handle\":" << id << ",\"name\":\"" << JsonEscape(tag) << "\"";
				if (handle == static_cast<entt::entity>(m_SelectedEntity))
					out << ",\"selected\":true";
				out << "}";
			}
		}
		// 必须是合法 JSON:之前写成 `"renderer":960x282`(裸的 960x282 不是 JSON 值),
		// 脚本侧 json.loads 直接失败。改成数组。
		out << "],\"renderer\":[" << m_SceneRenderer->GetWidth() << "," << m_SceneRenderer->GetHeight() << "]";
		out << ",\"panels\":" << m_Shell.AiDescribeState();
		out << "}";
		return out.str();
	}

	bool EditorLayer::ExecuteAiCommand(const std::string& cmd, const std::map<std::string, std::string>& args,
		std::string& result, std::string& error)
	{
		auto arg = [&](const char* key, const std::string& fallback = std::string()) -> std::string
		{
			const auto found = args.find(key);
			return found == args.end() ? fallback : found->second;
		};
		// 面板 id 便捷写法:materials/xxx.wmat → material:materials/xxx.wmat。
		auto normalizePanel = [](std::string panel)
		{
			if (!panel.empty() && panel.rfind("material:", 0) != 0 && panel.find(".wmat") != std::string::npos)
				panel = "material:" + panel;
			return panel;
		};
		// ui.invoke / ui.type 共用的无障碍节点解析(id 优先,其次 label(+kind));
		// 失败时把与 ui.invoke 逐字相同的错误文案写进 message。
		auto resolveNode = [&](std::string& message) -> const Wui::WuiAccessNode*
		{
			const Wui::WuiAccessibility& tree = Wui::WuiAccessibility::Get();
			const Wui::WuiAccessNode* node = nullptr;
			if (!arg("id").empty())
				node = tree.Find(static_cast<Wui::WuiId>(std::strtoull(arg("id").c_str(), nullptr, 10)));
			else
				node = tree.FindByLabel(arg("label"), arg("kind"));
			if (!node)
			{
				message = "no matching ui node (id/label); call ui.tree first";
				return nullptr;
			}
			if (!node->Interactive || !node->Enabled)
			{
				message = node->Visible
					? ("ui node is not interactive/enabled: " + node->Kind + " '" + node->Label + "'")
					: ("ui node is off-screen (中心点不在窗口客户区内,窗口太小): " + node->Kind
						+ " '" + node->Label + "' — 先 ui.resize 放大窗口");
				return nullptr;
			}
			return node;
		};

		if (cmd == "hello")
		{
			std::ostringstream out;
			out << "{\"app\":\"WorldEngine.Editor\",\"backend\":\"" << Renderer::GetBackendName() << "\""
				<< ",\"window\":[" << Application::Get().GetWindow().GetWidth() << ","
				<< Application::Get().GetWindow().GetHeight() << "]"
				<< ",\"ai\":" << (m_AiServer ? "true" : "false") << "}";
			result = out.str();
			return true;
		}
		if (cmd == "state.dump")
		{
			result = DescribeAiScene();
			return true;
		}
		if (cmd == "stats.scene")
		{
			// P1b D8a:场景渲染统计(剔除/提交规模/CPU 耗时),压力场景脚本与 Stats 面板同源。
			const Renderer3D::SceneStatistics scene = Renderer3D::GetSceneStatistics();
			std::ostringstream out;
			out << "{\"objects\":" << scene.Objects
				<< ",\"visible\":" << scene.Submitted
				<< ",\"culled\":" << scene.Culled
				<< ",\"shadowCasters\":" << scene.ShadowCasters
				<< ",\"drawCalls\":" << scene.DrawCalls
				<< ",\"triangles\":" << scene.Triangles
				<< ",\"droppedObjects\":" << scene.DroppedObjects
				<< ",\"culling\":" << (scene.CullingEnabled ? "true" : "false")
				<< ",\"instancing\":" << (scene.InstancingEnabled ? "true" : "false")
				<< ",\"instancedBatches\":" << scene.InstancedBatches
				<< ",\"instancedObjects\":" << scene.InstancedObjects
				<< ",\"sceneMs\":" << scene.SceneMilliseconds
				<< ",\"cullMs\":" << scene.CullMilliseconds
				<< ",\"gpuMs\":" << scene.GpuMilliseconds
				<< ",\"fps\":" << m_WuiContext.Input().FPS << "}";
			result = out.str();
			return true;
		}
		if (cmd == "log.tail")
		{
			const size_t count = args.count("count") ? static_cast<size_t>(std::strtoul(arg("count").c_str(), nullptr, 10)) : 40;
			result = JoinLines(Log::RecentLines(count ? count : 40));
			return true;
		}
		if (cmd == "ops.tail")
		{
			const size_t count = args.count("count") ? static_cast<size_t>(std::strtoul(arg("count").c_str(), nullptr, 10)) : 40;
			const std::vector<Wui::WuiOpRecord>& records = m_WuiContext.Ops().Records();
			const size_t begin = records.size() > count ? records.size() - count : 0;
			std::vector<std::string> lines;
			for (size_t i = begin; i < records.size(); ++i)
			{
				const Wui::WuiOpRecord& record = records[i];
				lines.push_back("#" + std::to_string(record.Seq) + " f" + std::to_string(record.Frame) + " "
					+ record.Category + "/" + record.Action + " " + record.Target + " " + record.Detail);
			}
			result = JoinLines(lines);
			return true;
		}
		if (cmd == "ui.tree")
		{
			result = Wui::WuiAccessibility::Get().Serialize();
			return true;
		}
		// U26:窗口几何(客户区屏幕原点 + 尺寸,物理像素),键 = ui.tree 节点的 window 字段。
		// 跨窗口自动化(把 a11y 矩形换算成屏幕点再注入)必须先有这份权威数据:
		// 脚本自己 EnumWindows 时,进程里还混着 GLFW 消息窗、驱动的 pbuffer 窗口等,
		// 按"客户区最大的 GLFW 窗口"猜角色实测会算错注入点。
		if (cmd == "ui.windows")
		{
			result = m_Shell.AiWindowRectsJson();
			return true;
		}
		if (cmd == "ui.invoke")
		{
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			const glm::vec2 center { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
			// D10:可选 button(0=左键默认 / 1=右键 / 2=中键)—— 右键用于脚本化开上下文菜单
			// (树行右键菜单等);树行现在是注册过的无障碍节点,可以按 id 直接点。
			int button = 0;
			if (args.count("button"))
				button = std::atoi(arg("button").c_str());
			if (button < 0 || button > 2)
				button = 0;
			Wui::WuiScriptedInput::Get().QueueClick(node->Window, center, button);
			result = "queued " + node->Kind + " '" + node->Label + "' at ("
				+ std::to_string(static_cast<int>(center.x)) + "," + std::to_string(static_cast<int>(center.y))
				+ ") window=" + node->Window;
			return true;
		}
		// P4-U7:滚轮注入。滚动区/列表"滚不动"这类问题只能靠滚轮复现(键盘与拖动滚动条
		// 是另外两条路径,不能互相证明),所以通道必须能发滚轮。用法二选一:
		//   {"cmd":"ui.wheel","id":<节点id>,"delta":1}      —— 坐标取该无障碍节点中心
		//   {"cmd":"ui.wheel","window":"main","x":640,"y":400,"delta":2}
		if (cmd == "ui.wheel")
		{
			std::string windowKey = args.count("window") ? arg("window") : "main";
			glm::vec2 position { 0.0f, 0.0f };
			if (args.count("x") || args.count("y"))
			{
				position.x = args.count("x") ? std::strtof(arg("x").c_str(), nullptr) : 0.0f;
				position.y = args.count("y") ? std::strtof(arg("y").c_str(), nullptr) : 0.0f;
			}
			else
			{
				std::string message;
				const Wui::WuiAccessNode* node = resolveNode(message);
				if (!node)
				{
					error = message.empty() ? "ui.wheel needs id/label or x/y" : message;
					return false;
				}
				position = { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
				windowKey = node->Window;
			}
			const float delta = args.count("delta") ? std::strtof(arg("delta").c_str(), nullptr) : 1.0f;
			Wui::WuiScriptedInput::Get().QueueWheel(windowKey, position, delta);
			result = "queued wheel " + std::to_string(delta) + " at ("
				+ std::to_string(static_cast<int>(position.x)) + "," + std::to_string(static_cast<int>(position.y))
				+ ") window=" + windowKey;
			return true;
		}
		// ---- U23:真拖拽注入(跨帧按住 + 位移)----
		//   {"cmd":"ui.mouse.press","window":"main"|"float:<面板>","x":..,"y":..,"button":"left|right|middle"}
		//   {"cmd":"ui.mouse.move","x":..,"y":..}
		//   {"cmd":"ui.mouse.release","x":..,"y":..,"button":"left"}
		//
		// x/y = **设计单位**(与 ui.invoke / 无障碍节点 rect 同一坐标系,窗口客户区原点)。
		// window 默认 main;附加到主窗口的面板也是 main(它们渲染在主窗口里),只有拖成
		// 独立 OS 窗口才写 float:<面板 id>。注入的按下是"虚拟按键"——按下那一刻系统按键
		// 是抬起的,所以 WuiInputCollector 不会在下一帧用 GetAsyncKeyState 把它判成抬起,
		// 跨帧按住因此成立(真拖拽);release 或 5 秒超时收口。
		// 注意:一帧内 Pump 会处理完队列里的全部命令 —— 脚本侧每步之间至少隔一帧发送,
		// 否则几步位移会落在同一帧里(只会产生一次位移量)。
		if (cmd == "ui.mouse.press" || cmd == "ui.mouse.move" || cmd == "ui.mouse.release")
		{
			ScriptedMouseState& mouse = ScriptedMouse();
			const std::string action = cmd.substr(std::string("ui.mouse.").size());
			const std::string windowKey = args.count("window") ? arg("window")
				: (mouse.Window.empty() ? std::string("main") : mouse.Window);
			std::string resolveError;
			HWND hwnd = ResolveInjectionWindow(windowKey, arg("title"), &resolveError);
			if (!hwnd)
			{
				error = resolveError;
				return false;
			}
			const bool anyHeld = mouse.Held[0] || mouse.Held[1] || mouse.Held[2];
			if (anyHeld && (mouse.Window != windowKey || mouse.Handle != hwnd))
			{
				error = "another button is still held on window '" + mouse.Window
					+ "'; release it before injecting into '" + windowKey + "'";
				return false;
			}
			int button = 0;
			if (!ParseMouseButton(arg("button"), mouse.LastButton, &button, &error))
				return false;
			glm::vec2 position = mouse.Last;
			const bool hasCoords = args.count("x") || args.count("y");
			if (hasCoords)
			{
				if (args.count("x")) position.x = std::strtof(arg("x").c_str(), nullptr);
				if (args.count("y")) position.y = std::strtof(arg("y").c_str(), nullptr);
			}
			if (action != "release" && !hasCoords)
			{
				error = "ui.mouse." + action + " needs x/y (design units, window client coords)";
				return false;
			}
			const glm::vec2 physical = ToPhysicalPixels(position);
			const std::string where = "(" + std::to_string(static_cast<int>(position.x)) + ","
				+ std::to_string(static_cast<int>(position.y)) + ") design ≈ ("
				+ std::to_string(static_cast<int>(physical.x)) + ","
				+ std::to_string(static_cast<int>(physical.y)) + ") px window=" + windowKey;
			if (action == "press")
			{
				// 同一帧里先移动再按下(与 ui.invoke 的"先悬停再点击"同节拍):悬停在按下那一帧
				// 就位,面板的 MouseClicked 分支才会把这次按下当成拖拽起点。
				if (!PostMouseMessage(hwnd, WM_MOUSEMOVE, ButtonMaskFor(mouse), physical, &error))
					return false;
				const WPARAM ownMask = button == 1 ? MK_RBUTTON : (button == 2 ? MK_MBUTTON : MK_LBUTTON);
				const UINT message = button == 1 ? WM_RBUTTONDOWN : (button == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN);
				if (!PostMouseMessage(hwnd, message, ButtonMaskFor(mouse) | ownMask, physical, &error))
					return false;
				mouse.Held[button] = true;
				mouse.LastButton = button;
				mouse.Last = position;
				mouse.Window = windowKey;
				mouse.Handle = hwnd;
				result = "pressed " + std::string(ButtonName(button)) + " at " + where;
				return true;
			}
			if (action == "move")
			{
				// 没有按住任何键时 = 纯悬停移动(与真实鼠标移动一致)。拖拽脚本先来一次
				// 悬停移动、下一帧再 press,能让"上一帧鼠标位置"这类宿主状态先就位。
				if (!PostMouseMessage(hwnd, WM_MOUSEMOVE, ButtonMaskFor(mouse), physical, &error))
					return false;
				mouse.Last = position;
				result = "moved to " + where + (anyHeld ? " (button held)" : " (hover)");
				return true;
			}
			if (!mouse.Held[button])
			{
				// 幂等:重复 release(脚本收尾路径)不报错,但明确告诉调用方什么都没按。
				result = std::string(ButtonName(button)) + " is not held (no-op) at " + where;
				return true;
			}
			mouse.Held[button] = false;
			const UINT message = button == 1 ? WM_RBUTTONUP : (button == 2 ? WM_MBUTTONUP : WM_LBUTTONUP);
			// VEC-H4:抬起前**先把指针同步挪到释放点**。脚本若只发了 press + release(没发 move),
			// 这一步仍会让控件在按住期间看到最终位移 ⇒ 拖动成立,而不是退化成单击。
			if (!PostMouseMessage(hwnd, WM_MOUSEMOVE, ButtonMaskFor(mouse) | ButtonMaskOf(button), physical,
				&error))
				return false;
			if (!PostMouseMessage(hwnd, message, ButtonMaskFor(mouse), physical, &error))
				return false;
			mouse.Last = position;
			result = "released " + std::string(ButtonName(button)) + " at " + where;
			if (!(mouse.Held[0] || mouse.Held[1] || mouse.Held[2]))
			{
				mouse.Window.clear();
				mouse.Handle = nullptr;
			}
			return true;
		}
		// ---- W9-3:向文本控件注入真实键入(点击聚焦 → 下一帧起逐帧写入字符)----
		if (cmd == "ui.type")
		{
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			if (node->Kind != "editor" && node->Kind != "text-field")
			{
				error = "node is not a text input: " + node->Kind;
				return false;
			}
			const std::string text = arg("text");
			if (text.empty())
			{
				error = "ui.type needs text";
				return false;
			}
			const glm::vec2 center { node->Rect.X + node->Rect.W * 0.5f, node->Rect.Y + node->Rect.H * 0.5f };
			// 聚焦点击复用既有 click 队列(与 ui.invoke 同一个节拍);文本挂在它后面:
			// 点击完成后的下一帧开始注入,走 WuiCodeEditor/TextField 的真实输入路径。
			Wui::WuiScriptedInput::Get().QueueClick(node->Window, center);
			Wui::WuiScriptedInput::Get().QueueType(node->Window, text);
			result = "queued type " + std::to_string(Utf8CodepointCount(text)) + " chars into "
				+ node->Kind + " '" + node->Label + "' window=" + node->Window;
			return true;
		}
		// ---- W9.8:注入一次按键(方向键/Enter/Tab/Esc 等),用于复现键路与自动化 ----
		if (cmd == "ui.key")
		{
			std::string message;
			const Wui::WuiAccessNode* node = resolveNode(message);
			if (!node)
			{
				error = message;
				return false;
			}
			const std::string keyName = arg("key");
			uint32_t keyCode = 0;
			if (keyName == "Up") keyCode = KeyCodes::Up;
			else if (keyName == "Down") keyCode = KeyCodes::Down;
			else if (keyName == "Left") keyCode = KeyCodes::Left;
			else if (keyName == "Right") keyCode = KeyCodes::Right;
			else if (keyName == "Home") keyCode = KeyCodes::Home;
			else if (keyName == "End") keyCode = KeyCodes::End;
			else if (keyName == "PageUp") keyCode = KeyCodes::PageUp;
			else if (keyName == "PageDown") keyCode = KeyCodes::PageDown;
			else if (keyName == "Enter") keyCode = KeyCodes::Enter;
			else if (keyName == "Tab") keyCode = KeyCodes::Tab;
			else if (keyName == "Escape") keyCode = KeyCodes::Escape;
			else if (keyName == "Backspace") keyCode = KeyCodes::Backspace;
			else if (keyName == "Delete") keyCode = KeyCodes::Delete;
			else if (keyName == "Space") keyCode = KeyCodes::Space;
			else if (keyName.size() == 1 && keyName[0] >= 'A' && keyName[0] <= 'Z')
				keyCode = static_cast<uint32_t>(KeyCodes::A) + static_cast<uint32_t>(keyName[0] - 'A');
			else if (keyName.size() == 1 && keyName[0] >= '0' && keyName[0] <= '9')
				keyCode = static_cast<uint32_t>(KeyCodes::D0) + static_cast<uint32_t>(keyName[0] - '0');
			else
			{
				error = "unknown key: " + keyName + " (Up/Down/Left/Right/Home/End/PageUp/PageDown/Enter/Tab/Escape/Backspace/Delete/Space/A-Z/0-9)";
				return false;
			}
			// P4-UX16:可选 ctrl=/shift=(1/true/yes)→ 注入组合键。真实键盘的 Ctrl 无法通过
			// keybd_event 进到 WUI 输入状态,组合键只能走这里(否则"框内 Ctrl+A"无法脚本复现)。
			// 可选 frames=N(≥0)→ 按下后**继续按住 N 帧**再释放:真人敲一次键横跨好几帧
			// (60fps 下 80ms ≈ 5 帧),默认的"只按 1 帧"会漏掉"按住连触发"类缺陷。
			const auto flag = [&args](const char* name)
			{
				if (!args.count(name))
					return false;
				const std::string value = args.at(name);
				return value == "1" || value == "true" || value == "yes";
			};
			const bool ctrl = flag("ctrl");
			const bool shift = flag("shift");
			const int holdFrames = args.count("frames") ? std::max(0, std::atoi(arg("frames").c_str())) : 0;
			Wui::WuiScriptedInput::Get().QueueKey(node->Window, keyCode, ctrl, shift, holdFrames);
			result = "queued key " + keyName + (ctrl ? " +Ctrl" : "") + (shift ? " +Shift" : "")
				+ (holdFrames > 0 ? (" (held " + std::to_string(holdFrames) + " frames)") : "")
				+ " to " + node->Kind + " '" + node->Label + "' window=" + node->Window;
			return true;
		}
		if (cmd == "ui.open" || cmd == "ui.toggle" || cmd == "ui.close")
		{
			const std::string panel = normalizePanel(arg("panel"));
			if (panel.empty())
			{
				error = "missing panel";
				return false;
			}
			if (!m_Shell.AiTogglePanel(panel))
			{
				error = "cannot toggle panel '" + panel + "' (undeclared or no UI context yet)";
				return false;
			}
			result = "toggled " + panel;
			return true;
		}
		// ---- P3-1②:脚本化的"分离 / 挂回"(独立窗口形态的面板)----
		// 与顶部挂靠栏拖拽、窗口菜单走同一对既有路径(OpenIndependentPanel /
		// AttachIndependentWindowToSlot);已处于目标状态时幂等,message 即结果文案。
		if (cmd == "ui.detach" || cmd == "ui.attach")
		{
			const std::string panel = normalizePanel(arg("panel"));
			if (panel.empty())
			{
				error = "missing panel";
				return false;
			}
			std::string message;
			const bool ok = cmd == "ui.detach"
				? m_Shell.AiDetachPanel(panel, &message)
				: m_Shell.AiAttachPanel(panel, &message);
			if (!ok)
			{
				error = message.empty() ? ("cannot " + cmd + " panel '" + panel + "'") : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "ui.focus")
		{
			const std::string panel = normalizePanel(arg("panel"));
			m_Shell.FocusIndependentWindow(panel);
			result = "focused " + panel;
			return true;
		}
		// P4-UX15:把**停靠面板**切到前台(选中它所在的标签页)。此前没有稳定入口,
		// 后台标签不渲染 → 树里没有它的节点,内容浏览器一类停靠面板无法被脚本驱动。
		if (cmd == "ui.activate")
		{
			const std::string panel = normalizePanel(arg("panel"));
			std::string message;
			if (!m_Shell.AiActivatePanel(panel, &message))
			{
				error = message.empty() ? ("cannot activate panel '" + panel + "'") : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "ui.resize")
		{
			const std::string panel = normalizePanel(arg("panel"));
			const float width = static_cast<float>(std::atof(arg("width", "0").c_str()));
			const float height = static_cast<float>(std::atof(arg("height", "0").c_str()));
			if (panel.empty() || width <= 0.0f || height <= 0.0f)
			{
				error = "ui.resize needs panel, width, height";
				return false;
			}
			if (!m_Shell.AiResizeWindow(panel, width, height))
			{
				error = "no visible independent window for panel '" + panel + "'";
				return false;
			}
			result = "resized " + panel + " to " + std::to_string(static_cast<int>(width)) + "x"
				+ std::to_string(static_cast<int>(height));
			return true;
		}
		if (cmd == "ui.layout.get")
		{
			result = m_Shell.AiLayoutJson();
			return true;
		}
		if (cmd == "capture.screen")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			// 只登记请求:引擎会在"UI 通道已提交、EndFramePresent 之前"执行抓取
			// (见 Renderer::FlushPresentCaptures 的说明 —— 提前抓会拍到空白并破坏后续布局)。
			Renderer::RequestPresentCapture(nullptr, path,
				Application::Get().GetWindow().GetWidth(), Application::Get().GetWindow().GetHeight());
			result = "queued (written at end of frame): " + path.string();
			return true;
		}
		if (cmd == "capture.scene")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			if (!m_SceneRenderer)
			{
				error = "no scene renderer";
				return false;
			}
			m_SceneRenderer->CaptureFrame(path);
			result = path.string();
			return true;
		}
		if (cmd == "capture.float")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			const std::string panel = normalizePanel(arg("panel"));
			if (!m_Shell.AiRequestFloatCapture(panel, path.string()))
			{
				error = "no visible independent window for panel '" + panel + "'";
				return false;
			}
			result = "queued (written next frame): " + path.string();
			return true;
		}
		if (cmd == "capture.texture")
		{
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			const std::string kind = arg("kind", "material-preview");
			if (kind != "material-preview")
			{
				error = "unsupported texture kind '" + kind + "' (supported: material-preview)";
				return false;
			}
			const std::string panel = normalizePanel(arg("panel"));
			if (panel.empty())
			{
				error = "missing panel (e.g. material:materials/glass_red.wmat)";
				return false;
			}
			if (!m_Shell.AiRequestPreviewCapture(panel, path.string()))
			{
				error = "panel '" + panel + "' has no material preview";
				return false;
			}
			result = "queued (written next frame): " + path.string();
			return true;
		}
		if (cmd == "quit")
		{
			result = "closing";
			Application::Get().Close();
			return true;
		}
		// ---- 场景(实体树/选中/属性/Play) ----
		if (cmd == "scene.list")
		{
			std::ostringstream out;
			out << "[";
			bool first = true;
			if (m_ActiveScene)
			{
				const Scene& scene = *m_ActiveScene;
				const entt::registry& registry = scene.GetRegistry();
				for (auto handle : registry.view<TagComponent>())
				{
					if (!first)
						out << ",";
					first = false;
					out << "{\"handle\":" << static_cast<uint32_t>(handle)
						<< ",\"name\":\"" << JsonEscape(registry.get<TagComponent>(handle).Tag) << "\""
						<< ",\"selected\":" << (handle == static_cast<entt::entity>(m_SelectedEntity) ? "true" : "false")
						<< "}";
				}
			}
			out << "]";
			result = out.str();
			return true;
		}
		if (cmd == "scene.select")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			Entity target;
			if (!arg("handle").empty())
			{
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			}
			else if (!arg("name").empty())
			{
				auto& registry = m_ActiveScene->GetRegistry();
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=<id> or name=<tag>); call scene.list first";
				return false;
			}
			m_Shell.SetSelectedEntity(target);
			m_SelectedEntity = target;
			result = "selected handle=" + std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(target)));
			return true;
		}
		if (cmd == "scene.get")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			Entity target = m_SelectedEntity;
			if (!arg("handle").empty())
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			else if (!arg("name").empty())
			{
				target = Entity {};
				const Scene& scene = *m_ActiveScene;
				const entt::registry& registry = scene.GetRegistry();
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=/name=, or select one first)";
				return false;
			}
			const Scene& sceneRef = *m_ActiveScene;
			const entt::registry& registry = sceneRef.GetRegistry();
			const entt::entity handle = static_cast<entt::entity>(target);
			std::ostringstream out;
			out << "{\"handle\":" << static_cast<uint32_t>(handle);
			if (const auto* tag = registry.try_get<TagComponent>(handle))
				out << ",\"name\":\"" << JsonEscape(tag->Tag) << "\"";
			if (const auto* transform = registry.try_get<TransformComponent>(handle))
			{
				const glm::vec3 location = transform->Location;
				const glm::vec3 rotation = transform->Rotation;
				const glm::vec3 scale = transform->Scale;
				out << ",\"location\":[" << location.x << "," << location.y << "," << location.z << "]"
					<< ",\"rotation\":[" << rotation.x << "," << rotation.y << "," << rotation.z << "]"
					<< ",\"scale\":[" << scale.x << "," << scale.y << "," << scale.z << "]";
			}
			if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
				out << ",\"primitive\":\"" << JsonEscape(mesh->Primitive) << "\""
					<< ",\"material\":\"" << JsonEscape(mesh->MaterialPath) << "\""
					<< ",\"color\":[" << mesh->Color.r << "," << mesh->Color.g << ","
					<< mesh->Color.b << "," << mesh->Color.a << "]";
			// PLUG-CLEAN-1:任意 schema 组件(含插件组件 blob)的**字段读取**。
			// 不带 component= ⇒ 上面的内建输出逐字节不变(回归口径)。
			if (!arg("component").empty())
			{
				const std::string componentName = arg("component");
				const Schema::SchemaRegistry& schemas = sceneRef.GetContext().Schemas();
				const Schema::TypeSchema* schema = FindComponentSchema(schemas, componentName);
				if (!schema)
				{
					error = "no component type '" + componentName
						+ "' (use the full type id, its short name, or its display name)";
					return false;
				}
				if (!schema->Storage)
				{
					error = "component '" + schema->Id.Name
						+ "' is schema-only (no storage) and cannot live on an entity";
					return false;
				}
				const auto* storage = registry.storage(schema->Storage->ComponentId);
				const void* instance = storage && storage->contains(handle)
					? storage->value(handle) : nullptr;
				if (!instance)
				{
					error = "entity does not have component '" + componentName + "'";
					return false;
				}
				const std::string fieldName = arg("property");
				out << ",\"component\":{\"type\":\"" << JsonEscape(schema->Id.Name) << "\"";
				if (fieldName.empty())
				{
					out << ",\"fields\":{";
					bool firstField = true;
					for (const Schema::FieldSchema& field : schema->Fields)
					{
						const Schema::Value value = Schema::ReadSchemaField(field, instance);
						if (std::holds_alternative<std::monostate>(value))
							continue;   // 未设(Transient / 缺访问器):不假装有值
						if (!firstField)
							out << ",";
						firstField = false;
						out << "\"" << JsonEscape(field.Name) << "\":" << SchemaValueToJson(value);
					}
					out << "}";
				}
				else
				{
					const Schema::FieldSchema* field = FindComponentField(*schema, fieldName);
					if (!field)
					{
						error = "component '" + schema->Id.Name + "' has no field '" + fieldName + "'";
						return false;
					}
					out << ",\"field\":\"" << JsonEscape(field->Name) << "\",\"value\":"
						<< SchemaValueToJson(Schema::ReadSchemaField(*field, instance));
				}
				out << "}";
			}
			out << "}";
			result = out.str();
			return true;
		}
		if (cmd == "scene.set")
		{
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			// Play/Simulate 是只读查看(与属性面板同一条规则):活动场景的结构写必须走命令提交,
			// 编辑器的 AI 通道不越权改写。
			if (m_SceneState != SceneState::Edit)
			{
				error = "scene is read-only in Play/Simulate; exit Play first";
				return false;
			}
			Entity target = m_SelectedEntity;
			if (!arg("handle").empty())
				target = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("handle").c_str(), nullptr, 10)));
			else if (!arg("name").empty())
			{
				auto& registry = m_ActiveScene->GetRegistry();
				target = Entity {};
				for (auto handle : registry.view<TagComponent>())
					if (registry.get<TagComponent>(handle).Tag == arg("name"))
					{
						target = Entity(m_ActiveScene.get(), handle);
						break;
					}
			}
			if (!target.IsValid())
			{
				error = "entity not found (need handle=/name=, or select one first)";
				return false;
			}
			const std::string property = arg("property");
			const std::string value = arg("value");
			auto& registry = m_ActiveScene->GetRegistry();
			const entt::entity handle = static_cast<entt::entity>(target);
			if (property == "Tag")
			{
				if (auto* tag = registry.try_get<TagComponent>(handle))
				{
					tag->Tag = value;
					MarkDocumentDirty();
					result = "Tag='" + value + "'";
					return true;
				}
			}
			if (auto* transform = registry.try_get<TransformComponent>(handle))
			{
				glm::vec3 vector { 0.0f };
				if (property == "Location" || property == "Rotation" || property == "Scale")
				{
					if (!ParseFloat3(value, &vector))
					{
						error = "value must be 'x,y,z'";
						return false;
					}
					if (property == "Location")
						transform->SetLocation(vector);
					else if (property == "Rotation")
						transform->SetRotation(vector);
					else
						transform->SetScale(vector);
					MarkDocumentDirty();
					result = property + "=(" + value + ")";
					return true;
				}
			}
			if (auto* mesh = registry.try_get<MeshRendererComponent>(handle))
			{
				if (property == "Material")
				{
					mesh->MaterialPath = value;
					MarkDocumentDirty();
					result = "Material='" + value + "'";
					return true;
				}
				if (property == "Primitive")
				{
					mesh->Primitive = value;
					MarkDocumentDirty();
					result = "Primitive='" + value + "'";
					return true;
				}
				if (property == "Color")
				{
					glm::vec3 rgb { 1.0f };
					if (!ParseFloat3(value, &rgb))
					{
						error = "value must be 'r,g,b'";
						return false;
					}
					mesh->Color = glm::vec4 { rgb, 1.0f };
					MarkDocumentDirty();
					result = "Color=(" + value + ")";
					return true;
				}
			}
			// PLUG-CLEAN-1:任意 schema 组件(含插件组件 blob)的**字段写入**。
			// 走 FieldSchema::Set 的宿主封装(WriteSchemaField),与属性面板/序列化同一条
			// 访问器;Play/Simulate 的只读规则在本命令开头已统一拒绝。
			if (!arg("component").empty())
			{
				const std::string componentName = arg("component");
				if (property.empty())
				{
					error = "scene.set with component=<type> needs property=<field> and value=<text>";
					return false;
				}
				Schema::SchemaRegistry& schemas = m_ActiveScene->GetContext().Schemas();
				const Schema::TypeSchema* schema = FindComponentSchema(schemas, componentName);
				if (!schema)
				{
					error = "no component type '" + componentName
						+ "' (use the full type id, its short name, or its display name)";
					return false;
				}
				if (!schema->Storage)
				{
					error = "component '" + schema->Id.Name
						+ "' is schema-only (no storage) and cannot live on an entity";
					return false;
				}
				auto* storage = registry.storage(schema->Storage->ComponentId);
				void* instance = storage && storage->contains(handle) ? storage->value(handle) : nullptr;
				if (!instance)
				{
					error = "entity does not have component '" + componentName
						+ "' (add it first, e.g. from the properties panel)";
					return false;
				}
				const Schema::FieldSchema* field = FindComponentField(*schema, property);
				if (!field)
				{
					error = "component '" + schema->Id.Name + "' has no field '" + property + "'";
					return false;
				}
				Schema::Value parsed;
				std::string parseError;
				if (!ParseSchemaFieldValue(*field, value, &parsed, &parseError))
				{
					error = parseError.empty()
						? ("field '" + field->Name + "' rejected the value") : parseError;
					return false;
				}
				if (!Schema::WriteSchemaField(*field, instance, parsed))
				{
					error = "field '" + field->Name + "' write rejected by '" + schema->Id.Name + "'";
					return false;
				}
				MarkDocumentDirty();
				result = "component " + schema->Id.Name + "." + field->Name + "=" + value;
				return true;
			}
			error = "unsupported property '" + property
				+ "' (Tag/Location/Rotation/Scale/Material/Primitive/Color; "
				"or pass component=<type> [property=<field>] for any schema component)";
			return false;
		}
		if (cmd == "scene.open")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			OpenScene(std::filesystem::path(path));
			result = "opened " + path;
			return true;
		}
		if (cmd == "scene.save")
		{
			// 只允许写到白名单路径(默认沿用当前文档路径)。
			if (arg("path").empty())
			{
				if (!SaveScene())
				{
					error = "save failed (no document path?)";
					return false;
				}
				result = "saved current document";
				return true;
			}
			std::string pathError;
			const std::filesystem::path path = ResolveCapturePath(arg("path"), &pathError);
			if (path.empty())
			{
				error = pathError;
				return false;
			}
			if (!m_Document.SaveTo(path))
			{
				error = "save failed: " + m_Document.GetLastError();
				return false;
			}
			result = "saved " + path.string();
			return true;
		}
		if (cmd == "play.enter" || cmd == "play.exit" || cmd == "play.pause" || cmd == "play.resume")
		{
			const bool playing = m_SceneState == SceneState::Play;
			if (cmd == "play.enter" && !playing)
				m_Shell.TogglePlay();
			else if (cmd == "play.exit" && playing)
				m_Shell.TogglePlay();
			else if (cmd == "play.pause" && playing && !m_ScenePaused)
				m_Shell.TogglePause();
			else if (cmd == "play.resume" && playing && m_ScenePaused)
				m_Shell.TogglePause();
			result = "playState=" + std::to_string(static_cast<int>(m_SceneState))
				+ " paused=" + (m_ScenePaused ? "true" : "false");
			return true;
		}
		// script.status / script.reload: 纯 ECS 下实体脚本组件已移除
		if (cmd == "script.status" || cmd == "script.reload")
		{
			error = "script.status and script.reload are deprecated in pure ECS (per-entity script components have been removed; use systems in scripts/systems/*.luau)";
			return false;
		}
		// ---- CPPT-3:Game 模块(`Game.dll`)级热重载(契约 = plan CPPT-2 §6.1 T5b)----
		//   module.status   当前状态(loaded|unloaded|reloading|rolled-back + 计数/诊断/ABI)
		//   module.unload   第一段:显式卸载(释放文件锁,Game.dll 可重编)
		//   module.reload   已加载 → 第一段卸载(进入 reloading);未加载 → 第二段加载新构建
		//                   (引擎内含 ABI 等值门;加载失败自动回滚到旧副本 → rolled-back)
		// 两段式是 Windows 文件锁的现实:Editor 映射着 Game.dll 时链接器无法改写该文件。
		// 人工作业顺序 = File ▸ Reload C++ Module(卸载)→ 重编 Game.dll → 再次激活(加载)。
		if (cmd == "module.status")
		{
			result = CppModuleStatusJson();
			return true;
		}
		if (cmd == "module.unload")
		{
			std::string message;
			const bool ok = UnloadCppModule(&message);
			result = CppModuleStatusJson();
			if (!ok)
			{
				error = message.empty() ? "module unload failed" : message;
				return false;
			}
			return true;
		}
		if (cmd == "module.reload")
		{
			std::string message;
			const bool ok = ReloadCppModule(&message);
			// 回滚(rolled-back)在命令层算"已执行":结果 JSON 里 ok=false + rolledBack=true,
			// 探针按字段断言,不吞掉结构化诊断。
			result = CppModuleStatusJson();
			if (!ok && !GetCppModuleStatus().RolledBack)
			{
				error = message.empty() ? "module reload failed" : message;
				return false;
			}
			return true;
		}
		// HOTR-P3-T7:`module.build_reload` = 卸载 + 后台构建项目 build.cmd + 成功后自动加载。
		// 命令异步:返回 ok=true 只代表"构建已启动",进度/结果轮询 `module.status`
		// (buildRunning / buildExitCode / buildOutput;菜单项走同一入口)。
		if (cmd == "module.build_reload")
		{
			std::string message;
			const bool ok = BuildAndReloadCppModule(&message);
			result = CppModuleStatusJson();
			if (!ok)
			{
				error = message.empty() ? "module build_reload failed" : message;
				return false;
			}
			return true;
		}
		// ---- PLUG-T3:插件管理器(与面板同一条数据/动作路径;E2:无项目 = "需要先打开项目")----
		//   plugin.list                     已发现条目 + 状态(JSON)
		//   plugin.info <id>                单条详情(JSON;依赖 / 能力 / 导出说明 / 诊断 / 根路径)
		//   plugin.set_enabled <id> <0|1>   引擎插件启停(写 local/plugins.json,下次启动生效)
		if (cmd == "plugin.list" || cmd == "plugin.info" || cmd == "plugin.set_enabled")
		{
			Plugins::PluginManager* plugins = m_Shell.GetPluginManager();
			if (!plugins)
			{
				// E2:插件管理器只在项目形态可用(启动器形态没有项目:面板不注册、命令给可读错误)。
				error = "需要先打开项目";
				return false;
			}
			const auto appendEntry = [this, plugins](std::ostringstream& out,
				const Plugins::PluginEntry& entry)
			{
				const bool disabled = m_Shell.IsPluginDisabled(entry.Manifest.Id);
				out << "{";
				out << "\"id\":\"" << JsonEscape(entry.Manifest.Id) << "\"";
				out << ",\"name\":\"" << JsonEscape(entry.Manifest.Name) << "\"";
				out << ",\"scope\":\"" << Plugins::PluginScopeName(entry.Manifest.Scope) << "\"";
				out << ",\"version\":\"" << JsonEscape(entry.Manifest.Version) << "\"";
				// 已加载 = 插件自报 ABI;未加载 = 清单声明值(诊断上可读,避免永远是 0)。
				out << ",\"abi\":" << (entry.PluginAbi != 0 ? entry.PluginAbi : entry.Manifest.Abi);
				out << ",\"state\":\"" << Plugins::PluginStateName(entry.State) << "\"";
				out << ",\"order\":" << entry.Order;
				out << ",\"ship\":\"" << Plugins::PluginShipPolicyName(entry.Manifest.Ship) << "\"";
				out << ",\"root\":\"" << JsonEscape(entry.Manifest.Root.generic_string()) << "\"";
				out << ",\"disabled\":" << (disabled ? "true" : "false");
				out << ",\"restartPending\":"
					<< (m_Shell.PluginRestartPending(entry.Manifest.Id) ? "true" : "false");
				out << ",\"diagnostic\":\"" << JsonEscape(entry.Diagnostic) << "\"";
				out << ",\"loadError\":\""
					<< JsonEscape(m_Shell.PluginLoadError(entry.Manifest.Id)) << "\"";
				// PLUG-CLEAN-1:清单 engine: 最低版本约束是否被宿主满足(宿主版本见 plugin.info
				// 的 hostEngineVersion;空清单字段 = true)。
				out << ",\"engineSatisfied\":" << (entry.EngineSatisfied ? "true" : "false");
				out << ",\"pendingReload\":"
					<< (plugins->HasPendingReload(entry.Manifest.Id) ? "true" : "false");
				out << "}";
			};
			if (cmd == "plugin.list")
			{
				const std::vector<Plugins::PluginEntry> entries = plugins->Entries();
				size_t loaded = 0;
				size_t rejected = 0;
				for (const Plugins::PluginEntry& entry : entries)
				{
					if (entry.State == Plugins::PluginState::Loaded)
						++loaded;
					if (entry.State == Plugins::PluginState::Rejected)
						++rejected;
				}
				std::ostringstream out;
				out << "{\"count\":" << entries.size() << ",\"loaded\":" << loaded
					<< ",\"rejected\":" << rejected << ",\"plugins\":[";
				for (size_t index = 0; index < entries.size(); ++index)
				{
					if (index > 0)
						out << ",";
					appendEntry(out, entries[index]);
				}
				out << "]}";
				result = out.str();
				return true;
			}
			if (cmd == "plugin.info")
			{
				const std::string id = arg("id");
				if (id.empty())
				{
					error = "plugin.info needs id";
					return false;
				}
				const Plugins::PluginEntry* entry = plugins->Find(id);
				if (!entry)
				{
					error = "no plugin with id '" + id + "'";
					return false;
				}
				std::ostringstream out;
				out << "{\"plugin\":";
				appendEntry(out, *entry);
				out << ",\"publisher\":\"" << JsonEscape(entry->Manifest.Publisher) << "\"";
				out << ",\"entry\":\"" << JsonEscape(entry->Manifest.Entry) << "\"";
				out << ",\"engine\":\"" << JsonEscape(entry->Manifest.Engine) << "\"";
				// PLUG-CLEAN-1:宿主引擎版本(单一事实源 = 根 CMakeLists 的 project VERSION)。
				out << ",\"hostEngineVersion\":\"" << JsonEscape(Plugins::HostEngineVersion()) << "\"";
				out << ",\"manifest\":\""
					<< JsonEscape(entry->Manifest.ManifestPath.generic_string()) << "\"";
				out << ",\"depends\":[";
				for (size_t index = 0; index < entry->Manifest.Depends.size(); ++index)
					out << (index ? "," : "") << "\"" << JsonEscape(entry->Manifest.Depends[index]) << "\"";
				out << "],\"provides\":[";
				for (size_t index = 0; index < entry->Manifest.Provides.size(); ++index)
					out << (index ? "," : "") << "\"" << JsonEscape(entry->Manifest.Provides[index]) << "\"";
				out << "],\"overrides\":[";
				for (size_t index = 0; index < entry->Manifest.Overrides.size(); ++index)
					out << (index ? "," : "") << "\"" << JsonEscape(entry->Manifest.Overrides[index]) << "\"";
				out << "],\"exports\":[";
				for (size_t index = 0; index < entry->ExportNames.size(); ++index)
					out << (index ? "," : "") << "\"" << JsonEscape(entry->ExportNames[index]) << "\"";
				out << "]";
				// PLUG-T6:宿主侧注册账本(卸载后必须全 0)+ 活实例 + 未完成的重载快照状态。
				const Plugins::PluginLedgerCounts ledgers = plugins->LedgerCounts(id);
				out << ",\"ledgers\":{\"schema_types\":" << ledgers.Components
					<< ",\"asset_types\":" << ledgers.AssetTypes
					<< ",\"importers\":" << ledgers.Importers
					<< ",\"editor_commands\":" << ledgers.EditorCommands
					<< ",\"editor_panels\":" << ledgers.EditorPanels
					<< ",\"script_functions\":" << ledgers.ScriptFunctions
					<< ",\"total\":" << ledgers.Total() << "}";
				out << ",\"liveInstances\":"
					<< plugins->LiveComponentInstances(id, &Application::Get().GetContext());
				out << ",\"hasPendingReload\":"
					<< (plugins->HasPendingReload(id) ? "true" : "false");
				out << ",\"loadedLibrary\":\""
					<< JsonEscape(plugins->LoadedLibraryPath(id)) << "\"";
				// HOTR-P3-T8:一键重载(后台构建插件 CMake 目标)的进度与最近一次结果 ——
				// 与插件面板「重新加载」按钮的置灰/状态行同一数据源(PluginManager)。
				const Plugins::PluginManager::PluginBuildProgress progress =
					plugins->PluginBuildState(id);
				out << ",\"building\":" << (progress.Running ? "true" : "false")
					<< ",\"buildExitCode\":" << progress.ExitCode
					<< ",\"buildOutput\":\"" << JsonEscape(progress.Output) << "\"";
				out << "}";
				result = out.str();
				return true;
			}
			// plugin.set_enabled
			const std::string id = arg("id");
			const std::string enabledText = arg("enabled");
			if (id.empty() || (enabledText != "0" && enabledText != "1"))
			{
				error = "plugin.set_enabled needs id and 0|1";
				return false;
			}
			std::string message;
			if (!m_Shell.SetPluginEnabled(id, enabledText == "1", &message))
			{
				error = message.empty() ? "plugin.set_enabled failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		// ---- PLUG-T6:插件卸载 / 两段式热重载 ------------------------------------------
		//   plugin.unload <id>   卸载单个插件(T2c:有活组件实例 = 干净拒绝 + 可读原因;账本归零)
		//   plugin.reload <id>   两段式:L(oaded) → 第一段(快照 + 卸载,释放 DLL 锁);
		//                        未加载(+pending) → 第二段(载入新 DLL + 写回快照;失败回滚)
		//   plugin.reload <id> build=1
		//                        HOTR-P3-T8 一键:第一段(快照+卸载)→ 后台构建插件 CMake 目标 →
		//                        构建成功后自动第二段;进度/输出见 plugin.info 的
		//                        building/buildExitCode/buildOutput。
		// 结果 JSON 字段 = 探针断言点(phase / rolledBack / instancesSnapshotted / instancesRestored)。
		if (cmd == "plugin.unload" || cmd == "plugin.reload")
		{
			const std::string id = arg("id");
			if (id.empty())
			{
				error = cmd + " needs id";
				return false;
			}
			Plugins::PluginManager* plugins = m_Shell.GetPluginManager();
			if (!plugins)
			{
				error = "需要先打开项目";
				return false;
			}
			if (cmd == "plugin.unload")
			{
				std::string message;
				if (!UnloadPlugin(id, &message))
				{
					error = message.empty() ? "plugin.unload failed" : message;
					return false;
				}
				std::ostringstream out;
				out << "{\"id\":\"" << JsonEscape(id) << "\",\"state\":\"unloaded\""
					<< ",\"loaded\":" << plugins->LoadedCount() << "}";
				result = out.str();
				return true;
			}
			// HOTR-P3-T8:`plugin.reload <id> build=1` = 一键重载(快照+卸载 → 后台构建插件
			// CMake 目标 → 构建成功后自动加载);进度/结果从 plugin.info 的
			// building/buildExitCode/buildOutput 读。不带 build 的旧命令仍是两段式,语义不变。
			if (arg("build") == "1")
			{
				std::string message;
				const bool started = StartPluginBuildAndReload(id, &message);
				std::ostringstream out;
				out << "{\"id\":\"" << JsonEscape(id) << "\""
					<< ",\"building\":" << (started ? "true" : "false")
					<< ",\"message\":\"" << JsonEscape(message) << "\"}";
				result = out.str();
				if (!started)
				{
					error = message.empty() ? "plugin.reload build=1 failed" : message;
					return false;
				}
				return true;
			}
			// plugin.reload:返回结构化阶段结果;回滚(rolledBack)= "已执行但本次重载失败",
			// 与 module.reload 同口径——探针按 JSON 字段断言,不靠命令层 exit。
			Plugins::PluginReloadResult reload;
			std::string message;
			const bool ok = ReloadPlugin(id, &reload, &message);
			std::ostringstream out;
			out << "{\"id\":\"" << JsonEscape(id) << "\""
				<< ",\"phase\":\"" << Plugins::PluginReloadPhaseName(reload.ResultPhase) << "\""
				<< ",\"ok\":" << (reload.Ok ? "true" : "false")
				<< ",\"rolledBack\":" << (reload.RolledBack ? "true" : "false")
				<< ",\"unloaded\":" << (reload.Unloaded ? "true" : "false")
				<< ",\"instancesSnapshotted\":" << reload.InstancesSnapshotted
				<< ",\"instancesRestored\":" << reload.InstancesRestored
				<< ",\"instancesSkipped\":" << reload.InstancesSkipped
				<< ",\"message\":\"" << JsonEscape(reload.Message) << "\""
				<< ",\"loadedLibrary\":\"" << JsonEscape(reload.LibraryPath) << "\""
				<< ",\"rollbackPath\":\"" << JsonEscape(reload.RollbackPath) << "\""
				<< ",\"diagnostics\":[";
			for (size_t index = 0; index < reload.Diagnostics.size(); ++index)
				out << (index ? "," : "") << "\"" << JsonEscape(reload.Diagnostics[index]) << "\"";
			out << "]}";
			result = out.str();
			if (!ok && !reload.RolledBack)
			{
				error = message.empty() ? "plugin.reload failed" : message;
				return false;
			}
			return true;
		}
		// ---- PLUG-T3b:插件贡献的编辑器命令(命令面 = 面板按钮同一条执行路径)----
		//   plugin.commands                     已注册的插件命令 + 执行次数(JSON)
		//   plugin.command.run <id>             触发一条(名字 = plugin.command.<pluginId>.<id>,
		//                                       也接受裸 <id>,唯一命中才执行)
		if (cmd == "plugin.commands")
		{
			Plugins::PluginManager* plugins = m_Shell.GetPluginManager();
			if (!plugins)
			{
				error = "需要先打开项目";
				return false;
			}
			const std::vector<Plugins::PluginEditorCommand> commands = plugins->EditorCommands();
			std::ostringstream out;
			out << "{\"count\":" << commands.size() << ",\"commands\":[";
			for (size_t index = 0; index < commands.size(); ++index)
			{
				if (index > 0)
					out << ",";
				out << "{\"plugin\":\"" << JsonEscape(commands[index].PluginId) << "\""
					<< ",\"id\":\"" << JsonEscape(commands[index].Id) << "\""
					<< ",\"name\":\"" << JsonEscape(commands[index].CommandName) << "\""
					<< ",\"label\":\"" << JsonEscape(commands[index].Label) << "\""
					<< ",\"tooltip\":\"" << JsonEscape(commands[index].Tooltip) << "\""
					<< ",\"invokes\":" << commands[index].InvokeCount << "}";
			}
			out << "]}";
			result = out.str();
			return true;
		}
		if (cmd == "plugin.command.run")
		{
			const std::string id = arg("id");
			if (id.empty())
			{
				error = "plugin.command.run needs id";
				return false;
			}
			Plugins::PluginManager* plugins = m_Shell.GetPluginManager();
			if (!plugins)
			{
				error = "需要先打开项目";
				return false;
			}
			std::string message;
			if (!m_Shell.InvokePluginEditorCommand(id, &message))
			{
				error = message.empty() ? ("plugin.command.run failed: " + id) : message;
				return false;
			}
			result = message;
			return true;
		}
		// ---- W9-2:内置脚本编辑器 ----
		// 打开脚本编辑器(逻辑路径;每个脚本一个 "script:<逻辑路径>" 面板)。
		// 与用户入口(内容浏览器双击 .lua/.luau、Scripts 面板"在引擎内打开")同一条
		// EditorShell::OpenScriptEditor 路径 —— 默认直接附加到主窗口。
		if (cmd == "script.open_editor")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "script.open_editor needs path";
				return false;
			}
			m_Shell.OpenScriptEditor(path);
			result = "opened script editor for " + path;
			return true;
		}
		// ---- 资产 / 材质 ----
		if (cmd == "asset.open_material")
		{
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			m_Shell.OpenMaterialEditor(path);
			result = "opened material editor for " + path;
			return true;
		}
		if (cmd == "asset.import_gltf")
		{
			// P1b D5:与"内容浏览器双击 .gltf"同一条路径(只导入,不改场景),
			// 供自动化验证编辑器内的导入交互;相对路径按内容根解析。
			std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::filesystem::path resolved(path);
			if (!resolved.is_absolute())
				resolved = World::Paths::AssetRoot() / resolved;
			std::string message;
			std::string logicalModel;
			// D10:可选 dest = 目标逻辑目录(相对内容根,如 "models/props");空 = 源所在目录。
			const std::string destination = arg("dest");
			if (!ImportModelFile(resolved.string(), &message, &logicalModel, destination))
			{
				error = message.empty() ? "glTF import failed" : message;
				return false;
			}
			// 与内容浏览器双击同一条:导入后打开模型预览(只读,不改场景)。
			if (!logicalModel.empty())
				m_Shell.OpenModelPreview(logicalModel);
			result = "{\"wmodel\":\"" + JsonEscape(logicalModel) + "\",\"message\":\"" + JsonEscape(message) + "\"}";
			return true;
		}
		if (cmd == "asset.instance_model")
		{
			// P1b D5:与模型预览面板的"放进当前场景"同一条路径(仅编辑态)。
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::string message;
			if (!InstantiateModelFile(path, &message))
			{
				error = message.empty() ? "instantiate model failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "asset.open_prefab")
		{
			// P4-U13c:与"内容浏览器双击 .wprefab"同一条路径 —— 打开**资产窗口**
			// (看/管理:实体树 + 组件摘要 + 引用资产 + 场景实例),不改当前文档。
			// 读不了的资产:窗口照常打开(状态行写原因),命令本身返回可读失败。
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::string message;
			if (!m_Shell.OpenPrefabWindowChecked(path, &message))
			{
				error = message.empty() ? ("cannot open prefab window: " + path) : message;
				return false;
			}
			result = "opened prefab window for " + path;
			return true;
		}
		if (cmd == "asset.edit_prefab")
		{
			// P4-U13c:进入**文档编辑会话**(原来的 asset.open_prefab 行为)——
			// 顶部横幅标明"正在编辑 Prefab",保存 = 写回资产,返回 = 回原来的场景。
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			m_Shell.OpenPrefabEditor(path);
			result = "opened prefab editor for " + path;
			return true;
		}
		if (cmd == "asset.save_prefab")
		{
			// P4-U13:横幅上的"保存 Prefab"同一条路径(写回 .wprefab)。
			std::string message;
			if (!m_Shell.SavePrefabDocument(&message))
			{
				error = message.empty() ? "save prefab failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "asset.close_prefab")
		{
			// P4-U13:横幅上的"返回场景"同一条路径。
			if (!m_Shell.ClosePrefabDocument())
			{
				error = "not editing a prefab";
				return false;
			}
			result = "left prefab edit session";
			return true;
		}
		if (cmd == "asset.prefab_set_field")
		{
			// P4-U13e:给自动化脚本一条"写 prefab 资产窗口字段"的入口 —— 与窗口控件同一
			// 写入口(同样的脏标记 / 状态行 / 资产路径警告),不是旁路写数据。
			// 参数:path(逻辑路径)或 panel("prefab:<逻辑路径>")二选一;
			//       component / field / value,可选 axis(x/y/z,vec3 字段按分量写)。
			std::string panel = arg("panel");
			const std::string path = arg("path");
			if (panel.empty() && !path.empty())
			{
				std::string normalized = path;
				std::replace(normalized.begin(), normalized.end(), '\\', '/');
				panel = "prefab:" + normalized;
			}
			if (panel.empty())
			{
				error = "asset.prefab_set_field needs panel=<prefab:…> or path=<logical path>";
				return false;
			}
			const std::string component = arg("component");
			const std::string field = arg("field");
			if (component.empty() || field.empty())
			{
				error = "asset.prefab_set_field needs component and field";
				return false;
			}
			std::string message;
			if (!m_Shell.SetPrefabPanelField(panel, component, field, arg("value"), arg("axis"), &message))
			{
				error = message.empty() ? "prefab field write failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "asset.instance_prefab")
		{
			// P4-U13:与内容浏览器右键"实例化到当前场景"同一条路径。
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "missing path";
				return false;
			}
			std::string message;
			if (!m_Shell.InstantiatePrefabAsset(path, &message))
			{
				error = message.empty() ? "prefab instantiate failed" : message;
				return false;
			}
			result = message;
			return true;
		}
		if (cmd == "asset.create_prefab")
		{
			// P4-U13d:层级面板"Create Prefab from Selection…"的不弹窗版本(同一条内核)。
			// 参数:entity(场景实体 handle,缺省 = 当前选中)/ path(逻辑路径,**必须显式给出**;
			// 缺 .wprefab 自动补)/ overwrite(默认 false;目标已存在且没给 = ok:false + 可读原因)。
			if (!m_ActiveScene)
			{
				error = "no active scene";
				return false;
			}
			Entity root = m_SelectedEntity;
			if (!arg("entity").empty())
				root = Entity(m_ActiveScene.get(),
					static_cast<entt::entity>(std::strtoul(arg("entity").c_str(), nullptr, 10)));
			if (!root.IsValid() || root.GetScene() != m_ActiveScene.get())
			{
				error = "no entity to export (pass entity=<handle> or select one first)";
				return false;
			}
			const std::string path = arg("path");
			if (path.empty())
			{
				error = "asset.create_prefab needs path=<logical path> (e.g. prefabs/MyCube.wprefab)";
				return false;
			}
			const bool overwrite = arg("overwrite") == "1" || arg("overwrite") == "true";
			std::string message;
			PrefabCreateResult created;
			if (!CreatePrefabFromSelection(root, path, overwrite, &message, &created))
			{
				error = message.empty() ? "create prefab failed" : message;
				return false;
			}
			std::ostringstream out;
			out << "{\"path\":\"" << JsonEscape(created.LogicalPath) << "\""
				<< ",\"entities\":" << created.EntityCount
				<< ",\"overwrote\":" << (created.Overwrote ? "true" : "false") << "}";
			result = out.str();
			return true;
		}
		// HOTR-P1-T3:引擎内建 shader 热重载。与编辑器 watch 走**同一入口**
		// (Renderer::ReloadShaders);本函数在主线程帧内执行(AiControlServer::Pump 位于
		// OnUiFrame 开头,早于本帧渲染),失败保留旧管线并用 failed/error 回报。
		if (cmd == "renderer.reload_shaders")
		{
			const ShaderReloadResult reload = Renderer::ReloadShaders();
			std::ostringstream out;
			out << "{\"owners\":" << reload.Owners
				<< ",\"pipelines\":" << reload.Pipelines
				<< ",\"failed\":" << reload.Failed
				<< ",\"error\":\"" << JsonEscape(reload.Error) << "\"}";
			result = out.str();
			return true;
		}
		if (cmd == "material.get")
		{
			const std::string path = arg("path");
			const Ref<Material> material = path.empty() ? nullptr : MaterialLibrary::Get().Load(path);
			if (!material)
			{
				error = "material not found: " + path;
				return false;
			}
			const MaterialDesc& desc = material->GetDesc();
			std::ostringstream out;
			out << "{\"path\":\"" << JsonEscape(material->GetPath()) << "\""
				<< ",\"albedo\":\"" << JsonEscape(desc.AlbedoTexture) << "\""
				<< ",\"normal\":\"" << JsonEscape(desc.NormalTexture) << "\""
				<< ",\"baseColor\":[" << desc.BaseColor.r << "," << desc.BaseColor.g << ","
				<< desc.BaseColor.b << "," << desc.BaseColor.a << "]"
				<< ",\"metallic\":" << desc.Metallic << ",\"roughness\":" << desc.Roughness
				<< ",\"blendMode\":" << static_cast<int>(desc.BlendMode)
				<< ",\"doubleSided\":" << (desc.DoubleSided ? "true" : "false")
				<< ",\"revision\":" << material->GetRevision() << "}";
			result = out.str();
			return true;
		}
		if (cmd == "material.set")
		{
			const std::string path = arg("path");
			const Ref<Material> material = path.empty() ? nullptr : MaterialLibrary::Get().Load(path);
			if (!material)
			{
				error = "material not found: " + path;
				return false;
			}
			std::vector<std::string> applied;
			if (args.count("albedo")) { material->SetAlbedoTexture(arg("albedo")); applied.push_back("albedo"); }
			if (args.count("normal")) { material->SetNormalTexture(arg("normal")); applied.push_back("normal"); }
			if (args.count("baseColor"))
			{
				float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				std::stringstream stream(arg("baseColor"));
				std::string part;
				int index = 0;
				while (std::getline(stream, part, ',') && index < 4)
					rgba[index++] = std::strtof(part.c_str(), nullptr);
				material->SetBaseColor(glm::vec4 { rgba[0], rgba[1], rgba[2], rgba[3] });
				applied.push_back("baseColor");
			}
			if (args.count("metallic")) { material->SetMetallic(std::strtof(arg("metallic").c_str(), nullptr)); applied.push_back("metallic"); }
			if (args.count("roughness")) { material->SetRoughness(std::strtof(arg("roughness").c_str(), nullptr)); applied.push_back("roughness"); }
			if (args.count("blendMode"))
				material->SetBlendMode(static_cast<MaterialBlendMode>(std::atoi(arg("blendMode").c_str())));
			if (args.count("doubleSided"))
				material->SetDoubleSided(arg("doubleSided") == "1" || arg("doubleSided") == "true");
			if (applied.empty() && !args.count("blendMode") && !args.count("doubleSided"))
			{
				error = "nothing to set (albedo/normal/baseColor/metallic/roughness/blendMode/doubleSided)";
				return false;
			}
			bool saved = false;
			if (arg("save") == "1" || arg("save") == "true")
				saved = MaterialLibrary::Get().Save(material, path, nullptr);
			result = "revision=" + std::to_string(material->GetRevision()) + " saved=" + (saved ? "true" : "false");
			return true;
		}
		// ---- 录制 / 回放(把一次通道会话变成可复现脚本) ----
		if (cmd == "record.start" || cmd == "record.stop" || cmd == "replay")
		{
			if (cmd == "record.start")
			{
				const std::string path = arg("path");
				if (path.empty())
				{
					error = "missing path";
					return false;
				}
				StartAiRecording(path);
				result = "recording to " + path;
				return true;
			}
			if (cmd == "record.stop")
			{
				const size_t count = StopAiRecording();
				result = "recorded " + std::to_string(count) + " commands";
				return true;
			}
			// replay:按行读取 JSON 脚本并逐条执行(跳过录制/回放/退出类命令,防递归)。
			const std::string path = arg("path");
			std::ifstream file(path);
			if (!file)
			{
				error = "cannot open script: " + path;
				return false;
			}
			size_t executed = 0;
			size_t skipped = 0;
			std::string line;
			while (std::getline(file, line))
			{
				const auto objectStart = line.find('{');
				if (objectStart == std::string::npos)
					continue;
				std::map<std::string, std::string> fields;
				if (!Editor::AiControlServer::ParseFlatJson(line.substr(objectStart), &fields, nullptr))
				{
					++skipped;
					continue;
				}
				const std::string nested = fields.count("cmd") ? fields["cmd"] : std::string();
				if (nested.empty() || nested == "record.start" || nested == "record.stop"
					|| nested == "replay" || nested == "quit")
				{
					++skipped;
					continue;
				}
				std::string nestedResult;
				std::string nestedError;
				if (ExecuteAiCommand(nested, fields, nestedResult, nestedError))
					++executed;
				else
					++skipped;
			}
			result = "replayed executed=" + std::to_string(executed) + " skipped=" + std::to_string(skipped);
			return true;
		}
		error = "unknown command '" + cmd + "'";
		return false;
	}
}
