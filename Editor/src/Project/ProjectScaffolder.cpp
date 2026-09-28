#include "wldpch.h"
#include "ProjectScaffolder.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Core.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Script/HotReload.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

		// 标准项目清单的固定字段(其余**设置**字段按"默认项目同口径"填充,见
		// ApplyStandardSettingDefaults;示例内容永远不来自默认项目)。
		constexpr const char* kStandardVersion = "1.0.0";
		constexpr const char* kStandardContentRoot = "assets";
		constexpr const char* kStandardStartScene = "scenes/Main.wd";

		constexpr const char* kManifestFileName = "project.we.yaml";
		constexpr const char* kMainSceneRelative = "assets/scenes/Main.wd";

		// PROJ-7/T2:模板库 = <checkout>/templates/project-<id>/,每个模板一个
		// template.json(id/name/description/order/defaultScene)。
		constexpr const char* kTemplateManifestName = "template.json";
		constexpr const char* kTemplateDirectoryPrefix = "project-";

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

		// 模板 id 直接拼进目录名(project-<id>)与向导的无障碍 id(project.new.template.<id>):
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

		// 读一个模板目录(template.json → TemplateInfo):成功 = true(*info 填好且 Valid=true);
		// 失败 = false,且 *error 是可读(已本地化)的原因。ListTemplates(列出坏模板)与
		// ValidateTemplate(选中时的行内错误)/ Create(前置守卫)共用这一条口径。
		bool LoadTemplateInfo(const fs::path& directory, const std::string& id,
			ProjectScaffolder::TemplateInfo* info, std::string* error)
		{
			info->Id = id;
			info->Name = id;
			info->Directory = directory;
			info->Valid = true;

			std::error_code ec;
			if (!fs::is_directory(directory, ec))
			{
				if (error)
					*error = Wui::TrFormat("modal.newproject.error.template_missing",
						"Project template is missing: {path}", { { "path", directory.u8string() } });
				return false;
			}
			const fs::path manifestPath = directory / kTemplateManifestName;
			if (!fs::is_regular_file(manifestPath, ec))
			{
				if (error)
					*error = Wui::TrFormat("modal.newproject.error.template_manifest_missing",
						"Project template has no template.json: {path}",
						{ { "path", manifestPath.u8string() } });
				return false;
			}
			const auto invalid = [&manifestPath, error](const std::string& reason)
			{
				if (error)
					*error = Wui::TrFormat("modal.newproject.error.template_invalid",
						"Cannot read the project template: {path} ({reason})",
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
			if (const Wui::JsonValue* scene = root->Find("defaultScene"))
				info->DefaultScene = scene->AsString();
			if (const Wui::JsonValue* order = root->Find("order"))
				if (order->type == Wui::JsonValue::Type::Number)
					info->Order = static_cast<int>(order->Number);
			// 示例内容提示的判据 = 模板里有没有 assets/scripts/examples/(示例脚本随模板走);
			// 在扫描期算一次,不进每帧路径。
			std::error_code samplesError;
			info->HasSamples = fs::is_directory(directory / "assets" / "scripts" / "examples", samplesError);
			return true;
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

		// 清单的 renderer / rendering / physics 是**代码常量**(PROJ-7/T2 裁决 4):项目骨架
		// 只由"所选模板 + 引擎代码生成"构成(仓库里没有可参照的具体项目),脚手架不能依赖
		// 任何项目文件。取值 = 模板化之前那份示例项目清单的等价口径(旧清单位于
		// 注释 kStandardXxx;逐字段"旧值 → 新常量"对照见 tools/agents/reports/PROJ7-T2.md):
		//   renderer: vulkan
		//   rendering: culling=true / shadows=false / shadow_map_size=2048 /
		//              max_directional_lights=1 / max_point_lights=7 / gpu_timing=false /
		//              vsync=true / instancing=true / anisotropy=1 / render_scale=1.0 / msaa=1
		//   physics:   fixed_step_hz=60 / gravity=-9.81
		// 显式逐字段赋值(**不**依赖 RenderingSettings / PhysicsSettingsData 的默认值):
		// 引擎默认值将来若改,标准项目的清单不该跟着漂 —— 要改就改这里并重新过 T2 的对照表。
		void ApplyStandardSettingDefaults(Asset::ProjectManifest& manifest)
		{
			manifest.Renderer = "vulkan";

			Asset::RenderingSettings rendering;
			rendering.Culling = true;
			rendering.Shadows = false;
			rendering.ShadowMapSize = 2048;
			rendering.MaxDirectionalLights = 1;
			rendering.MaxPointLights = 7;
			rendering.GpuTiming = false;
			rendering.Vsync = true;
			rendering.Instancing = true;
			rendering.Anisotropy = 1;
			rendering.RenderScale = 1.0f;
			rendering.Msaa = 1;
			manifest.Rendering = rendering;

			Asset::PhysicsSettingsData physics;
			physics.FixedStepHz = 60;
			physics.Gravity = -9.81f;
			manifest.Physics = physics;
		}

		// startScene = 内容根(assets/)相对路径:模板声明了 defaultScene 就用它,否则用
		// 引擎序列化写出的 Main.wd(kStandardStartScene)。
		bool WriteStandardManifest(const fs::path& projectRoot, const std::string& name,
			const std::string& startScene, std::string* error)
		{
			Asset::ProjectManifest manifest;
			manifest.Id = ProjectScaffolder::ManifestId(name);
			manifest.Version = kStandardVersion;
			manifest.ContentRoot = kStandardContentRoot;
			manifest.StartScene = startScene;
			ApplyStandardSettingDefaults(manifest);
			manifest.Packages.clear();   // 新项目还没有发行包(默认项目的 Base.wpak 是引擎内容)
			return Asset::ProjectManifest::Save(projectRoot / kManifestFileName, manifest, error);
		}

		// Main.wd 走引擎序列化(不手写 YAML):Scene 需要一个 WorldContext,序列化完即弃。
		// includeStarterScene = true 时写入"最小可运行场景":一台 Camera3D(位置 [0,1,5]、
		// Primary、透视)+ 一盏方向光 —— 只由引擎组件构成,不带任何示例资产/材质/脚本;
		// false 时保持旧行为(空场景)。形态与示例模板自带的 scenes/3DTest.wd
		// (templates/project-example/assets/scenes/3DTest.wd)同源(同一个 writer)。
		bool WriteMainScene(const fs::path& projectRoot, WorldContext& context, bool includeStarterScene,
			std::string* error)
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
			if (includeStarterScene)
			{
				try
				{
					Entity camera = Entity::CreateEntity(scene.get(), "Camera3D");
					camera.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 1.0f, 5.0f });
					CameraComponent cameraComponent;
					cameraComponent.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
					cameraComponent.Camera.SetPerspectiveFarClip(200.0f);
					cameraComponent.Primary = true;
					camera.AddComponent<CameraComponent>(cameraComponent);

					Entity light = Entity::CreateEntity(scene.get(), "Directional Light");
					light.AddComponent<TransformComponent>();
					light.AddComponent<DirectionalLightComponent>();
				}
				catch (const std::exception& exception)
				{
					if (error)
						*error = std::string("starter scene setup failed: ") + exception.what();
					return false;
				}
			}
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

		// ---- PROJ-3/T1(P2b)+PROJ-4/T1(P3):项目根的启动入口(exe 优先,缺失则 .cmd 兜底)----
		//
		// 用户口径(方案 v2 P2 / PROJ-4 v1 P3):新项目位置双击就能起引擎 —— 引擎自带的小
		// 启动器(WeEdit.exe / WePlay.exe,T2 产出)优先复制进项目根;构建目录里还没有
		// (或复制失败)就写 `.cmd` 兜底。落点名字跟项目名走(`<项目名>-Edit.exe` /
		// `<项目名>-Play.exe` / `<项目名>-Edit.cmd` / `<项目名>-Play.cmd`)。两个入口都是
		// **本机产物**(引擎绝对路径 + 二进制),因此同时补 `.gitignore` 通配条目,
		// 避免被提交进项目历史。

		// WLD_OUTPUT_DIR / WLD_BUILD_TYPE / WLD_REPO_ROOT 都可能是"带尾分隔符"的编译期宏;
		// 拼 Windows 路径前统一去掉尾分隔符。
		std::string TrimTrailingSeparators(std::string text)
		{
			while (!text.empty() && (text.back() == '/' || text.back() == '\\'))
				text.pop_back();
			return text;
		}

		// `.cmd` 里的路径一律用 Windows 反斜杠(前向斜杠在 cmd 的路径位置不可靠)。
		std::string ToWindowsPath(const std::string& text)
		{
			std::string out = text;
			for (char& character : out)
				if (character == '/')
					character = '\\';
			return out;
		}

		// 在构建目录里找 Launcher 产物的实际落点。输出布局由 T2 的 Launcher/CMakeLists.txt
		// 决定(RUNTIME_OUTPUT_DIRECTORY = <构建根>/Launcher,VS 多配置再补一层配置目录),
		// 所以**不写死单一路径**,而是按"落点形状"逐个试(顺序 = 优先级):
		//   1) <构建根>/Launcher/<配置>/<文件名>      —— 树内构建的正常落点(T2 的 CMakeLists)
		//   2) <构建根>/Launcher/<文件名>              —— 单配置生成器
		//   3) <构建根>/bin/<配置>/Launcher/<配置>/<文件名> —— 与 Game.dll 同口径的布局
		//   4) <构建根>/bin/<配置>/Launcher/<文件名>
		// **不做**全树递归搜索:构建目录里还有测试夹具(实测 T2 的 fakeroot 夹具就在
		// build/x64-Debug/proj3-t2-*/Launcher/<配置>/ 下),递归会把"故意指向假引擎根的夹具"
		// 当成产物复制进用户项目(knowledge/traps/multi-root-path-fallback 的同一条判据)。
		// 找不到 = 空,调用方退 .cmd 兜底(仍然可用)。
		fs::path FindLauncherArtifact(const std::string& fileName)
		{
			const fs::path outputRoot = RepoRoot() / TrimTrailingSeparators(WLD_OUTPUT_DIR);
			const std::string buildType = TrimTrailingSeparators(WLD_BUILD_TYPE);
			const fs::path name = fs::u8path(fileName);
			const fs::path candidates[] = {
				outputRoot / "Launcher" / buildType / name,
				outputRoot / "Launcher" / name,
				outputRoot / "bin" / buildType / "Launcher" / buildType / name,
				outputRoot / "bin" / buildType / "Launcher" / name,
			};
			for (const fs::path& candidate : candidates)
			{
				std::error_code sizeError;
				if (fs::is_regular_file(candidate)
					&& fs::file_size(candidate, sizeError) > 0)
					return candidate;
			}
			return {};
		}

		bool WriteTextFile(const fs::path& path, const std::string& text, std::string* error)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error)
					*error = "cannot open " + path.u8string() + " for writing";
				return false;
			}
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!stream)
			{
				if (error)
					*error = "cannot write " + path.u8string();
				return false;
			}
			return true;
		}

		// 生成 .cmd 的内容(方案 v2 的模板):`%~dp0` **带尾反斜杠**,所以 `--project "%~dp0."`
		// —— 点号规避 MSVC CRT 把 `…\"` 里的引号吃掉(见方案 F5)。`WE_ROOT` 可在环境里覆盖。
		std::string LauncherCmdText(const std::string& exeRelative)
		{
			std::string text;
			text += "@echo off\r\n";
			text += "rem WorldEngine project launcher (generated by the project wizard).\r\n";
			text += "rem Opens this project with the engine that created it; WE_ROOT overrides the engine root.\r\n";
			text += "if not defined WE_ROOT set \"WE_ROOT="
				+ ToWindowsPath(TrimTrailingSeparators(WLD_REPO_ROOT)) + "\"\r\n";
			text += "\"%WE_ROOT%\\" + ToWindowsPath(exeRelative) + "\" --project \"%~dp0.\"\r\n";
			return text;
		}

		std::vector<std::string> SplitLines(const std::string& text)
		{
			std::vector<std::string> lines;
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = text.find('\n', start);
				std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				lines.push_back(std::move(line));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return lines;
		}

		// `.gitignore`:启动入口是本机产物,不进项目历史;已存在则**只补缺行**
		// (逐行精确比较,不重写用户自己的内容,也不重复追加)。
		bool EnsureGitignoreEntries(const fs::path& projectRoot, const std::vector<std::string>& entries,
			std::string* error)
		{
			const fs::path path = projectRoot / ".gitignore";
			std::string content;
			std::error_code ec;
			if (fs::is_regular_file(path, ec))
			{
				std::ifstream stream(path, std::ios::binary);
				if (stream)
					content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			}
			const std::vector<std::string> lines = SplitLines(content);
			std::string appended;
			for (const std::string& entry : entries)
			{
				if (std::find(lines.begin(), lines.end(), entry) != lines.end())
					continue;
				appended += entry;
				appended += "\r\n";
			}
			if (appended.empty())
				return true;
			if (content.empty())
				content += "# WorldEngine: local launch entry points + machine-local state (generated).\r\n";
			else if (content.back() != '\n')
				content += "\r\n";
			content += appended;
			return WriteTextFile(path, content, error);
		}

		std::string JoinNames(const std::vector<std::string>& names)
		{
			std::string text;
			for (const std::string& name : names)
			{
				if (!text.empty())
					text += ", ";
				text += name;
			}
			return text;
		}

		// PROJ-4/T1(P3):启动入口文件名跟项目名走(`<项目名>-Edit.exe` / `<项目名>-Play.exe`,
		// `.cmd` 兜底同理)。Windows 非法文件名字符与控制字符替换成 `_` —— 只影响文件名,
		// 不改项目名本身(非 ASCII 字节原样保留)。
		std::string SanitizeLauncherFileStem(const std::string& projectName)
		{
			std::string stem = projectName;
			for (char& character : stem)
			{
				const unsigned char c = static_cast<unsigned char>(character);
				if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
					c == '"' || c == '<' || c == '>' || c == '|')
					character = '_';
			}
			if (stem.empty())
				stem = "project";
			return stem;
		}

		// P2b 的收尾步骤:优先复制 `WeEdit.exe`/`WePlay.exe`(**复制源不变**),按项目名落成
		// `<项目名>-Edit.exe` / `<项目名>-Play.exe`;缺哪个就给哪个写同名 `.cmd` 兜底;
		// 随后补 `.gitignore` 通配条目。返回写进项目根的入口文件名(排序)。
		// **任何失败都不影响项目本体** —— 只记警告,返回已经写成的部分。
		std::vector<std::string> PlaceLaunchEntryPoints(const fs::path& projectRoot,
			const std::string& projectName)
		{
			const std::string buildType = TrimTrailingSeparators(WLD_BUILD_TYPE);
			const std::string outputDir = TrimTrailingSeparators(WLD_OUTPUT_DIR);
			const std::string editorExeRelative = outputDir + "/Editor/" + buildType + "/Editor.exe";
			const std::string runtimeExeRelative = outputDir + "/Runtime/" + buildType + "/Runtime.exe";

			const std::string stem = SanitizeLauncherFileStem(projectName);
			struct EntryRequest
			{
				const char* SourceExeName;   // 构建目录里的启动器本体(名字固定)
				std::string ExeName;         // 落进项目根的名字(跟项目名)
				std::string CmdName;         // exe 缺失时的兜底脚本名(同样跟项目名)
				const std::string* TargetExeRelative;
			};
			const EntryRequest requests[2] = {
				{ "WeEdit.exe", stem + "-Edit.exe", stem + "-Edit.cmd", &editorExeRelative },
				{ "WePlay.exe", stem + "-Play.exe", stem + "-Play.cmd", &runtimeExeRelative },
			};

			std::vector<std::string> placed;
			for (const EntryRequest& request : requests)
			{
				bool copied = false;
				const fs::path artifact = FindLauncherArtifact(request.SourceExeName);
				if (!artifact.empty())
				{
					std::error_code copyError;
					fs::copy_file(artifact, projectRoot / fs::u8path(request.ExeName),
						fs::copy_options::none, copyError);
					copied = !copyError;
					if (copyError)
						WLD_CORE_WARN("[project-scaffolder] could not copy '{0}' -> '{1}': {2}",
							artifact.u8string(), (projectRoot / fs::u8path(request.ExeName)).u8string(),
							copyError.message());
				}
				if (copied)
				{
					WLD_CORE_INFO("[project-scaffolder] copied launcher '{0}' -> '{1}'",
						artifact.u8string(), (projectRoot / fs::u8path(request.ExeName)).u8string());
					placed.push_back(request.ExeName);
					continue;
				}
				std::string writeError;
				if (WriteTextFile(projectRoot / fs::u8path(request.CmdName),
					LauncherCmdText(*request.TargetExeRelative), &writeError))
				{
					WLD_CORE_INFO("[project-scaffolder] wrote fallback launcher '{0}' (engine root {1})",
						(projectRoot / fs::u8path(request.CmdName)).u8string(),
						TrimTrailingSeparators(WLD_REPO_ROOT));
					placed.push_back(request.CmdName);
				}
				else
					WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
						(projectRoot / fs::u8path(request.CmdName)).u8string(), writeError);
			}

			std::string gitignoreError;
			const std::vector<std::string> ignoreEntries = {
				"*-Edit.exe", "*-Play.exe", "*-Edit.cmd", "*-Play.cmd", "/build/", "/local/" };
			if (!EnsureGitignoreEntries(projectRoot, ignoreEntries, &gitignoreError))
				WLD_CORE_WARN("[project-scaffolder] could not update .gitignore: {0}", gitignoreError);

			std::sort(placed.begin(), placed.end());
			return placed;
		}
	}

	fs::path ProjectScaffolder::TemplateRoot()
	{
		return RepoRoot() / "templates";
	}

	fs::path ProjectScaffolder::TemplateDirectory(const std::string& templateId)
	{
		if (!IsValidTemplateId(templateId))
			return {};
		return TemplateRoot() / (std::string(kTemplateDirectoryPrefix) + templateId);
	}

	std::vector<ProjectScaffolder::TemplateInfo> ProjectScaffolder::ListTemplates()
	{
		std::vector<TemplateInfo> templates;
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
					TemplateInfo info;
					const std::string id = directoryName.substr(std::strlen(kTemplateDirectoryPrefix));
					std::string error;
					if (!IsValidTemplateId(id))
					{
						info.Id = id;
						info.Name = id;
						info.Directory = entry.path();
						info.Valid = false;
						info.Error = Wui::TrFormat("modal.newproject.error.template_id_invalid",
							"Invalid project template id (letters, digits, '.', '_' and '-' only): {id}",
							{ { "id", id } });
					}
					else if (!LoadTemplateInfo(entry.path(), id, &info, &error))
					{
						info.Valid = false;
						info.Error = error;
					}
					templates.push_back(std::move(info));
				}
			}
			iterator.increment(ec);
			if (ec)
				break;
		}
		std::sort(templates.begin(), templates.end(),
			[](const TemplateInfo& a, const TemplateInfo& b)
			{
				if (a.Order != b.Order)
					return a.Order < b.Order;
				return a.Id < b.Id;
			});
		return templates;
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

	std::string ProjectScaffolder::ValidateTemplate(const std::string& templateId)
	{
		const fs::path root = TemplateRoot();
		std::error_code ec;
		if (!fs::is_directory(root, ec))
			return Wui::TrFormat("modal.newproject.error.template_missing",
				"Project template is missing: {path}", { { "path", root.u8string() } });
		// 一个模板都没有(空 id)⇒ 报模板根:比"id 非法"更接近用户看到的事实。
		if (templateId.empty())
			return Wui::TrFormat("modal.newproject.error.template_missing",
				"Project template is missing: {path}", { { "path", root.u8string() } });
		if (!IsValidTemplateId(templateId))
			return Wui::TrFormat("modal.newproject.error.template_id_invalid",
				"Invalid project template id (letters, digits, '.', '_' and '-' only): {id}",
				{ { "id", templateId } });

		const fs::path directory = TemplateDirectory(templateId);
		TemplateInfo info;
		std::string reason;
		if (!LoadTemplateInfo(directory, templateId, &info, &reason))
			return reason;
		for (const char* entry : kRequiredTemplateEntries)
		{
			const bool wantsDirectory = entry[std::strlen(entry) - 1] == '/';
			const fs::path path = directory / fs::u8path(wantsDirectory ? std::string(entry, std::strlen(entry) - 1)
				: std::string(entry));
			std::error_code entryError;
			const bool present = wantsDirectory ? fs::is_directory(path, entryError)
				: fs::is_regular_file(path, entryError);
			if (!present)
				return Wui::TrFormat("modal.newproject.error.template_missing",
					"Project template is missing: {path}", { { "path", path.u8string() } });
		}
		// 模板声明的启动场景必须真的在模板里(相对内容根 assets/)—— 否则生成的清单会指向
		// 一个不存在的场景,用户第一次"打开项目"就踩空。
		if (!info.DefaultScene.empty())
		{
			const fs::path scenePath = directory / "assets" / fs::u8path(info.DefaultScene);
			std::error_code sceneError;
			if (!fs::is_regular_file(scenePath, sceneError))
				return Wui::TrFormat("modal.newproject.error.template_scene_missing",
					"Project template scene is missing: {scene}",
					{ { "scene", scenePath.u8string() } });
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
		WorldContext& context, bool includeStarterScene, const std::string& templateId)
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
		const std::string templateError = ValidateTemplate(templateId);
		if (!templateError.empty())
			return fail(templateError);
		// 校验已通过:再读一次模板信息拿 defaultScene(与 ValidateTemplate 同一条读取口径)。
		TemplateInfo templateInfo;
		std::string templateInfoError;
		if (!LoadTemplateInfo(TemplateDirectory(templateId), templateId, &templateInfo, &templateInfoError))
			return fail(templateInfoError);
		const std::string startScene = templateInfo.DefaultScene.empty()
			? std::string(kStandardStartScene) : templateInfo.DefaultScene;
		const bool templateOwnsStartScene = !templateInfo.DefaultScene.empty();

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
		bool built = CopyTemplateTree(templateInfo.Directory, temporary, &error);
		if (built)
			built = WriteStandardManifest(temporary, name, startScene, &error);
		// 模板自带启动场景(example)时不写 Main.wd —— 清单的 start_scene 已经指向模板里的场景,
		// 再写一份只会多出一个用不到的场景;没有 defaultScene 的模板(empty)保持原口径:
		// includeStarterScene = 相机 + 方向光 / 空场景。
		if (built && !templateOwnsStartScene)
			built = WriteMainScene(temporary, context, includeStarterScene, &error);
		if (built)
		{
			// Luau LSP 脚手架(.vscode/settings.json + .luau-lsp/config.json)走引擎既有实现:
			// 与编辑器启动期为当前项目生成的那两份逐字节同源(模板目录不放它们 —— 仓库的
			// 裸 `.vscode` 忽略规则会吞掉模板里的同名文件)。
			built = World::EnsureScriptEditorScaffold(temporary, &error);
		}
		// PROJ-3/T1(P2b)+PROJ-4/T1(P3):启动入口(exe 优先,缺失 ⇒ .cmd;名字跟项目名)+
		// `.gitignore` 通配条目 —— 一次创建的收尾步骤,写在临时目录里(与骨架一起改名进
		// 目标位置)。失败**不影响项目本体**,因此不参与 built 判定:只记警告,能写成几个算几个。
		std::vector<std::string> entryPoints;
		if (built)
		{
			entryPoints = PlaceLaunchEntryPoints(temporary, name);
			if (!entryPoints.empty())
				WLD_CORE_INFO("[project-scaffolder] launch entry points: {0}", JoinNames(entryPoints));
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
		result.EntryPoints = entryPoints;
		result.Files = CollectRelativeFiles(target);
		WLD_CORE_INFO("[project-scaffolder] created '{0}' ({1} files, template '{2}')", target.u8string(),
			result.Files.size(), templateInfo.Id);
		WLD_CORE_INFO("[project-scaffolder] template '{0}'; start scene '{1}'{2}", templateInfo.Id, startScene,
			templateOwnsStartScene ? " (from the template)"
				: (includeStarterScene ? " (starter scene: Camera3D + Directional Light)" : " (empty scene)"));
		return result;
	}
}
