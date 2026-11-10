#include "wldpch.h"
#include "Project/PluginScaffolder.h"

#include "Project/ProjectScaffolder.h"

#include "World/Plugins/PluginManifest.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <system_error>

namespace World::Editor
{
	namespace
	{
		namespace fs = std::filesystem;

		// 模板库 = <checkout>/templates/plugin-<id>/(与 project-<id> 并列)。
		constexpr const char* kTemplateManifestName = "template.json";
		constexpr const char* kTemplateDirectoryPrefix = "plugin-";
		constexpr const char* kManifestFileName = "plugin.we.yaml";
		// 模板里必须存在的条目(目录条目用尾部 '/' 表示)。坏模板也列出来,
		// 选中时给可读的行内错误 —— 但绝不生成一个缺文件、编不过的插件包。
		constexpr const char* kRequiredTemplateEntries[] = {
			"plugin.we.yaml",
			"README.md",
			"src/plugin.cpp",
			"CMakeLists.txt",
		};

		std::string Trimmed(const std::string& text)
		{
			size_t begin = 0;
			while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			size_t end = text.size();
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			return text.substr(begin, end - begin);
		}

		bool EqualsIgnoreCase(const std::string& text, const char* other)
		{
			size_t index = 0;
			for (; index < text.size() && other[index] != '\0'; ++index)
			{
				const unsigned char a = static_cast<unsigned char>(text[index]);
				const unsigned char b = static_cast<unsigned char>(other[index]);
				if (std::tolower(a) != std::tolower(b))
					return false;
			}
			return index == text.size() && other[index] == '\0';
		}

		// Windows 保留设备名:这些名字(或其带扩展名的形式)不能作为目录名。
		bool IsReservedDeviceName(const std::string& name)
		{
			std::string stem = name;
			if (const size_t dot = stem.find('.'); dot != std::string::npos)
				stem = stem.substr(0, dot);
			static const char* const kReserved[] = {
				"CON", "PRN", "AUX", "NUL",
				"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
				"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
			};
			for (const char* candidate : kReserved)
				if (EqualsIgnoreCase(stem, candidate))
					return true;
			return false;
		}

		// 模板 id 直接拼进目录名(plugin-<id>)与向导的无障碍 id(plugin.new.template.<id>):
		// 只接受一段安全 ASCII 名字 —— 路径分隔符 / ".." / 空段一律拒绝(不做路径拼接)。
		bool IsValidTemplateId(const std::string& id)
		{
			if (id.empty() || id == "." || id == "..")
				return false;
			if (std::isalnum(static_cast<unsigned char>(id.front())) == 0)
				return false;
			for (const char character : id)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (std::isalnum(c) == 0 && c != '.' && c != '_' && c != '-')
					return false;
			}
			return true;
		}

		bool IsTemplateDirectoryName(const std::string& name)
		{
			const size_t prefixLength = std::strlen(kTemplateDirectoryPrefix);
			return name.size() > prefixLength
				&& name.compare(0, prefixLength, kTemplateDirectoryPrefix) == 0;
		}

		Plugins::PluginScope ScopeFor(PluginScaffolder::PluginTarget target)
		{
			return target == PluginScaffolder::PluginTarget::Project
				? Plugins::PluginScope::Project : Plugins::PluginScope::Engine;
		}

