#include "wldpch.h"
#include "World/Core/Asset/ProjectManifest.h"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace World::Asset
{
	namespace
	{
		// 相对路径校验:非空、非绝对、无盘符、无 ".." 段,统一用 '/'。
		bool ValidateRelativePath(const std::string& raw, std::string* normalized, std::string* error)
		{
			if (raw.empty())
			{
				if (error) *error = "path must not be empty";
				return false;
			}
			if (raw.find('\\') != std::string::npos)
			{
				if (error) *error = "path must use '/' separators: " + raw;
				return false;
			}
			const std::filesystem::path path(raw);
			if (path.is_absolute() || path.has_root_name())
			{
				if (error) *error = "path must be relative: " + raw;
				return false;
			}
			for (const auto& part : path)
				if (part == "..")
				{
					if (error) *error = "path must not contain '..': " + raw;
					return false;
				}
			std::string cleaned;
			for (const auto& part : path)
			{
				if (!cleaned.empty() && cleaned.back() != '/')
					cleaned += '/';
				cleaned += part.string();
			}
			if (normalized)
				*normalized = cleaned.empty() ? raw : cleaned;
			return true;
		}

		bool ValidateManifest(ProjectManifest& manifest, std::string* error)
		{
			if (manifest.Id.empty())
			{
				if (error) *error = "manifest 'id' must not be empty";
				return false;
			}
			std::string contentRoot;
			if (!ValidateRelativePath(manifest.ContentRoot.generic_string(), &contentRoot, error))
				return false;
			manifest.ContentRoot = contentRoot;
			if (manifest.StartScene.empty() ||
				!ValidateRelativePath(manifest.StartScene, &manifest.StartScene, error))
				return false;
			if (manifest.Renderer != "opengl" && manifest.Renderer != "vulkan")
			{
				if (error) *error = "renderer must be 'opengl' or 'vulkan': " + manifest.Renderer;
				return false;
			}
			// rendering 区块(可缺省):数值范围与 2 的幂约束在这里统一拒绝,
			// 避免"贴图尺寸 100"这类值到初始化期才炸在 GPU 资源创建里。
			const RenderingSettings& rendering = manifest.Rendering;
			const uint32_t shadowSize = rendering.ShadowMapSize;
			if (shadowSize < 256 || shadowSize > 4096 || (shadowSize & (shadowSize - 1)) != 0)
			{
				if (error) *error = "rendering.shadow_map_size must be a power of two in [256, 4096]: "
					+ std::to_string(shadowSize);
				return false;
			}
			if (rendering.MaxDirectionalLights < 1 || rendering.MaxDirectionalLights > 2)
			{
				if (error) *error = "rendering.max_directional_lights must be in [1, 2]: "
					+ std::to_string(rendering.MaxDirectionalLights);
				return false;
			}
			if (rendering.MaxPointLights > 7)
			{
				if (error) *error = "rendering.max_point_lights must be in [0, 7]: "
					+ std::to_string(rendering.MaxPointLights);
				return false;
			}
			if (rendering.MaxDirectionalLights + rendering.MaxPointLights > 8)
			{
				if (error) *error = "rendering light limits exceed the 8-light UBO capacity";
				return false;
			}
			// P4-1:各向异性上限(1..16)——超过设备能力时引擎内部再退化。
			if (rendering.Anisotropy < 1 || rendering.Anisotropy > 16)
			{
				if (error) *error = "rendering.anisotropy must be in [1, 16]: "
					+ std::to_string(rendering.Anisotropy);
				return false;
			}
			// P4-4b:MSAA 采样数只接受 1/2/4/8;非法值必须在加载期拒绝(3/0/16 这类值
			// 直接创建 VkSampleCountFlagBits/GL 附件的非法采样数)。
			if (!RenderingSettings::IsValidMsaa(rendering.Msaa))
			{
				if (error) *error = "rendering.msaa must be 1, 2, 4 or 8: "
					+ std::to_string(rendering.Msaa);
				return false;
			}
			// P4-3:渲染分辨率倍率(0.25..2.0;含 NaN/inf 拒绝)。非法值必须在加载期
			// 拒绝:0 或负数会被换算成 0 尺寸的纹理/帧缓冲,到 GPU 资源创建才炸。
			if (!std::isfinite(rendering.RenderScale) ||
				rendering.RenderScale < RenderingSettings::MinRenderScale ||
				rendering.RenderScale > RenderingSettings::MaxRenderScale)
			{
				if (error) *error = "rendering.render_scale must be in [0.25, 2.0]: "
					+ std::to_string(rendering.RenderScale);
				return false;
			}
			// P4-1:物理设置范围校验。
			if (manifest.Physics.FixedStepHz < 1 || manifest.Physics.FixedStepHz > 240)
			{
				if (error) *error = "physics.fixed_step_hz must be in [1, 240]: "
					+ std::to_string(manifest.Physics.FixedStepHz);
				return false;
			}
			if (!std::isfinite(manifest.Physics.Gravity))
			{
				if (error) *error = "physics.gravity must be a finite number";
				return false;
			}
			for (std::string& package : manifest.Packages)
				if (!ValidateRelativePath(package, &package, error))
					return false;
			return true;
		}

		// ---- U2e:清单文本合并 ----------------------------------------------------
		//
		// 为什么不全量重写:设置面板是"改一个开关 → 400ms 防抖自动写盘",而旧实现用
		// YAML::Emitter 重新序列化整份清单 —— 一次误触就会抹掉清单里给引擎用户看的
		// 中文注释,还会把 `render_scale: 1.0` 写成 `1`、`-9.81` 写成 `-9.810000`。
		// 注释是产品资产,所以文件已存在时只做 key 级文本合并:命中
		// `^(\s*)<key>:\s*.*$` 就只换值(保留缩进、值后空白、行尾注释与行尾符);
		// 缺的 key 追加到所属区块尾;`packages:` 这种序列整块重写。文件不存在或为空时
		// 仍走全量序列化(与旧行为逐字节一致)。

		constexpr size_t kNoLine = static_cast<size_t>(-1);

		bool IsBlankOrComment(const std::string& text)
		{
			size_t offset = 0;
			while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t'))
				++offset;
			return offset >= text.size() || text[offset] == '#';
		}

		struct TextLine
		{
			std::string Text;   // 行内容(不含行尾符)
			std::string Eol;    // "\n" / "\r\n" / ""(文件最后一行没有换行)
		};

		// 逐行切开并保留每行自己的行尾符:清单在 Windows 上是 CRLF,不能被换成 LF。
		std::vector<TextLine> SplitLines(const std::string& text)
		{
			std::vector<TextLine> lines;
			size_t offset = 0;
			while (offset < text.size())
			{
				const size_t newline = text.find('\n', offset);
				if (newline == std::string::npos)
				{
					lines.push_back({ text.substr(offset), std::string() });
					break;
				}
				size_t end = newline;
				std::string eol = "\n";
				if (end > offset && text[end - 1] == '\r')
				{
					--end;
					eol = "\r\n";
				}
				lines.push_back({ text.substr(offset, end - offset), eol });
				offset = newline + 1;
			}
			return lines;
		}

		std::string JoinLines(const std::vector<TextLine>& lines)
		{
			std::string text;
			for (const TextLine& line : lines)
			{
				text += line.Text;
				text += line.Eol;
			}
			return text;
		}

		// 追加行沿用文件自己已有的行尾符(第一行有行尾符的那个)。
		std::string FileEol(const std::vector<TextLine>& lines)
		{
			for (const TextLine& line : lines)
				if (!line.Eol.empty())
					return line.Eol;
			return "\n";
		}

		// 命中 `^(\s*)<key>:\s*.*$` 时给出缩进宽度、值起点与行尾注释起点。
		// `<key>` 后必须紧跟 ':'(避免 `shadow_map_size_x` 命中 `shadow_map_size`)。
		bool MatchKeyLine(const std::string& text, std::string_view key, size_t* indent,
			size_t* valueStart, size_t* commentStart)
		{
			size_t offset = 0;
			while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t'))
				++offset;
			if (text.compare(offset, key.size(), key) != 0)
				return false;
			const size_t colon = offset + key.size();
			if (colon >= text.size() || text[colon] != ':')
				return false;
			size_t value = colon + 1;
			while (value < text.size() && (text[value] == ' ' || text[value] == '\t'))
				++value;
			size_t comment = text.size();
			bool inSingle = false;
			bool inDouble = false;
			for (size_t i = value; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c == '\'' && !inDouble)
					inSingle = !inSingle;
				else if (c == '"' && !inSingle)
					inDouble = !inDouble;
				else if (c == '#' && !inSingle && !inDouble &&
					(i == value || text[i - 1] == ' ' || text[i - 1] == '\t'))
				{
					comment = i;
					break;
				}
			}
			if (indent) *indent = offset;
			if (valueStart) *valueStart = value;
			if (commentStart) *commentStart = comment;
			return true;
		}

		// 只换值:缩进、`key:` 后的空白、值后的空白、行尾注释与行尾符全部保留。
		std::string ReplaceKeyLineValue(const std::string& text, std::string_view key, const std::string& value)
		{
			size_t valueStart = 0;
			size_t commentStart = 0;
			if (!MatchKeyLine(text, key, nullptr, &valueStart, &commentStart))
				return text;
			size_t valueEnd = commentStart;
			while (valueEnd > valueStart && (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t'))
				--valueEnd;
			std::string updated = text.substr(0, valueStart);
			if (updated.empty() || (updated.back() != ' ' && updated.back() != '\t'))
				updated += ' ';   // `key:` 后没空白时补一个(`culling:` → `culling: true`)
			updated += value;
			const std::string tail = text.substr(valueEnd);
			if (!tail.empty() && tail[0] == '#')
				updated += ' ';   // `culling: # 说明` 这类"值就是注释"的行
			updated += tail;
			return updated;
		}

		std::string FormatBoolValue(bool value)
		{
			return value ? "true" : "false";
		}

		// 数值写"最短往返"格式:`std::to_string` 会把 float 写成 `-9.810000`,
		// 而 1.0f 的最短往返表示就是 `1` —— 直接写回会让 render_scale 看起来成了整数,
		// 所以缺小数点时补 `.0` 保住浮点写法(`1.0`→`1.0`,`-9.81`→`-9.81`,不丢精度)。
		std::string FormatFloatValue(float value)
		{
			char buffer[64];
			const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
			std::string text;
			if (result.ec == std::errc())
				text.assign(buffer, result.ptr);
			else
				text = std::to_string(value);
			if (text.find_first_of(".eE") == std::string::npos &&
				text.find("inf") == std::string::npos && text.find("nan") == std::string::npos)
				text += ".0";
			return text;
		}

		size_t FindTopLevelKey(const std::vector<TextLine>& lines, std::string_view key)
		{
			for (size_t i = 0; i < lines.size(); ++i)
			{
				size_t indent = 0;
				if (MatchKeyLine(lines[i].Text, key, &indent, nullptr, nullptr) && indent == 0)
					return i;
			}
			return kNoLine;
		}

		// 区块内的子键(缩进 > 0,不会和顶层同名键混起来)。
		size_t FindBlockKey(const std::vector<TextLine>& lines, size_t blockStart, size_t blockEnd,
			std::string_view key)
		{
			for (size_t i = blockStart + 1; i < blockEnd; ++i)
			{
				size_t indent = 0;
				if (MatchKeyLine(lines[i].Text, key, &indent, nullptr, nullptr) && indent > 0)
					return i;
			}
			return kNoLine;
		}

		// 区块范围:区块头之后到下一个顶格行(下一个顶层键)或文件末尾。
		size_t BlockEnd(const std::vector<TextLine>& lines, size_t blockStart)
		{
			for (size_t i = blockStart + 1; i < lines.size(); ++i)
				if (!IsBlankOrComment(lines[i].Text) && lines[i].Text[0] != ' ' && lines[i].Text[0] != '\t')
					return i;
			return lines.size();
		}

		// 区块内最后一个"内容行"之后的位置:缺的 key 追加在这里 —— 区块尾随的
		// 空行/注释行往往是下一块的说明,不能被插到中间去。
		size_t BlockContentEnd(const std::vector<TextLine>& lines, size_t blockStart, size_t blockEnd)
		{
			size_t end = blockStart + 1;
			for (size_t i = blockStart + 1; i < blockEnd; ++i)
				if (!IsBlankOrComment(lines[i].Text))
					end = i + 1;
			return end;
		}

		// 追加的 key 沿用区块里已有的缩进,没有可参照的行时用 2 空格。
		std::string BlockIndent(const std::vector<TextLine>& lines, size_t blockStart, size_t blockEnd)
		{
			for (size_t i = blockStart + 1; i < blockEnd; ++i)
			{
				const std::string& text = lines[i].Text;
				if (IsBlankOrComment(text))
					continue;
				size_t indent = 0;
				while (indent < text.size() && (text[indent] == ' ' || text[indent] == '\t'))
					++indent;
				if (indent > 0)
					return text.substr(0, indent);
				break;
			}
			return "  ";
		}

		struct ManifestKeyValue
		{
			const char* Key;
			std::string Value;
		};

		std::vector<ManifestKeyValue> TopLevelKeyValues(const ProjectManifest& manifest)
		{
			return {
				{ "id", manifest.Id },
				{ "version", manifest.Version },
				{ "content_root", manifest.ContentRoot.generic_string() },
				{ "start_scene", manifest.StartScene },
				{ "renderer", manifest.Renderer },
			};
		}

		// 顺序与旧的全量序列化一致(值没变时合并结果逐字节相同)。
		std::vector<ManifestKeyValue> RenderingKeyValues(const ProjectManifest& manifest)
		{
			return {
				{ "culling", FormatBoolValue(manifest.Rendering.Culling) },
				{ "shadows", FormatBoolValue(manifest.Rendering.Shadows) },
				{ "shadow_map_size", std::to_string(manifest.Rendering.ShadowMapSize) },
				{ "max_directional_lights", std::to_string(manifest.Rendering.MaxDirectionalLights) },
				{ "max_point_lights", std::to_string(manifest.Rendering.MaxPointLights) },
				{ "gpu_timing", FormatBoolValue(manifest.Rendering.GpuTiming) },
				{ "vsync", FormatBoolValue(manifest.Rendering.Vsync) },
				{ "instancing", FormatBoolValue(manifest.Rendering.Instancing) },
				{ "anisotropy", std::to_string(manifest.Rendering.Anisotropy) },
				{ "msaa", std::to_string(manifest.Rendering.Msaa) },
				{ "render_scale", FormatFloatValue(manifest.Rendering.RenderScale) },
			};
		}

		std::vector<ManifestKeyValue> PhysicsKeyValues(const ProjectManifest& manifest)
		{
			return {
				{ "fixed_step_hz", std::to_string(manifest.Physics.FixedStepHz) },
				{ "gravity", FormatFloatValue(manifest.Physics.Gravity) },
			};
		}

		struct TextEdit
		{
			size_t Start = 0;    // 原始行下标
			size_t Count = 0;    // 被替换掉的行数
			std::vector<TextLine> Replacement;
		};

		// `rendering:` / `physics:` 区块:逐 key 换值,缺的 key 追加到区块内容末尾;
		// 整个区块缺失时补一整块(追加到文件末尾)。
		void MergeManifestBlock(std::vector<TextLine>& lines, std::vector<TextEdit>& edits,
			std::vector<TextLine>& appended, std::string_view blockKey,
			const std::vector<ManifestKeyValue>& entries, const std::string& eol)
		{
			const size_t blockStart = FindTopLevelKey(lines, blockKey);
			if (blockStart == kNoLine)
			{
				appended.push_back({ std::string(blockKey) + ":", eol });
				for (const ManifestKeyValue& entry : entries)
					appended.push_back({ "  " + std::string(entry.Key) + ": " + entry.Value, eol });
				return;
			}

			// 区块头带内联值(`rendering: {}` 或整块写在一行)时追加子键会写出非法 YAML,
			// 先归一成裸区块头(行尾注释保留)。
			size_t headerValue = 0;
			size_t headerComment = 0;
			if (MatchKeyLine(lines[blockStart].Text, blockKey, nullptr, &headerValue, &headerComment))
			{
				size_t headerEnd = headerComment;
				while (headerEnd > headerValue &&
					(lines[blockStart].Text[headerEnd - 1] == ' ' || lines[blockStart].Text[headerEnd - 1] == '\t'))
					--headerEnd;
				if (headerEnd > headerValue)
					lines[blockStart].Text = std::string(blockKey) + ":" +
						(headerComment < lines[blockStart].Text.size()
							? " " + lines[blockStart].Text.substr(headerComment)
							: std::string());
			}

			const size_t blockEnd = BlockEnd(lines, blockStart);
			const size_t insertAt = BlockContentEnd(lines, blockStart, blockEnd);
			const std::string indent = BlockIndent(lines, blockStart, blockEnd);
			std::vector<TextLine> missing;
			for (const ManifestKeyValue& entry : entries)
			{
				const size_t index = FindBlockKey(lines, blockStart, blockEnd, entry.Key);
				if (index == kNoLine)
					missing.push_back({ indent + std::string(entry.Key) + ": " + entry.Value, eol });
				else
					lines[index].Text = ReplaceKeyLineValue(lines[index].Text, entry.Key, entry.Value);
			}
			if (!missing.empty())
				edits.push_back({ insertAt, 0, std::move(missing) });
		}

		std::string MergeManifestText(const std::string& original, const ProjectManifest& manifest)
		{
			std::vector<TextLine> lines = SplitLines(original);
			const std::string eol = FileEol(lines);
			std::vector<TextEdit> edits;
			std::vector<TextLine> appended;   // 追加到文件末尾的新行

			for (const ManifestKeyValue& entry : TopLevelKeyValues(manifest))
			{
				const size_t index = FindTopLevelKey(lines, entry.Key);
				if (index == kNoLine)
					appended.push_back({ std::string(entry.Key) + ": " + entry.Value, eol });
				else
					lines[index].Text = ReplaceKeyLineValue(lines[index].Text, entry.Key, entry.Value);
			}

			MergeManifestBlock(lines, edits, appended, "rendering", RenderingKeyValues(manifest), eol);
			MergeManifestBlock(lines, edits, appended, "physics", PhysicsKeyValues(manifest), eol);

			// `packages:` 是序列:key 级合并对列表项没有意义,整块按当前值重写
			// (块前的注释行不在块内,自然保留)。
			std::vector<TextLine> packages;
			packages.push_back({ "packages:", eol });
			for (const std::string& package : manifest.Packages)
				packages.push_back({ "  - " + package, eol });
			const size_t packagesIndex = FindTopLevelKey(lines, "packages");
			if (packagesIndex == kNoLine)
				appended.insert(appended.end(), packages.begin(), packages.end());
			else
			{
				const size_t contentEnd = BlockContentEnd(lines, packagesIndex, BlockEnd(lines, packagesIndex));
				edits.push_back({ packagesIndex, contentEnd - packagesIndex, std::move(packages) });
			}

			// 从后往前应用:前面的行下标不受后面的增删影响;同一起点时先做整块替换/删除,
			// 这样"插在这个位置"的行(前一个区块缺的 key)才会落在被替换内容的前面。
			std::sort(edits.begin(), edits.end(), [](const TextEdit& left, const TextEdit& right)
			{
				if (left.Start != right.Start)
					return left.Start > right.Start;
				return left.Count > right.Count;
			});
			for (const TextEdit& edit : edits)
			{
				const auto begin = lines.begin() + static_cast<std::ptrdiff_t>(edit.Start);
				lines.erase(begin, begin + static_cast<std::ptrdiff_t>(edit.Count));
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(edit.Start),
					edit.Replacement.begin(), edit.Replacement.end());
			}

			// 缺的顶层键/整块补在文件末尾:先给最后一行补行尾符(与全量序列化"末尾一定有
			// 换行"的口径一致),再追加。
			if (!appended.empty())
			{
				if (!lines.empty() && lines.back().Eol.empty())
					lines.back().Eol = eol;
				lines.insert(lines.end(), appended.begin(), appended.end());
			}
			return JoinLines(lines);
		}

		bool ReadTextFile(const std::filesystem::path& path, std::string* out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			*out = buffer.str();
			return true;
		}

		// 合并的前提是既有内容真的是一份清单(map):损坏文件或无关文件仍旧走全量序列化,
		// 免得写出"半旧半新"的混合体。
		bool IsYamlMapText(const std::string& text)
		{
			try
			{
				return YAML::Load(text).IsMap();
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		bool WriteTextFile(const std::filesystem::path& path, const std::string& text, std::string* error)
		{
			if (!path.parent_path().empty())
			{
				std::error_code ec;
				std::filesystem::create_directories(path.parent_path(), ec);
				if (ec)
				{
					if (error) *error = ec.message();
					return false;
				}
			}
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error) *error = "cannot open manifest for writing: " + path.string();
				return false;
			}
			stream << text;
			return static_cast<bool>(stream);
		}
	}

	bool ProjectManifest::Load(const std::filesystem::path& path, ProjectManifest* out, std::string* error)
	{
		if (!out)
		{
			if (error) *error = "null output manifest";
			return false;
		}
		try
		{
			const YAML::Node root = YAML::LoadFile(path.string());
			if (!root || !root.IsMap())
			{
				if (error) *error = "manifest root must be a map: " + path.string();
				return false;
			}
			ProjectManifest manifest;
			manifest.Id = root["id"] ? root["id"].as<std::string>("") : "";
			manifest.Version = root["version"] ? root["version"].as<std::string>("1.0.0") : "1.0.0";
			manifest.ContentRoot = root["content_root"] ? root["content_root"].as<std::string>("assets") : "assets";
			manifest.StartScene = root["start_scene"] ? root["start_scene"].as<std::string>("") : "";
			manifest.Renderer = root["renderer"] ? root["renderer"].as<std::string>("opengl") : "opengl";
			if (const YAML::Node packages = root["packages"])
				for (const YAML::Node& item : packages)
					manifest.Packages.push_back(item.as<std::string>(""));
			if (const YAML::Node rendering = root["rendering"])
			{
				if (!rendering.IsMap())
				{
					if (error) *error = "manifest 'rendering' must be a map: " + path.string();
					return false;
				}
				auto readBool = [&rendering](const char* key, bool fallback)
				{
					return rendering[key] ? rendering[key].as<bool>(fallback) : fallback;
				};
				auto readU32 = [&rendering](const char* key, uint32_t fallback)
				{
					return rendering[key] ? rendering[key].as<uint32_t>(fallback) : fallback;
				};
				manifest.Rendering.Culling = readBool("culling", manifest.Rendering.Culling);
				manifest.Rendering.Shadows = readBool("shadows", manifest.Rendering.Shadows);
				manifest.Rendering.ShadowMapSize = readU32("shadow_map_size", manifest.Rendering.ShadowMapSize);
				manifest.Rendering.MaxDirectionalLights =
					readU32("max_directional_lights", manifest.Rendering.MaxDirectionalLights);
				manifest.Rendering.MaxPointLights =
					readU32("max_point_lights", manifest.Rendering.MaxPointLights);
				manifest.Rendering.GpuTiming = readBool("gpu_timing", manifest.Rendering.GpuTiming);
				manifest.Rendering.Vsync = readBool("vsync", manifest.Rendering.Vsync);
				manifest.Rendering.Instancing = readBool("instancing", manifest.Rendering.Instancing);
				manifest.Rendering.Anisotropy = readU32("anisotropy", manifest.Rendering.Anisotropy);
				manifest.Rendering.Msaa = readU32("msaa", manifest.Rendering.Msaa);
				if (rendering["render_scale"])
					manifest.Rendering.RenderScale =
						rendering["render_scale"].as<float>(manifest.Rendering.RenderScale);
			}
			if (const YAML::Node physics = root["physics"])
			{
				if (!physics.IsMap())
				{
					if (error) *error = "manifest 'physics' must be a map: " + path.string();
					return false;
				}
				if (physics["fixed_step_hz"])
					manifest.Physics.FixedStepHz = physics["fixed_step_hz"].as<uint32_t>(manifest.Physics.FixedStepHz);
				if (physics["gravity"])
					manifest.Physics.Gravity = physics["gravity"].as<float>(manifest.Physics.Gravity);
			}
			if (!ValidateManifest(manifest, error))
				return false;
			*out = std::move(manifest);
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}

	bool ProjectManifest::Save(const std::filesystem::path& path, const ProjectManifest& manifest, std::string* error)
	{
		ProjectManifest copy = manifest;
		if (!ValidateManifest(copy, error))
			return false;
		try
		{
			// U2e:文件已存在时只做 key 级文本合并(保留注释/排版/数值写法);
			// 文件不存在或为空时走下面的全量序列化(与旧行为逐字节一致)。
			std::string existing;
			if (ReadTextFile(path, &existing) && !existing.empty() && IsYamlMapText(existing))
				return WriteTextFile(path, MergeManifestText(existing, copy), error);

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "id" << YAML::Value << copy.Id;
			out << YAML::Key << "version" << YAML::Value << copy.Version;
			out << YAML::Key << "content_root" << YAML::Value << copy.ContentRoot.generic_string();
			out << YAML::Key << "start_scene" << YAML::Value << copy.StartScene;
			out << YAML::Key << "renderer" << YAML::Value << copy.Renderer;
			out << YAML::Key << "rendering" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "culling" << YAML::Value << copy.Rendering.Culling;
			out << YAML::Key << "shadows" << YAML::Value << copy.Rendering.Shadows;
			out << YAML::Key << "shadow_map_size" << YAML::Value << copy.Rendering.ShadowMapSize;
			out << YAML::Key << "max_directional_lights" << YAML::Value << copy.Rendering.MaxDirectionalLights;
			out << YAML::Key << "max_point_lights" << YAML::Value << copy.Rendering.MaxPointLights;
			out << YAML::Key << "gpu_timing" << YAML::Value << copy.Rendering.GpuTiming;
			out << YAML::Key << "vsync" << YAML::Value << copy.Rendering.Vsync;
			out << YAML::Key << "instancing" << YAML::Value << copy.Rendering.Instancing;
			out << YAML::Key << "anisotropy" << YAML::Value << copy.Rendering.Anisotropy;
			out << YAML::Key << "msaa" << YAML::Value << copy.Rendering.Msaa;
			out << YAML::Key << "render_scale" << YAML::Value << copy.Rendering.RenderScale;
			out << YAML::EndMap;
			out << YAML::Key << "physics" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "fixed_step_hz" << YAML::Value << copy.Physics.FixedStepHz;
			out << YAML::Key << "gravity" << YAML::Value << copy.Physics.Gravity;
			out << YAML::EndMap;
			out << YAML::Key << "packages" << YAML::Value << YAML::BeginSeq;
			for (const std::string& package : copy.Packages)
				out << package;
			out << YAML::EndSeq;
			out << YAML::EndMap;

			// YAML 文件保持行尾换行(git 差异干净)。
			return WriteTextFile(path, std::string(out.c_str()) + '\n', error);
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}

	std::filesystem::path ProjectManifest::ResolveContentRoot(const std::filesystem::path& manifestPath) const
	{
		const std::filesystem::path base = std::filesystem::absolute(manifestPath).parent_path();
		return (base / ContentRoot).lexically_normal();
	}

	bool ProjectManifest::Locate(const std::filesystem::path& workingDirectory, std::filesystem::path* manifestPath)
	{
		std::error_code ec;
		for (const char* candidate : { "project.we.yaml", "Game/project.we.yaml" })
		{
			const std::filesystem::path full = workingDirectory / candidate;
			if (std::filesystem::is_regular_file(full, ec))
			{
				if (manifestPath)
					*manifestPath = full;
				return true;
			}
		}
		return false;
	}
}
