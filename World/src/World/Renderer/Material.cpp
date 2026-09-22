#include "wldpch.h"

#include "World/Renderer/Material.h"

#include "World/Core/Application.h"
#include "World/Renderer/MaterialLibrary.h"

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
		std::string Trim(const std::string& text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

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

		// Parent 这类逻辑路径:正常路径写裸标量(与方案的格式示例一致,可读),
		// 只有会破坏 YAML 的字符(首尾空白、'#'、': '、引号、反斜杠、换行、制表)才加引号。
		bool NeedsQuoting(const std::string& text)
		{
			if (text.empty())
				return true;
			if (text.front() == ' ' || text.front() == '\t' || text.back() == ' ' || text.back() == '\t')
				return true;
			for (std::size_t index = 0; index < text.size(); ++index)
			{
				const char c = text[index];
				if (c == '#' || c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t')
					return true;
				if (c == ':' && index + 1 < text.size() && text[index + 1] == ' ')
					return true;
			}
			return false;
		}

		std::string FormatLogicalPath(const std::string& text)
		{
			return NeedsQuoting(text) ? Quote(text) : text;
		}

		// M4-S2:`.wmat` 的 Params 值(值文本,见 MaterialParams.h)。
		//  - 标量:原样文本("0.25" / "true" / "textures/icon.png");
		//  - 序列:[a, b, c](空白归一,值文本是逗号分隔);
		//  - 其它(嵌套映射等)不支持 —— 调用方按错误处理。
		bool ReadParamValueText(const YAML::Node& node, std::string* out)
		{
			if (node.IsScalar())
			{
				if (out) *out = Trim(node.Scalar());
				return true;
			}
			if (node.IsSequence())
			{
				std::string text;
				for (const YAML::Node& element : node)
				{
					if (!element.IsScalar())
						return false;
					if (!text.empty())
						text.append(", ");
					text.append(Trim(element.Scalar()));
				}
				if (out) *out = text;
				return true;
			}
			return false;
		}

		// 写 .wmat 时的值形态:知道声明类型 → 数字/布尔/序列/字符串;不知道 → 按标量文本写。
		std::string FormatParamValueText(const std::string& name, const std::string& value,
			const std::vector<MaterialParamDecl>* paramDecls)
		{
			const MaterialParamDecl* decl = paramDecls ? FindParamDecl(*paramDecls, name) : nullptr;
			if (decl)
			{
				switch (decl->Type)
				{
					case ParamType::Texture2D:
						return Quote(value);
					case ParamType::Bool:
					case ParamType::Float:
					case ParamType::Int:
						return IsParamValueCompatible(decl->Type, value) ? value : Quote(value);
					case ParamType::Vec2:
					case ParamType::Vec3:
					case ParamType::Vec4:
					case ParamType::Color:
					{
						std::string normalized;
						std::string reason;
						if (NormalizeParamValue(decl->Type, value, &normalized, &reason))
							return "[" + normalized + "]";
						return Quote(value);
					}
				}
			}
			return FormatLogicalPath(value);
		}

		// 字段 ↔ MaterialDesc 成员的唯一映射(读/写/比较都走它,避免 9 个字段在 3 处各抄一遍)。
		void AssignField(MaterialDesc& target, const MaterialDesc& source, MaterialField field)
		{
			switch (field)
			{
			case MaterialField::Name: target.Name = source.Name; break;
			case MaterialField::BaseColor: target.BaseColor = source.BaseColor; break;
			case MaterialField::Metallic: target.Metallic = source.Metallic; break;
			case MaterialField::Roughness: target.Roughness = source.Roughness; break;
			case MaterialField::Emissive: target.Emissive = source.Emissive; break;
			case MaterialField::AlbedoTexture: target.AlbedoTexture = source.AlbedoTexture; break;
			case MaterialField::NormalTexture: target.NormalTexture = source.NormalTexture; break;
			case MaterialField::BlendMode: target.BlendMode = source.BlendMode; break;
			case MaterialField::DoubleSided: target.DoubleSided = source.DoubleSided; break;
			case MaterialField::Count: break;
			}
		}

		bool FieldsEqual(const MaterialDesc& a, const MaterialDesc& b, MaterialField field)
		{
			switch (field)
			{
			case MaterialField::Name: return a.Name == b.Name;
			case MaterialField::BaseColor: return a.BaseColor == b.BaseColor;
			case MaterialField::Metallic: return a.Metallic == b.Metallic;
			case MaterialField::Roughness: return a.Roughness == b.Roughness;
			case MaterialField::Emissive: return a.Emissive == b.Emissive;
			case MaterialField::AlbedoTexture: return a.AlbedoTexture == b.AlbedoTexture;
			case MaterialField::NormalTexture: return a.NormalTexture == b.NormalTexture;
			case MaterialField::BlendMode: return a.BlendMode == b.BlendMode;
			case MaterialField::DoubleSided: return a.DoubleSided == b.DoubleSided;
			case MaterialField::Count: break;
			}
			return false;
		}
	}

	int MaterialFieldSet::Count() const
	{
		int count = 0;
		for (uint8_t index = 0; index < static_cast<uint8_t>(MaterialField::Count); ++index)
			if (Has(static_cast<MaterialField>(index)))
				++count;
		return count;
	}

	void Material::SetDesc(const MaterialDesc& desc)
	{
		if (m_Desc == desc)
			return;
		// 逐字段比较:变了的值 = 这次是显式写入 → 记进覆盖集(M3)。
		// 老调用点(整份拷贝)因此仍然写全字段;只改一个字段的调用点只把那个字段记成覆盖。
		for (uint8_t index = 0; index < static_cast<uint8_t>(MaterialField::Count); ++index)
		{
			const MaterialField field = static_cast<MaterialField>(index);
			if (!FieldsEqual(m_Desc, desc, field))
				m_Overrides.Set(field);
		}
		const bool shaderChanged = m_Desc.ShaderPath != desc.ShaderPath;
		m_Desc = desc;
		if (shaderChanged)
		{
			m_HasShaderOverride = true;
			// shader 换了 → 注解参数表随之刷新(读盘由库负责)。
			MaterialLibrary::Get().RefreshParams(*this);
		}
		BumpRevision();
	}

	void Material::RevertField(MaterialField field)
	{
		if (!m_Overrides.Has(field))
			return;   // 已经是继承态:no-op(不动 Revision、不动脏标记)
		const MaterialDesc& inherited = m_Parent ? m_Parent->GetDesc() : MaterialIO::DefaultMaterialDesc();
		AssignField(m_Desc, inherited, field);
		m_Overrides.Clear(field);
		BumpRevision();
		// 值可能没变,但文件里那一行必须消失 —— 只有保存才落地,所以标记未保存。
		MarkDirty(true);
	}

	uint32_t Material::GetFormatVersion() const
	{
		// 只有"没有父级 + 全字段都写了"才回到老写法(与 M3 前的文件逐字节一致);
		// 其它情况都是材质实例格式。M4-S2:带 Shader/Params 的文件也必须是 v2。
		return (m_ParentPath.empty() && m_Overrides.All() && !m_HasShaderOverride
				&& m_ParamOverrides.empty())
			? kMaterialFormatVersionLegacy
			: kMaterialFormatVersionMax;
	}

	void Material::RecomputeParamWarnings()
	{
		m_ParamWarnings = BuildParamWarnings(m_ParamDecls, m_ParamOverrides, m_Desc.ShaderPath);
	}

	const std::string* Material::FindParamOverride(const std::string& name) const
	{
		for (const MaterialParamOverride& entry : m_ParamOverrides)
			if (entry.Name == name)
				return &entry.Value;
		return nullptr;
	}

	bool Material::HasParamOverride(const std::string& name) const
	{
		return FindParamOverride(name) != nullptr;
	}

	MaterialParamSource Material::ParamSource(const std::string& name) const
	{
		if (FindParamOverride(name))
			return MaterialParamSource::Local;
		if (m_Parent && m_Parent->FindParamOverride(name))
			return MaterialParamSource::Parent;
		return MaterialParamSource::ShaderDefault;
	}

	std::string Material::ParamDefaultValue(const std::string& name) const
	{
		const MaterialParamDecl* decl = FindParamDecl(m_ParamDecls, name);
		return decl ? decl->Default : std::string();
	}

	std::string Material::ResolvedParamValue(const std::string& name) const
	{
		if (const std::string* local = FindParamOverride(name))
			return *local;
		for (const Material* current = m_Parent.get(); current != nullptr;
			current = current->ResolvedParent().get())
		{
			if (const std::string* inherited = current->FindParamOverride(name))
				return *inherited;
		}
		return ParamDefaultValue(name);
	}

	bool Material::ParamMatchesDefault(const std::string& name) const
	{
		const std::string* local = FindParamOverride(name);
		if (!local)
			return false;
		const MaterialParamDecl* decl = FindParamDecl(m_ParamDecls, name);
		if (!decl)
			return false;
		std::string normalized;
		std::string reason;
		if (!NormalizeParamValue(decl->Type, *local, &normalized, &reason))
			return false;
		std::string defaultNormalized;
		if (!NormalizeParamValue(decl->Type, decl->Default, &defaultNormalized, &reason))
			return false;
		return normalized == defaultNormalized;
	}

	void Material::SetShaderPath(const std::string& path)
	{
		const std::string normalized = MaterialIO::NormalizePath(path);
		if (m_HasShaderOverride && m_Desc.ShaderPath == normalized)
			return;
		m_Desc.ShaderPath = normalized;
		m_HasShaderOverride = true;
		MaterialLibrary::Get().RefreshParams(*this);
		BumpRevision();
		MarkDirty(true);
	}

	void Material::RevertShader()
	{
		if (!m_HasShaderOverride)
			return;
		m_Desc.ShaderPath = m_Parent ? m_Parent->GetDesc().ShaderPath : std::string();
		m_HasShaderOverride = false;
		MaterialLibrary::Get().RefreshParams(*this);
		BumpRevision();
		MarkDirty(true);
	}

	void Material::SetParamOverride(const std::string& name, const std::string& value)
	{
		if (name.empty())
			return;
		// 类型已知时把值归一(序列空白、Color 补 alpha、0.5f → 0.5),保存回读才稳定;
		// 类型不符的值原样保留(靠 ParamWarnings 提示,不静默改写用户输入)。
		std::string stored = value;
		if (const MaterialParamDecl* decl = FindParamDecl(m_ParamDecls, name))
		{
			std::string normalized;
			std::string reason;
			if (NormalizeParamValue(decl->Type, value, &normalized, &reason))
				stored = normalized;
		}
		for (MaterialParamOverride& entry : m_ParamOverrides)
		{
			if (entry.Name != name)
				continue;
			if (entry.Value == stored)
				return;   // 值没变:不动 Revision / 脏标记
			entry.Value = stored;
			RecomputeParamWarnings();
			BumpRevision();
			MarkDirty(true);
			return;
		}
		m_ParamOverrides.push_back(MaterialParamOverride { name, stored });
		RecomputeParamWarnings();
		BumpRevision();
		MarkDirty(true);
	}

	void Material::RevertParam(const std::string& name)
	{
		for (auto entry = m_ParamOverrides.begin(); entry != m_ParamOverrides.end(); ++entry)
		{
			if (entry->Name != name)
				continue;
			m_ParamOverrides.erase(entry);
			RecomputeParamWarnings();
			BumpRevision();
			MarkDirty(true);
			return;
		}
	}

	namespace MaterialIO
	{
		const MaterialDesc& DefaultMaterialDesc()
		{
			static const MaterialDesc kDefault;
			return kDefault;
		}

		std::string NormalizePath(const std::string& path)
		{
			std::string normalized;
			normalized.reserve(path.size());
			for (const char c : path)
				normalized.push_back(c == '\\' ? '/' : c);
			while (normalized.rfind("./", 0) == 0)
				normalized.erase(0, 2);
			// 去掉重复的斜杠(保留协议风格前缀不在本用例范围)。
			normalized.erase(std::unique(normalized.begin(), normalized.end(),
				[](char a, char b) { return a == '/' && b == '/'; }), normalized.end());
			return normalized;
		}

		const char* FieldKey(MaterialField field)
		{
			switch (field)
			{
			case MaterialField::Name: return "Name";
			case MaterialField::BaseColor: return "BaseColor";
			case MaterialField::Metallic: return "Metallic";
			case MaterialField::Roughness: return "Roughness";
			case MaterialField::Emissive: return "Emissive";
			case MaterialField::AlbedoTexture: return "AlbedoTexture";
			case MaterialField::NormalTexture: return "NormalTexture";
			case MaterialField::BlendMode: return "BlendMode";
			case MaterialField::DoubleSided: return "DoubleSided";
			case MaterialField::Count: break;
			}
			return "";
		}

		std::string FormatFieldValue(const MaterialDesc& desc, MaterialField field)
		{
			switch (field)
			{
			case MaterialField::Name: return desc.Name;
			case MaterialField::BaseColor: return FormatVec(desc.BaseColor);
			case MaterialField::Metallic: return FormatFloat(desc.Metallic);
			case MaterialField::Roughness: return FormatFloat(desc.Roughness);
			case MaterialField::Emissive: return FormatVec(desc.Emissive);
			case MaterialField::AlbedoTexture: return desc.AlbedoTexture.empty() ? "(none)" : desc.AlbedoTexture;
			case MaterialField::NormalTexture: return desc.NormalTexture.empty() ? "(none)" : desc.NormalTexture;
			case MaterialField::BlendMode: return BlendModeName(desc.BlendMode);
			case MaterialField::DoubleSided: return desc.DoubleSided ? "true" : "false";
			case MaterialField::Count: break;
			}
			return std::string();
		}

		MaterialLoadResult ParseDocument(const std::string& text, MaterialDocument& out, std::string* error)
		{
			MaterialLoadResult result;
			out = MaterialDocument {};

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
			out.FormatVersion = version;

			std::string warning;
			try
			{
				// M3:写了哪个字段 = 覆盖哪个字段;没写的字段继承(没有父级 = 引擎内置默认)。
				// 老文件(v1 全字段)因此得到"全部覆盖",合并结果与 M3 前逐字段一致。
				if (root["Parent"])
				{
					out.ParentPath = NormalizePath(root["Parent"].as<std::string>());
				}
				if (root["Shader"])
				{
					out.Values.ShaderPath = NormalizePath(root["Shader"].as<std::string>());
					out.HasShader = true;
				}
				if (root["Params"])
				{
					const YAML::Node params = root["Params"];
					if (!params.IsMap())
					{
						result.Error = "Params 需要映射结构(参数名: 值)";
						if (error) *error = result.Error;
						return result;
					}
					for (const auto& entry : params)
					{
						MaterialParamOverride override;
						override.Name = entry.first.as<std::string>();
						if (override.Name.empty())
						{
							result.Error = "Params 里有空参数名";
							if (error) *error = result.Error;
							return result;
						}
						for (const MaterialParamOverride& existing : out.Params)
						{
							if (existing.Name == override.Name)
							{
								result.Error = "Params 里参数 '" + override.Name + "' 重复";
								if (error) *error = result.Error;
								return result;
							}
						}
						if (!ReadParamValueText(entry.second, &override.Value))
						{
							result.Error = "参数 '" + override.Name + "' 的值需要标量或序列(不支持嵌套结构)";
							if (error) *error = result.Error;
							return result;
						}
						out.Params.push_back(std::move(override));
					}
				}
				if (root["Name"])
				{
					out.Values.Name = root["Name"].as<std::string>();
					out.Overridden.Set(MaterialField::Name);
				}
				if (root["BaseColor"])
				{
					out.Values.BaseColor = ReadVec4(root["BaseColor"], out.Values.BaseColor, "BaseColor", &warning);
					out.Overridden.Set(MaterialField::BaseColor);
				}
				if (root["Metallic"])
				{
					out.Values.Metallic = root["Metallic"].as<float>();
					out.Overridden.Set(MaterialField::Metallic);
				}
				if (root["Roughness"])
				{
					out.Values.Roughness = root["Roughness"].as<float>();
					out.Overridden.Set(MaterialField::Roughness);
				}
				if (root["Emissive"])
				{
					out.Values.Emissive = ReadVec3(root["Emissive"], out.Values.Emissive, "Emissive", &warning);
					out.Overridden.Set(MaterialField::Emissive);
				}
				if (root["AlbedoTexture"])
				{
					out.Values.AlbedoTexture = root["AlbedoTexture"].as<std::string>();
					out.Overridden.Set(MaterialField::AlbedoTexture);
				}
				if (root["NormalTexture"])
				{
					out.Values.NormalTexture = root["NormalTexture"].as<std::string>();
					out.Overridden.Set(MaterialField::NormalTexture);
				}
				if (root["BlendMode"])
				{
					const std::string mode = root["BlendMode"].as<std::string>();
					if (mode == "Opaque") out.Values.BlendMode = MaterialBlendMode::Opaque;
					else if (mode == "Transparent") out.Values.BlendMode = MaterialBlendMode::Transparent;
					else
					{
						if (!warning.empty()) warning.append("; ");
						warning.append("BlendMode '").append(mode).append("' 未知,已用 Opaque");
						out.Values.BlendMode = MaterialBlendMode::Opaque;
					}
					out.Overridden.Set(MaterialField::BlendMode);
				}
				if (root["DoubleSided"])
				{
					out.Values.DoubleSided = root["DoubleSided"].as<bool>();
					out.Overridden.Set(MaterialField::DoubleSided);
				}
			}
			catch (const YAML::Exception& exception)
			{
				result.Error = std::string("字段类型错误: ") + exception.what();
				if (error) *error = result.Error;
				return result;
			}

			// 夹紧/越界警告与 M3 前逐字一致(未覆盖字段用的是引擎默认值,天然在范围内,不会产生噪声)。
			ClampWarn(out.Values.Metallic, 0.0f, 1.0f, "Metallic", &warning);
			ClampWarn(out.Values.Roughness, 0.02f, 1.0f, "Roughness", &warning);
			for (int i = 0; i < 3; ++i)
			{
				if (out.Values.BaseColor[i] < 0.0f || out.Values.BaseColor[i] > 1.0f)
					out.Values.BaseColor[i] = std::clamp(out.Values.BaseColor[i], 0.0f, 1.0f);
				if (out.Values.Emissive[i] < 0.0f)
					out.Values.Emissive[i] = 0.0f;
			}
			out.Values.BaseColor.w = std::clamp(out.Values.BaseColor.w, 0.0f, 1.0f);

			result.Success = true;
			result.Error = warning;
			if (error) *error = warning;
			return result;
		}

		MaterialDesc MergeDocument(const MaterialDocument& document, const MaterialDesc* parentDesc)
		{
			MaterialDesc merged = parentDesc ? *parentDesc : DefaultMaterialDesc();
			for (uint8_t index = 0; index < static_cast<uint8_t>(MaterialField::Count); ++index)
			{
				const MaterialField field = static_cast<MaterialField>(index);
				if (document.Overridden.Has(field))
					AssignField(merged, document.Values, field);
			}
			// M4-S2:Shader 是独立可继承字段(不参与 M3 的 MaterialField 位集,免得改变
			// "全字段 = v1 老写法"的判据)。
			if (document.HasShader)
				merged.ShaderPath = document.Values.ShaderPath;
			return merged;
		}

		uint32_t DocumentFormatVersion(const MaterialDocument& document)
		{
			// 老写法的唯一判据:没有父级 + 全部字段都写出 → 与 M3 前的文件逐字节一致。
			// 只写部分字段的文件必须是 v2(否则"缺字段 = 引擎默认"会与"覆盖字段"混淆)。
			// M4-S2:带 Shader / Params 的文件也只能是 v2(v1 没有这两个键)。
			return (document.ParentPath.empty() && document.Overridden.All()
					&& !document.HasShader && document.Params.empty())
				? kMaterialFormatVersionLegacy : kMaterialFormatVersionMax;
		}

		MaterialLoadResult Parse(const std::string& text, MaterialDesc& out, std::string* error)
		{
			MaterialDocument document;
			std::string parseError;
			const MaterialLoadResult parsed = ParseDocument(text, document, &parseError);
			if (!parsed.Success)
			{
				out = MaterialDesc {};
				if (error) *error = parsed.Error;
				return parsed;
			}

			out = MergeDocument(document, nullptr);

			MaterialLoadResult result = parsed;
			if (!document.ParentPath.empty())
			{
				// 纯文本解析不读盘:告诉调用方这条路径拿不到继承值(素材本身仍是可用的)。
				if (!result.Error.empty()) result.Error.append("; ");
				result.Error.append("Parent '").append(document.ParentPath)
					.append("' 未解析(纯文本解析只按引擎默认合并;要继承请用 MaterialLibrary::Load)");
			}
			if (error) *error = result.Error;
			return result;
		}

		std::string SerializeDocument(const MaterialDocument& document,
			const std::vector<MaterialParamDecl>* paramDecls)
		{
			const uint32_t version = DocumentFormatVersion(document);
			std::ostringstream out;
			if (version == kMaterialFormatVersionLegacy)
			{
				// 老写法保持 M3 前的头注释 + 全字段,写出字节与今天完全一致。
				out << "# WorldEngine 材质资产(D3)。颜色为 sRGB 空间取值。\n";
			}
			out << "FormatVersion: " << version << "\n";
			if (!document.ParentPath.empty())
				out << "Parent: " << FormatLogicalPath(document.ParentPath) << "\n";
			if (document.HasShader)
				out << "Shader: " << FormatLogicalPath(document.Values.ShaderPath) << "\n";
			if (document.Overridden.Has(MaterialField::Name))
				out << "Name: " << Quote(document.Values.Name) << "\n";
			if (document.Overridden.Has(MaterialField::BaseColor))
				out << "BaseColor: " << FormatVec(document.Values.BaseColor) << "\n";
			if (document.Overridden.Has(MaterialField::Metallic))
				out << "Metallic: " << FormatFloat(document.Values.Metallic) << "\n";
			if (document.Overridden.Has(MaterialField::Roughness))
				out << "Roughness: " << FormatFloat(document.Values.Roughness) << "\n";
			if (document.Overridden.Has(MaterialField::Emissive))
				out << "Emissive: " << FormatVec(document.Values.Emissive) << "\n";
			if (document.Overridden.Has(MaterialField::AlbedoTexture))
				out << "AlbedoTexture: " << Quote(document.Values.AlbedoTexture) << "\n";
			if (document.Overridden.Has(MaterialField::NormalTexture))
				out << "NormalTexture: " << Quote(document.Values.NormalTexture) << "\n";
			if (document.Overridden.Has(MaterialField::BlendMode))
				out << "BlendMode: " << BlendModeName(document.Values.BlendMode) << "\n";
			if (document.Overridden.Has(MaterialField::DoubleSided))
				out << "DoubleSided: " << (document.Values.DoubleSided ? "true" : "false") << "\n";
			if (!document.Params.empty())
			{
				out << "Params:\n";
				for (const MaterialParamOverride& entry : document.Params)
					out << "  " << entry.Name << ": "
						<< FormatParamValueText(entry.Name, entry.Value, paramDecls) << "\n";
			}
			return out.str();
		}

		std::string Serialize(const MaterialDesc& desc)
		{
			// 老口径:全字段 + (没有父级)→ v1,与 M3 前的 Serialize 逐字节一致。
			// 导入器 / 新建模板 / 既有测试都走这里,行为不变。
			MaterialDocument document;
			document.Values = desc;
			document.Overridden = MaterialFieldSet::Everything();
			// M4-S2:带了 shader 的材质不能用 v1 写出(v1 没有 Shader 键,会静默丢引用)。
			document.HasShader = !desc.ShaderPath.empty();
			return SerializeDocument(document);
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