		// 读一个模板目录(template.json → PluginTemplateInfo):成功 = true(*info 填好);
		// 失败 = false,且 *error 是可读(已本地化)的原因。
		bool ReadTemplateInfo(const fs::path& directory, const std::string& id,
			PluginScaffolder::PluginTemplateInfo* info, std::string* error)
		{
			info->Id = id;
			info->Name = id;
			info->Directory = directory;
			info->Valid = true;

			std::error_code ec;
			if (!fs::is_directory(directory, ec))
			{
				if (error)
					*error = Wui::TrFormat("modal.newplugin.error.template_missing",
						"Plugin template is missing: {path}", { { "path", directory.u8string() } });
				return false;
			}
			const fs::path manifestPath = directory / kTemplateManifestName;
			if (!fs::is_regular_file(manifestPath, ec))
			{
				if (error)
					*error = Wui::TrFormat("modal.newplugin.error.template_manifest_missing",
						"Plugin template has no template.json: {path}",
						{ { "path", manifestPath.u8string() } });
				return false;
			}
			const auto invalid = [&manifestPath, error](const std::string& reason)
			{
				if (error)
					*error = Wui::TrFormat("modal.newplugin.error.template_invalid",
						"Cannot read the plugin template: {path} ({reason})",
						{ { "path", manifestPath.u8string() }, { "reason", reason } });
				return false;
			};

			std::ifstream stream(manifestPath, std::ios::binary);
			if (!stream)
				return invalid("cannot open the file");
			const std::string text((std::istreambuf_iterator<char>(stream)),
				std::istreambuf_iterator<char>());
			std::string parseError;
			const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(text, &parseError);
			if (!root || root->type != Wui::JsonValue::Type::Object)
				return invalid(parseError.empty() ? std::string("not a JSON object") : parseError);
			// 目录名是 id 的事实源(目录唯一);template.json 里写了 id 就必须与它一致。
			if (const Wui::JsonValue* declaredId = root->Find("id"))
			{
				const std::string value = declaredId->AsString();
				if (!value.empty() && value != id)
					return invalid("id '" + value + "' does not match the directory name '"
						+ std::string(kTemplateDirectoryPrefix) + id + "'");
			}
			if (const Wui::JsonValue* name = root->Find("name"))
			{
				const std::string value = name->AsString();
				if (!value.empty())
					info->Name = value;
			}
			if (const Wui::JsonValue* description = root->Find("description"))
				info->Description = description->AsString();
			if (const Wui::JsonValue* requires = root->Find("requires"))
			{
				const std::string value = Trimmed(requires->AsString());
				if (!value.empty())
					info->Requires = value;
			}
			if (info->Requires.empty())
				info->Requires = "none";
			if (const Wui::JsonValue* order = root->Find("order"))
				if (order->type == Wui::JsonValue::Type::Number)
					info->Order = static_cast<int>(order->Number);
			// scopes: 缺省 = engine + project 都允许;写了就按写的来(未知 scope 忽略)。
			if (const Wui::JsonValue* scopes = root->Find("scopes"))
			{
				if (scopes->type == Wui::JsonValue::Type::Array)
				{
					info->EngineOk = false;
					info->ProjectOk = false;
					for (const Wui::JsonValue& scope : scopes->Array)
					{
						const std::string value = Trimmed(scope.AsString());
						if (value == "engine")
							info->EngineOk = true;
						else if (value == "project")
							info->ProjectOk = true;
					}
				}
			}
			return true;
		}

		// 模板缺必需文件 = 坏模板(可读原因里给出第一个缺失路径)。
		bool RequiredEntriesPresent(const fs::path& directory, std::string* missingPath)
		{
			for (const char* entry : kRequiredTemplateEntries)
			{
				const fs::path path = directory / fs::u8path(entry);
				std::error_code ec;
				if (!fs::is_regular_file(path, ec))
				{
					if (missingPath)
						*missingPath = path.u8string();
					return false;
				}
			}
			return true;
		}

		std::string RandomToken()
		{
			const uint64_t now = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			std::mt19937_64 generator(std::random_device{}() ^ now);
			char buffer[9] = {};
			std::snprintf(buffer, sizeof(buffer), "%08x",
				static_cast<unsigned int>(generator() & 0xffffffffu));
			return buffer;
		}

		std::string WriteFailedText(const std::string& detail)
		{
			return Wui::Tr("modal.newplugin.error.write_failed", "Could not create the plugin: ") + detail;
		}

		void ReplaceAll(std::string& text, const std::string& from, const std::string& to)
		{
			if (from.empty())
				return;
			size_t position = 0;
			while ((position = text.find(from, position)) != std::string::npos)
			{
				text.replace(position, from.size(), to);
				position += to.size();
			}
		}

