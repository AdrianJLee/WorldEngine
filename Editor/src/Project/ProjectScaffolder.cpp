#include "wldpch.h"
#include "ProjectScaffolder.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Core.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Script/HotReload.h"
#include "World/WUI/WuiLocalization.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

namespace World::Editor
{
	namespace
	{
		namespace fs = std::filesystem;

		// 标准项目清单的固定字段(其余**设置**字段按"默认项目同口径"填充,见
		// ApplyStandardSettingDefaults;示例内容永远不来自默认项目)。
		constexpr const char* kStandardVersion = "1.0.0";
		constexpr const char* kStandardContentRoot = "assets";
		constexpr const char* kStandardStartScene = "scenes/Main.wd";

		constexpr const char* kManifestFileName = "project.we.yaml";
		constexpr const char* kMainSceneRelative = "assets/scenes/Main.wd";

		// 模板里必须存在的条目(模板缺失/被删空时向导给可读的行内错误,而不是生成
		// 一个残缺项目)。目录条目用尾部 '/' 表示。
		constexpr const char* kRequiredTemplateEntries[] = {
			"levels.welevel",
			"assets/input.weinput",
			"assets/materials/",
			"assets/models/",
			"assets/prefabs/",
			"assets/scenes/",
			"assets/scripts/templates/WorldScript.lua",
			"assets/shaders/",
			"assets/textures/",
			"src/Components/",
			"src/Scripts/README.md",
		};

		fs::path RepoRoot()
		{
			// checkout 根锚点 = WLD_REPO_ROOT(编译期绝对路径,与进程 CWD 无关)。
			return fs::path(std::string(WLD_REPO_ROOT));
		}

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

		std::string RandomToken()
		{
			const uint64_t now = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			std::mt19937_64 generator(std::random_device{}() ^ now);
			char buffer[9] = {};
			std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned int>(generator() & 0xffffffffu));
			return buffer;
		}

		std::string WriteFailedText(const std::string& detail)
		{
			return Wui::Tr("modal.newproject.error.write_failed", "Could not create the project: ") + detail;
		}

