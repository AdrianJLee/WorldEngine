#include "wldpch.h"

#include "World/Renderer/Material.h"

#include "World/Core/Application.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World
{
	namespace
	{
		bool InRange(float value, float min, float max)
		{
			return value >= min && value <= max;
		}

		void ClampWarn(float& value, float min, float max, const char* name, std::string* warning)
		{
			if (InRange(value, min, max))
				return;
			const float clamped = std::clamp(value, min, max);
			if (warning)
			{
				if (!warning->empty())
					warning->append("; ");
				warning->append(name);
				warning->append(" 超出范围 [")
					.append(std::to_string(min)).append(", ").append(std::to_string(max))
					.append("],已夹紧为 ").append(std::to_string(clamped));
			}
			value = clamped;
		}

		glm::vec4 ReadVec4(const YAML::Node& node, const glm::vec4& fallback, const char* field, std::string* warning)
		{
			if (!node)
				return fallback;
			if (!node.IsSequence() || node.size() != 4)
			{
				if (warning)
				{
					if (!warning->empty()) warning->append("; ");
					warning->append(field).append(" 需要 4 个数字,已用默认值");
				}
				return fallback;
			}
			glm::vec4 out;
			for (int i = 0; i < 4; ++i)
			{
				try { out[i] = node[i].as<float>(); }
				catch (const YAML::Exception&)
				{
					if (warning)
					{
						if (!warning->empty()) warning->append("; ");
						warning->append(field).append(" 含非数字项,已用默认值");
					}
					return fallback;
				}
			}
			return out;
		}

		glm::vec3 ReadVec3(const YAML::Node& node, const glm::vec3& fallback, const char* field, std::string* warning)
		{
			if (!node)
				return fallback;
			if (!node.IsSequence() || node.size() != 3)
			{
				if (warning)
				{
					if (!warning->empty()) warning->append("; ");
					warning->append(field).append(" 需要 3 个数字,已用默认值");
				}
				return fallback;
			}
			glm::vec3 out;
			for (int i = 0; i < 3; ++i)
			{
				try { out[i] = node[i].as<float>(); }
				catch (const YAML::Exception&)
				{
					if (warning)
					{
						if (!warning->empty()) warning->append("; ");
						warning->append(field).append(" 含非数字项,已用默认值");
					}
					return fallback;
				}
			}
			return out;
		}

		const char* BlendModeName(MaterialBlendMode mode)
		{
			return mode == MaterialBlendMode::Transparent ? "Transparent" : "Opaque";
		}

		std::string FormatFloat(float value)
		{
			// U23:必须保证 float → 文本 → float **逐位可逆**。
			//
			// 旧写法是 std::fixed + precision(6):0.1f + 0.2f(= 0.30000001192092896)写成
			// "0.300000",而 MaterialLibrary::Save 的回读校验当时按逐位相等比较 —— 于是
			// "拖一下滑杆再保存"会报 "Save failed: 写入校验失败",磁盘其实已经写了、
			// 脏标记却永远清不掉(U22 实测复现)。
			//
			// std::to_chars 给的是"最短往返表示":能用 1 位小数表示的写 1 位,精度不够的
			// 自动加位数,读回**始终是同一个 float**;同时比 fixed + 9 位小数可读
			// (0.3 写 "0.3"、0.25 写 "0.25")。非有限值(np/inf)没有往返语义,回落到
			// 旧格式,让 Parse/校验按原路径拒绝,而不是写出无法解析的字面量。
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
			// 去掉多余的尾随 0(保持文件可读),但至少保留一位小数。
			while (text.size() > 3 && text.back() == '0' && text[text.size() - 2] != '.')
				text.pop_back();
			return text;
		}

		std::string FormatVec(const glm::vec4& value)
		{
			return "[" + FormatFloat(value.x) + ", " + FormatFloat(value.y) + ", "
				+ FormatFloat(value.z) + ", " + FormatFloat(value.w) + "]";
		}

		std::string FormatVec(const glm::vec3& value)
		{
			return "[" + FormatFloat(value.x) + ", " + FormatFloat(value.y) + ", "
				+ FormatFloat(value.z) + "]";
		}

		std::string Quote(const std::string& text)
		{
			std::string out = "\"";
			for (const char c : text)
			{
				if (c == '"' || c == '\\')
					out.push_back('\\');
				out.push_back(c);
			}
			out.push_back('"');
			return out;
		}
	}

	namespace MaterialIO
	{
		MaterialLoadResult Parse(const std::string& text, MaterialDesc& out, std::string* error)
		{
			MaterialLoadResult result;
			out = MaterialDesc {};

			YAML::Node root;
			try
			{
				root = YAML::Load(text);
			}
			catch (const YAML::Exception& exception)
			{
				result.Error = std::string("YAML 解析失败: ") + exception.what();
				if (error) *error = result.Error;
				return result;
			}
			if (!root || !root.IsMap())
			{
				result.Error = "材质文件为空或不是映射结构";
				if (error) *error = result.Error;
				return result;
			}

			uint32_t version = kFormatVersion;
			try
			{
				if (root["FormatVersion"])
					version = root["FormatVersion"].as<uint32_t>();
			}
			catch (const YAML::Exception&)
			{
				result.Error = "FormatVersion 不是整数";
				if (error) *error = result.Error;
				return result;
			}
			if (version == 0 || version > kFormatVersion)
			{
				result.Error = "不支持的材质格式版本 " + std::to_string(version)
					+ "(本引擎支持 1.." + std::to_string(kFormatVersion) + ")";
				if (error) *error = result.Error;
				return result;
			}

			std::string warning;
			try
			{
				if (root["Name"]) out.Name = root["Name"].as<std::string>();
				out.BaseColor = ReadVec4(root["BaseColor"], out.BaseColor, "BaseColor", &warning);
				if (root["Metallic"]) out.Metallic = root["Metallic"].as<float>();
				if (root["Roughness"]) out.Roughness = root["Roughness"].as<float>();
				out.Emissive = ReadVec3(root["Emissive"], out.Emissive, "Emissive", &warning);
				if (root["AlbedoTexture"]) out.AlbedoTexture = root["AlbedoTexture"].as<std::string>();
				if (root["NormalTexture"]) out.NormalTexture = root["NormalTexture"].as<std::string>();
				if (root["BlendMode"])
				{
					const std::string mode = root["BlendMode"].as<std::string>();
					if (mode == "Opaque") out.BlendMode = MaterialBlendMode::Opaque;
					else if (mode == "Transparent") out.BlendMode = MaterialBlendMode::Transparent;
					else
					{
						if (!warning.empty()) warning.append("; ");
						warning.append("BlendMode '").append(mode).append("' 未知,已用 Opaque");
						out.BlendMode = MaterialBlendMode::Opaque;
					}
				}
				if (root["DoubleSided"]) out.DoubleSided = root["DoubleSided"].as<bool>();
			}
			catch (const YAML::Exception& exception)
			{
				result.Error = std::string("字段类型错误: ") + exception.what();
				if (error) *error = result.Error;
				return result;
			}

			ClampWarn(out.Metallic, 0.0f, 1.0f, "Metallic", &warning);
			ClampWarn(out.Roughness, 0.02f, 1.0f, "Roughness", &warning);
			for (int i = 0; i < 3; ++i)
			{
				if (out.BaseColor[i] < 0.0f || out.BaseColor[i] > 1.0f)
					out.BaseColor[i] = std::clamp(out.BaseColor[i], 0.0f, 1.0f);
				if (out.Emissive[i] < 0.0f)
					out.Emissive[i] = 0.0f;
			}
			out.BaseColor.w = std::clamp(out.BaseColor.w, 0.0f, 1.0f);

			result.Success = true;
			result.Error = warning;
			if (error) *error = warning;
			return result;
		}

		std::string Serialize(const MaterialDesc& desc)
		{
			std::ostringstream out;
			out << "# WorldEngine 材质资产(D3)。颜色为 sRGB 空间取值。\n";
			out << "FormatVersion: " << kFormatVersion << "\n";
			out << "Name: " << Quote(desc.Name) << "\n";
			out << "BaseColor: " << FormatVec(desc.BaseColor) << "\n";
			out << "Metallic: " << FormatFloat(desc.Metallic) << "\n";
			out << "Roughness: " << FormatFloat(desc.Roughness) << "\n";
			out << "Emissive: " << FormatVec(desc.Emissive) << "\n";
			out << "AlbedoTexture: " << Quote(desc.AlbedoTexture) << "\n";
			out << "NormalTexture: " << Quote(desc.NormalTexture) << "\n";
			out << "BlendMode: " << BlendModeName(desc.BlendMode) << "\n";
			out << "DoubleSided: " << (desc.DoubleSided ? "true" : "false") << "\n";
			return out.str();
		}

		bool EquivalentForSave(const MaterialDesc& actual, const MaterialDesc& expected, float tolerance)
		{
			const float limit = tolerance > 0.0f ? tolerance : 0.0f;
			// NaN 的 |a-b| <= limit 恒为 false:与逐位比较一样拒绝 NaN。
			// (名字不用 near:Windows.h 的历史宏 near/far 会把它替换掉。)
			const auto closeEnough = [limit](float a, float b) { return std::fabs(a - b) <= limit; };
			const auto nearVec4 = [&closeEnough](const glm::vec4& a, const glm::vec4& b)
			{
				return closeEnough(a.x, b.x) && closeEnough(a.y, b.y)
					&& closeEnough(a.z, b.z) && closeEnough(a.w, b.w);
			};
			const auto nearVec3 = [&closeEnough](const glm::vec3& a, const glm::vec3& b)
			{
				return closeEnough(a.x, b.x) && closeEnough(a.y, b.y) && closeEnough(a.z, b.z);
			};
			return actual.Name == expected.Name
				&& nearVec4(actual.BaseColor, expected.BaseColor)
				&& closeEnough(actual.Metallic, expected.Metallic)
				&& closeEnough(actual.Roughness, expected.Roughness)
				&& nearVec3(actual.Emissive, expected.Emissive)
				&& actual.AlbedoTexture == expected.AlbedoTexture
				&& actual.NormalTexture == expected.NormalTexture
				&& actual.BlendMode == expected.BlendMode
				&& actual.DoubleSided == expected.DoubleSided;
		}

		// 内容根解析:项目清单的 content_root = Game/assets,而 WLD_GAME_DIR = Game/。
		// MaterialPath 一律按"**相对内容根**"书写;P4-U12 删掉了"再试 Game/ 旧布局"的候选。
		namespace
		{
			std::filesystem::path ResolveOnDisk(const std::string& path, bool forWrite)
			{
				std::error_code ec;
				const std::filesystem::path candidate =
					std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets" / path;
				if (forWrite || std::filesystem::exists(candidate, ec))
					return candidate;
				return {};
			}
		}

		bool ReadFileText(const std::string& path, std::string& out)
		{
			out.clear();
			if (path.empty())
				return false;

			// 1. VFS 优先(开发目录 provider / 发行包 provider)。
			if (Application::HasInstance())
			{
				std::error_code vfsError;
				std::vector<uint8_t> bytes;
				if (Application::Get().GetContext().Vfs().Read(path, bytes, vfsError) && !bytes.empty())
				{
					out.assign(bytes.begin(), bytes.end());
					return true;
				}
			}

			// 2. 磁盘回退:**内容根**(Game/assets;绝对路径原样命中,不拼内容根)。
			// P4-U12:删掉"再试 Editor/ 仓库布局"的第二候选 —— 内容根只有一个。
			const std::filesystem::path candidate = ResolveOnDisk(path, /*forWrite*/ false);
			std::ifstream file(candidate, std::ios::binary);
			if (!file)
				return false;
			std::ostringstream buffer;
			buffer << file.rdbuf();
			out = buffer.str();
			return !out.empty();
		}

		bool WriteFileText(const std::string& path, const std::string& text, std::string* error)
		{
			if (path.empty())
			{
				if (error) *error = "路径为空";
				return false;
			}
			std::error_code ec;
			// 写入统一落在内容根(Game/assets)下,与 VFS 的开发目录挂载一致。
			const std::filesystem::path target = ResolveOnDisk(path, /*forWrite*/ true);
			std::filesystem::create_directories(target.parent_path(), ec);
			const std::filesystem::path temporary = target.string() + ".tmp";
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file)
				{
					if (error) *error = "无法写入 " + temporary.string();
					return false;
				}
				file << text;
			}
			std::filesystem::rename(temporary, target, ec);
			if (ec)
			{
				// Windows 上目标已存在时 rename 会失败:先删再换。
				std::filesystem::remove(target, ec);
				ec.clear();
				std::filesystem::rename(temporary, target, ec);
			}
			if (ec)
			{
				if (error) *error = "无法替换 " + target.string() + ": " + ec.message();
				return false;
			}
			return true;
		}
	}
}