		// 逐文件复制模板树 + 4 个占位符的**字面替换**(不做 YAML 重写):
		// {{PluginId}} / {{PluginName}} / {{PluginDir}} / {{PluginScope}}。
		// 文件用 trunc 写法,但因为整棵树落在刚创建的空临时目录里,
		// 目标已存在 = 前面 create_directories 就失败(不覆盖任何既有文件)。
		bool CopyTemplateTree(const fs::path& source, const fs::path& destination,
			const std::vector<std::pair<std::string, std::string>>& replacements,
			std::vector<std::string>* files, std::string* error)
		{
			std::error_code ec;
			if (!fs::is_directory(source, ec))
			{
				if (error)
					*error = "template directory is missing: " + source.u8string();
				return false;
			}
			fs::create_directories(destination, ec);
			if (ec)
			{
				if (error)
					*error = "cannot create " + destination.u8string() + " (" + ec.message() + ")";
				return false;
			}

			fs::recursive_directory_iterator iterator(source, ec);
			const fs::recursive_directory_iterator end;
			if (ec)
			{
				if (error)
					*error = "cannot scan " + source.u8string() + " (" + ec.message() + ")";
				return false;
			}
			while (iterator != end)
			{
				const fs::directory_entry entry = *iterator;
				const fs::path relative = entry.path().lexically_relative(source);
				if (relative.empty() || relative.begin()->string() == "..")
				{
					if (error)
						*error = "template entry escapes the template directory: "
							+ entry.path().u8string();
					return false;
				}
				// template.json 只是模板元数据(列出模板/占位符契约用),不复制进插件包 ——
				// 生成物只有"清单 + 源码 + CMake + README"这几件用户该看到的东西。
				const bool isTemplateManifest = relative == fs::path(kTemplateManifestName);
				std::error_code typeError;
				if (!isTemplateManifest && entry.is_directory(typeError))
				{
					fs::create_directories(destination / relative, ec);
					if (ec)
					{
						if (error)
							*error = "cannot create " + (destination / relative).u8string()
								+ " (" + ec.message() + ")";
						return false;
					}
				}
				else if (!isTemplateManifest && entry.is_regular_file(typeError))
				{
					std::ifstream input(entry.path(), std::ios::binary);
					if (!input)
					{
						if (error)
							*error = "cannot read " + entry.path().u8string();
						return false;
					}
					std::string text((std::istreambuf_iterator<char>(input)),
						std::istreambuf_iterator<char>());
					for (const auto& replacement : replacements)
						ReplaceAll(text, replacement.first, replacement.second);

					const fs::path outputPath = destination / relative;
					const fs::path parent = outputPath.parent_path();
					if (!parent.empty())
					{
						fs::create_directories(parent, ec);
						if (ec)
						{
							if (error)
								*error = "cannot create " + parent.u8string() + " (" + ec.message() + ")";
							return false;
						}
					}
					std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
					if (!output)
					{
						if (error)
							*error = "cannot write " + outputPath.u8string();
						return false;
					}
					output.write(text.data(), static_cast<std::streamsize>(text.size()));
					if (!output.good())
					{
						if (error)
							*error = "failed to write " + outputPath.u8string();
						return false;
					}
					if (files)
						files->push_back(relative.generic_u8string());
				}
				iterator.increment(ec);
				if (ec)
				{
					if (error)
						*error = "cannot scan " + source.u8string() + " (" + ec.message() + ")";
					return false;
				}
			}
			return true;
		}