		// 逐文件复制模板树:目录按需创建;文件用 copy_options::none —— 目标已存在即失败
		// (绝不覆盖既有文件,这是"逐文件不得覆盖"的第二道保险)。
		bool CopyTemplateTree(const fs::path& source, const fs::path& destination, std::string* error)
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
			try
			{
				for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source))
				{
					const fs::path relative = fs::relative(entry.path(), source, ec);
					if (ec || relative.empty())
					{
						if (error)
							*error = "cannot resolve template entry: " + entry.path().u8string();
						return false;
					}
					const fs::path target = destination / relative;
					if (entry.is_directory(ec))
					{
						fs::create_directories(target, ec);
						if (ec)
						{
							if (error)
								*error = "cannot create " + target.u8string() + " (" + ec.message() + ")";
							return false;
						}
						continue;
					}
					if (!entry.is_regular_file(ec))
						continue;   // 符号链接等一律跳过:模板只放普通文件/目录
					fs::create_directories(target.parent_path(), ec);
					std::error_code copyError;
					fs::copy_file(entry.path(), target, fs::copy_options::none, copyError);
					if (copyError)
					{
						if (error)
							*error = "cannot write " + target.u8string() + " (" + copyError.message() + ")";
						return false;
					}
				}
			}
			catch (const std::exception& exception)
			{
				if (error)
					*error = std::string("template copy failed: ") + exception.what();
				return false;
			}
			return true;
		}

		// 清单的 renderer / rendering / physics 取"默认项目同口径":默认项目的清单是这三个
		// **设置**字段的参考(读不到就退回 ProjectManifest 的引擎默认值)。注意这里只借
		// 设置值,绝不复制默认项目的任何内容/示例。
		void ApplyStandardSettingDefaults(Asset::ProjectManifest& manifest)
		{
			Asset::ProjectManifest reference;
			std::string error;
			const fs::path referencePath = RepoRoot() / "projects" / "default" / kManifestFileName;
			if (Asset::ProjectManifest::Load(referencePath, &reference, &error))
			{
				manifest.Renderer = reference.Renderer;
				manifest.Rendering = reference.Rendering;
				manifest.Physics = reference.Physics;
			}
		}

		bool WriteStandardManifest(const fs::path& projectRoot, const std::string& name, std::string* error)
		{
			Asset::ProjectManifest manifest;
			manifest.Id = ProjectScaffolder::ManifestId(name);
			manifest.Version = kStandardVersion;
			manifest.ContentRoot = kStandardContentRoot;
			manifest.StartScene = kStandardStartScene;
			ApplyStandardSettingDefaults(manifest);
			manifest.Packages.clear();   // 新项目还没有发行包(默认项目的 Base.wpak 是引擎内容)
			return Asset::ProjectManifest::Save(projectRoot / kManifestFileName, manifest, error);
		}

		// 空场景走引擎序列化(不手写 YAML):Scene 需要一个 WorldContext,序列化完即弃。
		bool WriteEmptyScene(const fs::path& projectRoot, WorldContext& context, std::string* error)
		{
			const fs::path scenePath = projectRoot / kMainSceneRelative;
			std::error_code ec;
			fs::create_directories(scenePath.parent_path(), ec);
			if (ec)
			{
				if (error)
					*error = "cannot create " + scenePath.parent_path().u8string() + " (" + ec.message() + ")";
				return false;
			}
			const Ref<Scene> scene = CreateRef<Scene>(context);
			SceneSerializer serializer(scene);
			if (!serializer.Serialize(scenePath.string()))
			{
				if (error)
				{
					const std::string reason = serializer.GetLastError();
					*error = "scene serialization failed: " + scenePath.u8string()
						+ (reason.empty() ? std::string() : " (" + reason + ")");
				}
				return false;
			}
			return true;
		}

		std::vector<std::string> CollectRelativeFiles(const fs::path& root)
		{
			std::vector<std::string> files;
			std::error_code ec;
			try
			{
				fs::recursive_directory_iterator iterator(root, ec);
				if (ec)
					return files;
				const fs::recursive_directory_iterator end;
				while (iterator != end)
				{
					const fs::directory_entry& entry = *iterator;
					std::error_code typeError;
					if (entry.is_regular_file(typeError))
						files.push_back(fs::relative(entry.path(), root).generic_u8string());
					iterator.increment(ec);
					if (ec)
						break;
				}
			}
			catch (const std::exception&)
			{
				// 收集失败不影响创建结果(只是报告里的文件清单不完整)。
			}
			std::sort(files.begin(), files.end());
			return files;
		}
	}

	fs::path ProjectScaffolder::TemplateRoot()
	{
		return RepoRoot() / "templates" / "project";
	}

	fs::path ProjectScaffolder::DefaultProjectLocation()
	{
		if (const char* profile = std::getenv("USERPROFILE"))
		{
			std::error_code ec;
			const fs::path documents = fs::u8path(profile) / "Documents";
			if (fs::is_directory(documents, ec))
				return documents;
		}
		std::error_code ec;
		return fs::current_path(ec);
	}

	std::string ProjectScaffolder::ValidateProjectName(const std::string& rawName)
	{
		const std::string name = Trimmed(rawName);
		if (name.empty())
			return Wui::Tr("modal.newproject.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("modal.newproject.name.invalid",
				"Name cannot be used as a directory name: no \\ / : * ? \" < > | characters "
				"and no trailing dot or space");
		for (const char character : name)
		{
			const unsigned char c = static_cast<unsigned char>(character);
			if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|')
				return Wui::Tr("modal.newproject.name.invalid",
					"Name cannot be used as a directory name: no \\ / : * ? \" < > | characters "
					"and no trailing dot or space");
		}
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("modal.newproject.name.invalid",
				"Name cannot be used as a directory name: no \\ / : * ? \" < > | characters "
				"and no trailing dot or space");
		if (IsReservedDeviceName(name))
			return Wui::Tr("modal.newproject.name.reserved",
				"This name is a reserved Windows device name (CON/NUL/COM1…); pick another one");
		return {};
	}

	std::string ProjectScaffolder::ValidateLocation(const fs::path& location, const std::string& rawName)
	{
		if (location.empty())
			return Wui::Tr("modal.newproject.error.location_empty", "Pick a location for the project");
		std::error_code ec;
		if (!fs::is_directory(location, ec))
			return Wui::TrFormat("modal.newproject.error.location_missing",
				"The location must be an existing directory: {path}", { { "path", location.u8string() } });

		const std::string name = Trimmed(rawName);
		if (!ValidateProjectName(name).empty())
			return {};   // 名称不合法时只报名称错(落点由名称决定,不重复报)
		const fs::path target = location / fs::u8path(name);
		std::error_code existsError;
		if (fs::exists(target, existsError))
		{
			if (!fs::is_directory(target, existsError) || !fs::is_empty(target, existsError))
				return Wui::TrFormat("modal.newproject.error.target_exists",
					"The target directory already exists and is not empty: {path}",
					{ { "path", target.u8string() } });
		}
		return {};
	}

	std::string ProjectScaffolder::ValidateTemplate()
	{
		const fs::path root = TemplateRoot();
		std::error_code ec;
		if (!fs::is_directory(root, ec))
			return Wui::TrFormat("modal.newproject.error.template_missing",
				"Project template is missing: {path}", { { "path", root.u8string() } });
		for (const char* entry : kRequiredTemplateEntries)
		{
			const bool wantsDirectory = entry[std::strlen(entry) - 1] == '/';
			const fs::path path = root / fs::u8path(wantsDirectory ? std::string(entry, std::strlen(entry) - 1)
				: std::string(entry));
			std::error_code entryError;
			const bool present = wantsDirectory ? fs::is_directory(path, entryError)
				: fs::is_regular_file(path, entryError);
			if (!present)
				return Wui::TrFormat("modal.newproject.error.template_missing",
					"Project template is missing: {path}", { { "path", path.u8string() } });
		}
		return {};
	}

	std::string ProjectScaffolder::ManifestId(const std::string& rawName)
	{
		const std::string name = Trimmed(rawName);
		std::string stem;
		for (const char character : name)
		{
			const unsigned char c = static_cast<unsigned char>(character);
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				stem.push_back(static_cast<char>(std::tolower(c)));
			else if (c == '_' || c == '-')
				stem.push_back(static_cast<char>(c));
			else if (!stem.empty() && stem.back() != '.')
				stem.push_back('.');
		}
		while (!stem.empty() && stem.back() == '.')
			stem.pop_back();
		if (stem.empty())
			stem = "project";   // 纯非 ASCII 名称:给一个稳定的兜底 id(不含任何示例字样)
		return "com." + stem;
	}

	ProjectScaffolder::Result ProjectScaffolder::Create(const fs::path& location, const std::string& rawName,
		WorldContext& context)
	{
		Result result;
		const auto fail = [&result](const std::string& message)
		{
			result.Ok = false;
			result.Error = message;
			return result;
		};
		const auto writeFailed = [](const std::string& detail)
		{
			return WriteFailedText(detail);
		};

		const std::string name = Trimmed(rawName);
		const std::string nameError = ValidateProjectName(name);
		if (!nameError.empty())
			return fail(nameError);
		const std::string locationError = ValidateLocation(location, name);
		if (!locationError.empty())
			return fail(locationError);
		const std::string templateError = ValidateTemplate();
		if (!templateError.empty())
			return fail(templateError);

		std::error_code ec;
		const fs::path locationAbs = fs::absolute(location, ec);
		if (ec)
			return fail(writeFailed("cannot resolve the location " + location.u8string()
				+ " (" + ec.message() + ")"));
		const fs::path target = (locationAbs / fs::u8path(name)).lexically_normal();

		// 目标:不存在,或存在但为空目录(空目录在改名阶段先移除 —— rename 不能落到
		// 一个已存在的目录上)。
		bool targetExists = false;
		std::error_code existsError;
		if (fs::exists(target, existsError))
		{
			if (!fs::is_directory(target, existsError) || !fs::is_empty(target, existsError))
				return fail(Wui::TrFormat("modal.newproject.error.target_exists",
					"The target directory already exists and is not empty: {path}",
					{ { "path", target.u8string() } }));
			targetExists = true;
		}

		// 临时目录与目标同父目录(同卷 → 改名是"整体切换",不会留下半成品)。
		fs::path temporary;
		for (int attempt = 0; attempt < 16; ++attempt)
		{
			fs::path candidate = locationAbs / fs::u8path(name + ".tmp-" + RandomToken());
			std::error_code candidateError;
			if (!fs::exists(candidate, candidateError))
			{
				temporary = candidate;
				break;
			}
		}
		if (temporary.empty())
			return fail(writeFailed("cannot find a free temporary directory next to " + target.u8string()));

		std::error_code createError;
		fs::create_directories(temporary, createError);
		if (createError)
		{
			std::error_code cleanupError;
			fs::remove_all(temporary, cleanupError);
			return fail(writeFailed("cannot create the temporary directory " + temporary.u8string()
				+ " (" + createError.message() + "); is the location writable?"));
		}

		std::string error;
		bool built = CopyTemplateTree(TemplateRoot(), temporary, &error);
		if (built)
			built = WriteStandardManifest(temporary, name, &error);
		if (built)
			built = WriteEmptyScene(temporary, context, &error);
		if (built)
		{
			// Luau LSP 脚手架(.vscode/settings.json + .luau-lsp/config.json)走引擎既有实现:
			// 与编辑器启动期为当前项目生成的那两份逐字节同源(模板目录不放它们 —— 仓库的
			// 裸 `.vscode` 忽略规则会吞掉模板里的同名文件)。
			built = World::EnsureScriptEditorScaffold(temporary, &error);
		}
		if (built && targetExists)
		{
			std::error_code removeError;
			fs::remove(target, removeError);
			if (removeError)
			{
				built = false;
				error = "cannot replace the empty target directory " + target.u8string()
					+ " (" + removeError.message() + ")";
			}
		}
		if (built)
		{
			std::error_code renameError;
			fs::rename(temporary, target, renameError);
			if (renameError)
			{
				built = false;
				error = "cannot move the scaffold into place (" + renameError.message() + ")";
			}
		}
		if (!built)
		{
			std::error_code cleanupError;
			fs::remove_all(temporary, cleanupError);
			WLD_CORE_WARN("[project-scaffolder] create failed at '{0}': {1}", target.u8string(), error);
			return fail(writeFailed(error));
		}

		result.Ok = true;
		result.ProjectRoot = target;
		result.Files = CollectRelativeFiles(target);
		WLD_CORE_INFO("[project-scaffolder] created '{0}' ({1} files, no samples)", target.u8string(),
			result.Files.size());
		return result;
	}
}