		// 已有插件里是否已用同一个 id(位置即 scope:先按引擎解析,清单写了
		// `scope: project` 时再按项目解析一次 —— 不猜 id,只信清单)。
		bool FindExistingPluginId(const fs::path& manifestPath, std::string* outId)
		{
			Plugins::PluginManifest manifest;
			std::string error;
			if (!Plugins::PluginManifest::Load(manifestPath, Plugins::PluginScope::Engine,
					&manifest, &error))
			{
				if (!Plugins::PluginManifest::Load(manifestPath, Plugins::PluginScope::Project,
						&manifest, &error))
					return false;
			}
			if (outId)
				*outId = manifest.Id;
			return !manifest.Id.empty();
		}
	}

	const char* PluginScaffolder::TargetName(PluginTarget target)
	{
		return target == PluginTarget::Project ? "project" : "engine";
	}

	fs::path PluginScaffolder::TemplateRoot()
	{
		// 与项目模板同一条口径(单一定义在 ProjectScaffolder):
		// <checkout>/templates(WLD_REPO_ROOT 锚定,与进程 CWD 无关)。
		return ProjectScaffolder::TemplateRoot();
	}

	fs::path PluginScaffolder::TemplateDirectory(const std::string& templateId)
	{
		if (!IsValidTemplateId(templateId))
			return {};
		return TemplateRoot() / (std::string(kTemplateDirectoryPrefix) + templateId);
	}

	std::vector<PluginScaffolder::PluginTemplateInfo> PluginScaffolder::ListTemplates()
	{
		std::vector<PluginTemplateInfo> templates;
		std::error_code ec;
		const fs::path root = TemplateRoot();
		fs::directory_iterator iterator(root, ec);
		if (ec)
			return templates;   // 模板根不存在/读不了 = 空库(ValidateTemplate 给可读原因)
		const fs::directory_iterator end;
		while (iterator != end)
		{
			const fs::directory_entry& entry = *iterator;
			std::error_code typeError;
			if (entry.is_directory(typeError))
			{
				const std::string directoryName = entry.path().filename().u8string();
				if (IsTemplateDirectoryName(directoryName))
				{
					PluginTemplateInfo info;
					const std::string id =
						directoryName.substr(std::strlen(kTemplateDirectoryPrefix));
					std::string error;
					if (!IsValidTemplateId(id))
					{
						info.Id = id;
						info.Name = id;
						info.Directory = entry.path();
						info.Valid = false;
						info.Error = Wui::TrFormat("modal.newplugin.error.template_id_invalid",
							"Invalid plugin template id (letters, digits, '.', '_' and '-' only): {id}",
							{ { "id", id } });
					}
					else if (!ReadTemplateInfo(entry.path(), id, &info, &error))
					{
						info.Valid = false;
						info.Error = error;
					}
					else
					{
						std::string missing;
						if (!RequiredEntriesPresent(entry.path(), &missing))
						{
							info.Valid = false;
							info.Error = Wui::TrFormat("modal.newplugin.error.template_missing",
								"Plugin template is missing: {path}", { { "path", missing } });
						}
					}
					templates.push_back(std::move(info));
				}
			}
			iterator.increment(ec);
			if (ec)
				break;
		}
		std::sort(templates.begin(), templates.end(),
			[](const PluginTemplateInfo& a, const PluginTemplateInfo& b)
			{
				if (a.Order != b.Order)
					return a.Order < b.Order;
				return a.Id < b.Id;
			});
		return templates;
	}

	std::string PluginScaffolder::ValidatePluginName(const std::string& rawName)
	{
		const std::string name = Trimmed(rawName);
		if (name.empty())
			return Wui::Tr("modal.newplugin.error.name.empty", "Name cannot be empty");
		if (name.size() > 64)
			return Wui::Tr("modal.newplugin.error.name.too_long",
				"Name is too long (64 characters maximum)");
		// 目录名同时是 CMake 目标名(WePlugin_<目录名>)与产物名(<目录名>.dll):
		// 只接受 ASCII 字母/数字/'-'/'_',从源头保证生成的 CMakeLists.txt 可配置。
		for (const char character : name)
		{
			const unsigned char c = static_cast<unsigned char>(character);
			if (std::isalnum(c) == 0 && c != '-' && c != '_')
				return Wui::Tr("modal.newplugin.error.name.invalid",
					"Name can only contain letters, digits, '-' and '_' (it is the directory "
					"and the CMake target name)");
		}
		if (IsReservedDeviceName(name))
			return Wui::Tr("modal.newplugin.error.name.reserved",
				"This name is a reserved Windows device name (CON/NUL/COM1…); pick another one");
		return {};
	}

	std::string PluginScaffolder::ValidatePluginId(const std::string& rawId)
	{
		const std::string id = Trimmed(rawId);
		if (id.empty())
			return Wui::Tr("modal.newplugin.error.id.empty", "Plugin ID cannot be empty");
		if (id.size() > 128)
			return Wui::Tr("modal.newplugin.error.id.too_long",
				"Plugin ID is too long (128 characters maximum)");
		// 反域名形态:至少两段(a.b);小写字母/数字/'-';每段以字母或数字开头结尾。
		size_t segments = 0;
		size_t segmentStart = 0;
		for (size_t index = 0; index <= id.size(); ++index)
		{
			const bool atEnd = index == id.size();
			const char character = atEnd ? '.' : id[index];
			if (character == '.')
			{
				const size_t length = index - segmentStart;
				if (length == 0)
					return Wui::Tr("modal.newplugin.error.id.invalid",
						"Plugin ID must be a reverse-domain name (lowercase letters, digits, '-' "
						"and '.', e.g. com.studio.my-plugin)");
				const char first = id[segmentStart];
				const char last = id[index - 1];
				const auto isAlphaNumeric = [](char value)
				{
					const unsigned char c = static_cast<unsigned char>(value);
					return std::isdigit(c) != 0
						|| (c >= 'a' && c <= 'z');
				};
				if (!isAlphaNumeric(first) || !isAlphaNumeric(last))
					return Wui::Tr("modal.newplugin.error.id.invalid",
						"Plugin ID must be a reverse-domain name (lowercase letters, digits, '-' "
						"and '.', e.g. com.studio.my-plugin)");
				for (size_t label = segmentStart; label < index; ++label)
				{
					const unsigned char c = static_cast<unsigned char>(id[label]);
					if (std::isdigit(c) == 0 && c != '-' && (c < 'a' || c > 'z'))
						return Wui::Tr("modal.newplugin.error.id.invalid",
							"Plugin ID must be a reverse-domain name (lowercase letters, digits, "
							"'-' and '.', e.g. com.studio.my-plugin)");
				}
				++segments;
				segmentStart = index + 1;
			}
		}
		if (segments < 2)
			return Wui::Tr("modal.newplugin.error.id.not_reverse_domain",
				"Plugin ID needs at least two dot-separated labels (reverse-domain, "
				"e.g. com.studio.my-plugin)");
		return {};
	}

	std::string PluginScaffolder::ValidateTarget(const fs::path& pluginsRoot,
		const std::string& rawName, const std::string& rawPluginId)
	{
		if (pluginsRoot.empty())
			return Wui::Tr("modal.newplugin.error.root_missing",
				"Cannot resolve the plugins directory for this target");

		const std::string name = Trimmed(rawName);
		const std::string pluginId = Trimmed(rawPluginId);
		const std::string nameError = ValidatePluginName(name);
		if (!nameError.empty())
			return nameError;
		const std::string idError = ValidatePluginId(pluginId);
		if (!idError.empty())
			return idError;

		std::error_code ec;
		const fs::path target = (pluginsRoot / fs::u8path(name)).lexically_normal();
		if (fs::exists(target, ec))
			return Wui::TrFormat("modal.newplugin.error.target_exists",
				"Target directory already exists: {path}", { { "path", target.u8string() } });

		fs::directory_iterator iterator(pluginsRoot, ec);
		if (ec)
			return {};   // 插件根不存在 = 没有冲突(Scaffold 会创建它)
		const fs::directory_iterator end;
		while (iterator != end)
		{
			const fs::directory_entry& entry = *iterator;
			std::error_code typeError;
			if (entry.is_directory(typeError))
			{
				const fs::path manifestPath = entry.path() / kManifestFileName;
				std::error_code fileError;
				if (fs::is_regular_file(manifestPath, fileError))
				{
					std::string existingId;
					if (FindExistingPluginId(manifestPath, &existingId) && existingId == pluginId)
						return Wui::TrFormat("modal.newplugin.error.id_exists",
							"A plugin with ID '{id}' already exists: {path}",
							{ { "id", pluginId }, { "path", entry.path().u8string() } });
				}
			}
			iterator.increment(ec);
			if (ec)
				break;
		}
		return {};
	}

	std::string PluginScaffolder::ValidateTemplate(const std::string& rawTemplateId,
		PluginTarget target)
	{
		const std::string templateId = Trimmed(rawTemplateId);
		const fs::path root = TemplateRoot();
		std::error_code ec;
		if (!fs::is_directory(root, ec))
			return Wui::TrFormat("modal.newplugin.error.template_missing",
				"Plugin template is missing: {path}", { { "path", root.u8string() } });
		if (templateId.empty())
			return Wui::TrFormat("modal.newplugin.error.template_missing",
				"Plugin template is missing: {path}", { { "path", root.u8string() } });
		if (!IsValidTemplateId(templateId))
			return Wui::TrFormat("modal.newplugin.error.template_id_invalid",
				"Invalid plugin template id (letters, digits, '.', '_' and '-' only): {id}",
				{ { "id", templateId } });

		const fs::path directory = TemplateDirectory(templateId);
		PluginTemplateInfo info;
		std::string reason;
		if (!ReadTemplateInfo(directory, templateId, &info, &reason))
			return reason;
		std::string missing;
		if (!RequiredEntriesPresent(directory, &missing))
			return Wui::TrFormat("modal.newplugin.error.template_missing",
				"Plugin template is missing: {path}", { { "path", missing } });
		const bool allowed = target == PluginTarget::Engine ? info.EngineOk : info.ProjectOk;
		if (!allowed)
			return Wui::TrFormat("modal.newplugin.error.template_scope",
				"This template cannot be created as a {target} plugin",
				{ { "target", TargetName(target) } });
		return {};
	}

	PluginScaffolder::PluginScaffoldResult PluginScaffolder::Scaffold(
		const fs::path& pluginsRoot, const PluginScaffoldRequest& request)
	{
		PluginScaffoldResult result;
		const auto fail = [&result](const std::string& message)
		{
			result.Ok = false;
			result.Error = message;
			return result;
		};

		const std::string name = Trimmed(request.Name);
		const std::string pluginId = Trimmed(request.PluginId);
		if (pluginsRoot.empty())
			return fail(Wui::Tr("modal.newplugin.error.root_missing",
				"Cannot resolve the plugins directory for this target"));
		const std::string templateError = ValidateTemplate(request.TemplateId, request.Target);
		if (!templateError.empty())
			return fail(templateError);
		// ValidateTarget 内含名称/ID 校验(Scaffold 的第二道守卫,防止绕过按钮的调用)。
		const std::string targetError = ValidateTarget(pluginsRoot, name, pluginId);
		if (!targetError.empty())
			return fail(targetError);

		std::error_code ec;
		const fs::path rootAbsolute = fs::absolute(pluginsRoot, ec);
		if (ec)
			return fail(WriteFailedText("cannot resolve " + pluginsRoot.u8string()
				+ " (" + ec.message() + ")"));
		const fs::path target = (rootAbsolute / fs::u8path(name)).lexically_normal();

		// 临时目录与目标同父目录(同卷 → 改名是"整体切换",不会留下半成品)。
		fs::path temporary;
		for (int attempt = 0; attempt < 16; ++attempt)
		{
			const fs::path candidate =
				rootAbsolute / fs::u8path(name + ".tmp-" + RandomToken());
			std::error_code candidateError;
			if (!fs::exists(candidate, candidateError))
			{
				temporary = candidate;
				break;
			}
		}
		if (temporary.empty())
			return fail(WriteFailedText("cannot allocate a temporary directory in "
				+ rootAbsolute.u8string()));

		const auto cleanup = [&temporary]()
		{
			std::error_code cleanupError;
			fs::remove_all(temporary, cleanupError);
		};

		fs::create_directories(rootAbsolute, ec);
		if (ec)
			return fail(WriteFailedText("cannot create " + rootAbsolute.u8string()
				+ " (" + ec.message() + ")"));
		fs::create_directories(temporary, ec);
		if (ec)
			return fail(WriteFailedText("cannot create " + temporary.u8string()
				+ " (" + ec.message() + ")"));

		const std::vector<std::pair<std::string, std::string>> replacements = {
			{ "{{PluginId}}", pluginId },
			{ "{{PluginName}}", name },
			{ "{{PluginDir}}", name },
			{ "{{PluginScope}}", TargetName(request.Target) },
		};
		std::vector<std::string> files;
		std::string copyError;
		if (!CopyTemplateTree(TemplateDirectory(request.TemplateId), temporary, replacements,
				&files, &copyError))
		{
			cleanup();
			return fail(WriteFailedText(copyError));
		}

		// 发现期自校验:生成的 plugin.we.yaml 必须能被 PluginManifest::Load 接受
		// (否则"文件落盘了但插件永远被发现期拒绝"是更坏的失败 —— 宁可回滚)。
		Plugins::PluginManifest manifest;
		std::string manifestError;
		if (!Plugins::PluginManifest::Load(temporary / kManifestFileName,
				ScopeFor(request.Target), &manifest, &manifestError))
		{
			cleanup();
			return fail(Wui::TrFormat("modal.newplugin.error.manifest_invalid",
				"The generated plugin.we.yaml was rejected by the loader: {reason}",
				{ { "reason", manifestError } }));
		}

		fs::rename(temporary, target, ec);
		if (ec)
		{
			cleanup();
			return fail(WriteFailedText("cannot move the new plugin into place ("
				+ target.u8string() + "): " + ec.message()));
		}

		std::sort(files.begin(), files.end());
		files.erase(std::unique(files.begin(), files.end()), files.end());
		result.Ok = true;
		result.Error.clear();
		result.PluginRoot = target;
		result.Files = std::move(files);
		return result;
	}
}
