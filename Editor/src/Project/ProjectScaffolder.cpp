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
#include <ctime>
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

					// PROJ-10(用户 2026-09-29):空白模板也给一颗**内置图元**立方体 —— 启动就能看见东西,
					// 不引入任何资产/材质(Primitive 是引擎内置网格,Color 直接当基色)。
					Entity cube = Entity::CreateEntity(scene.get(), "Cube");
					cube.AddComponent<TransformComponent>(glm::vec3 { 0.0f, 0.0f, 0.0f });
					MeshRendererComponent cubeRenderer;
					cubeRenderer.Primitive = "cube";
					cubeRenderer.Color = glm::vec4 { 0.82f, 0.82f, 0.86f, 1.0f };
					cube.AddComponent<MeshRendererComponent>(cubeRenderer);
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

		// ---- PROJ-3/T1(P2b)+PROJ-4/T1(P3)+PROJ-12/T2:项目启动入口(exe 优先,.cmd 兜底进 .we/)----
		//
		// 用户口径(方案 v2 P2 / PROJ-4 v1 P3 / PROJ-12 v1):引擎自带的小启动器
		// (WeEdit.exe / WePlay.exe)优先按项目名复制进**项目根**(`<项目名>-Edit.exe` /
		// `<项目名>-Play.exe`);构建目录里还没有(或复制失败)才写 `.cmd` 兜底,且兜底从
		// PROJ-12/T2 起落在 `<项目>/.we/`(根目录保持"2 exe + build.cmd + CMakePresets.json")。
		// 这些入口都是**本机产物**(引擎绝对路径 + 二进制),因此同时补 `.gitignore` 通配条目。

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

		// PROJ-12F/T2:备份名 = `<原名>.bak-<yyyyMMdd-HHmmss>`(本地时间)。同一秒里
		// 再触发一次就用 `-2`/`-3`… 补序号 —— **备份绝不复用、绝不覆盖**:
		// 已经存在的备份保持原样,新备份总是换一个新名字。
		std::string LocalBackupTimestamp()
		{
			const std::time_t now = std::time(nullptr);
			std::tm local {};
			localtime_s(&local, &now);
			char buffer[32] = {};
			if (std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local) == 0)
				return "00000000-000000";
			return buffer;
		}

		// 在 directory 里挑一个**当前不存在**的备份路径 —— 一律"已存在就换名",
		// 绝不覆盖别人(或上一次)的备份:
		//   * alwaysTimestamp = true(模板文件升级):先试 `<baseName>-<时间戳>`,
		//     被占用(同一秒里又升级一次)再补 `-2` / `-3` …;
		//   * alwaysTimestamp = false(.we/ 里的 legacy 备份):名字本来就带 `.bak`,
		//     **先用原名**,同名备份已存在才加时间戳后缀(再冲突继续补序号)。
		// 挑不出来(999 次重名)返回空 ⇒ 调用方按"备份失败"处理(宁可不覆盖,也不丢用户内容)。
		fs::path MakeFreshBackupPath(const fs::path& directory, const std::string& baseName,
			bool alwaysTimestamp)
		{
			std::error_code ec;
			if (!alwaysTimestamp && !fs::exists(directory / fs::u8path(baseName), ec))
				return directory / fs::u8path(baseName);
			const std::string first = baseName + "-" + LocalBackupTimestamp();
			if (!fs::exists(directory / fs::u8path(first), ec))
				return directory / fs::u8path(first);
			for (int suffix = 2; suffix < 1000; ++suffix)
			{
				const std::string candidate = first + "-" + std::to_string(suffix);
				if (!fs::exists(directory / fs::u8path(candidate), ec))
					return directory / fs::u8path(candidate);
			}
			return {};
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

		std::string ReadTextFile(const fs::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return {};
			return std::string((std::istreambuf_iterator<char>(stream)),
				std::istreambuf_iterator<char>());
		}

		// 生成 .cmd 的内容(方案 v2 的模板):`%~dp0` **带尾反斜杠**,所以 `--project "%~dp0."`
		// —— 点号规避 MSVC CRT 把 `…\"` 里的引号吃掉(见方案 F5)。`WE_ROOT` 可在环境里覆盖。
		// PROJ-12/T2:兜底 `.cmd` 移进 `<项目>/.we/` 后,项目根是 `%~dp0..`(`.we` 的上一级)。
		std::string LauncherCmdText(const std::string& exeRelative, bool inWeDirectory)
		{
			const char* projectArgument = inWeDirectory ? "%~dp0.." : "%~dp0.";
			std::string text;
			text += "@echo off\r\n";
			text += "rem WorldEngine project launcher (generated by the project wizard).\r\n";
			text += "rem Opens this project with the engine that created it; WE_ROOT overrides the engine root.\r\n";
			text += "if not defined WE_ROOT set \"WE_ROOT="
				+ ToWindowsPath(TrimTrailingSeparators(WLD_REPO_ROOT)) + "\"\r\n";
			text += "\"%WE_ROOT%\\" + ToWindowsPath(exeRelative) + "\" --project \"" + projectArgument + "\"\r\n";
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

		// ---- PROJ-12/T2:入口整理(.we/)+ 机器本地 VS 工作流文件 ----

		// 一次入口整理的完整结果(向导与"生成构建入口"共用同一条落盘路径)。
		struct LaunchPlacement
		{
			std::vector<std::string> EntryPoints;   // 项目根 exe,或 .we/ 下的 .cmd 兜底(相对项目根,已排序)
			std::vector<std::string> Created;
			std::vector<std::string> Refreshed;
			std::vector<std::string> Skipped;
			std::vector<std::string> Removed;
			std::vector<std::string> Migrated;      // PROJ-12F/T2:根级旧 .cmd 迁到 .we/ 的(相对项目根)
			std::vector<std::string> Backups;       // PROJ-12F/T2:实际写出的备份文件(相对项目根)
			std::vector<std::string> Warnings;
		};

		// JSON 字符串转义(路径里只有反斜杠/引号需要处理;控制字符兜底成 \u00xx)。
		std::string JsonEscape(const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (const char character : text)
			{
				switch (character)
				{
				case '\\': out += "\\\\"; break;
				case '"': out += "\\\""; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(character) < 0x20)
					{
						char buffer[8] = {};
						std::snprintf(buffer, sizeof(buffer), "\\u%04x",
							static_cast<unsigned int>(static_cast<unsigned char>(character)));
						out += buffer;
					}
					else
						out.push_back(character);
					break;
				}
			}
			return out;
		}

		// 引擎构建目录(= WE_ENGINE_BUILD_DIR 的候选值)。只有快路径的三件产物都在时才算
		// "可用";否则本地 preset 不写这一项,让项目 CMake 回落源码模式,VS 仍然能配置成功。
		std::string UsableEngineBuildDir(const std::string& engineRoot)
		{
			const std::string buildType = TrimTrailingSeparators(WLD_BUILD_TYPE);
			const fs::path directory = fs::u8path(engineRoot)
				/ fs::u8path(TrimTrailingSeparators(WLD_OUTPUT_DIR));
			std::error_code ec;
			const bool library = fs::is_regular_file(
				directory / "Engine" / fs::u8path(buildType) / "WorldRuntime.lib", ec);
			const bool runtime = fs::is_regular_file(
				directory / "Engine" / fs::u8path(buildType) / "WorldRuntime.dll", ec);
			const bool compiler = fs::is_regular_file(
				directory / "Engine" / "generators" / "schema-compiler" / fs::u8path(buildType)
					/ "schema-compiler.exe",
				ec);
			if (library && runtime && compiler)
				return ToWindowsPath(directory.u8string());
			return {};
		}

		// 在模板的可移植 CMakePresets.json 里插入机器本地 `local` configure preset:
		//   * 插到 configurePresets 数组**最前**(VS 默认取第一个可见预设)⇒ 打开项目文件夹即可配置;
		//   * 继承模板的第一个可见预设,只覆盖 WE_ROOT(以及可用的 WE_ENGINE_BUILD_DIR);
		//   * 模板已有 `local` 预设 / 没有 configurePresets 数组 ⇒ 返回空串(调用方原样写模板)。
		// 注意:预设定名字不能重复,否则 CMake 会直接报错 —— 这里宁可退化成"只有可移植字段"。
		std::string InjectLocalPreset(const std::string& portable, const std::string& engineRoot,
			const std::string& engineBuildDir)
		{
			const size_t keyPosition = portable.find("\"configurePresets\"");
			if (keyPosition == std::string::npos)
				return {};
			const size_t arrayPosition = portable.find('[', keyPosition);
			if (arrayPosition == std::string::npos)
				return {};

			const auto readStringValue = [](const std::string& text, size_t keyStart,
				std::string* value)
			{
				const size_t colon = text.find(':', keyStart);
				if (colon == std::string::npos)
					return false;
				const size_t openQuote = text.find('"', colon);
				if (openQuote == std::string::npos)
					return false;
				std::string parsed;
				for (size_t index = openQuote + 1; index < text.size(); ++index)
				{
					const char character = text[index];
					if (character == '\\' && index + 1 < text.size())
					{
						parsed.push_back(text[index + 1]);
						++index;
						continue;
					}
					if (character == '"')
					{
						*value = parsed;
						return true;
					}
					parsed.push_back(character);
				}
				return false;
			};

			std::string baseName;
			size_t search = arrayPosition + 1;
			while (baseName.empty())
			{
				const size_t namePosition = portable.find("\"name\"", search);
				if (namePosition == std::string::npos)
					break;
				const size_t nextName = portable.find("\"name\"", namePosition + 6);
				const size_t segmentEnd = nextName == std::string::npos ? portable.size() : nextName;
				std::string name;
				if (readStringValue(portable, namePosition, &name) && !name.empty())
				{
					if (name == "local")
						return {};   // 模板已经用了这个名字:不制造重复 preset
					const std::string segment = portable.substr(namePosition, segmentEnd - namePosition);
					const size_t hiddenPosition = segment.find("\"hidden\"");
					const bool hidden = hiddenPosition != std::string::npos
						&& segment.find("true", hiddenPosition) != std::string::npos;
					if (!hidden)
						baseName = name;
				}
				search = namePosition + 6;
			}
			if (baseName.empty())
				return {};

			std::string local = "\n    {\n";
			local += "      \"name\": \"local\",\n";
			local += "      \"displayName\": \"Local machine (generated)\",\n";
			local += "      \"inherits\": \"" + JsonEscape(baseName) + "\",\n";
			local += "      \"cacheVariables\": {\n";
			local += "        \"WE_ROOT\": \"" + JsonEscape(engineRoot) + "\"";
			if (!engineBuildDir.empty())
				local += ",\n        \"WE_ENGINE_BUILD_DIR\": \"" + JsonEscape(engineBuildDir) + "\"\n";
			else
				local += "\n";
			local += "      }\n    },\n";

			std::string output = portable;
			output.insert(arrayPosition + 1, local);
			return output;
		}

		// VS 启动项的名字:`launch.vs.json` 的 configuration name 与 `.vs/ProjectSettings.json`
		// 的 `CurrentProjectSetting`(`<名字> (<配置>)`)必须同一个来源,否则"默认选中"指不上。
		constexpr const char* kLaunchItemName = "Open Editor (this project)";
		// VS 自己记录"当前启动项"的那个键(值是字符串)。
		constexpr const char* kProjectSettingKey = "CurrentProjectSetting";

		// `<项目>/.vs/launch.vs.json`:**一个** VS 启动项(Open Editor)。落 **`.vs/`** ——
		// 官方文档口径:`launch.vs.json` 位于项目根的 `.vs` 文件夹;`projectTarget` 必须匹配
		// "启动项"下拉里已有的**可执行**目标。锚点 = 项目 CMake 自己的 `ProjectRun`
		// (add_dependencies(Game) ⇒ F5 先构建项目 C++ 再启动引擎);不再写 `program` ——
		// 让 VS 跑它自己构建出来的那个 exe。这一条默认就是**打开编辑器**,想跑游戏给
		// `ProjectRun.exe` 加 `--play`(下拉里只有这一项,用户不用在两项之间挑)。
		// 文件内容是本机的(路径写死生成时的项目根),`.gitignore` 忽略 `/.vs/`。
		std::string LaunchVsJsonText(const fs::path& projectRoot)
		{
			const std::string buildType = TrimTrailingSeparators(WLD_BUILD_TYPE);
			const std::string root = ToWindowsPath(projectRoot.u8string());
			// 产物布局由项目模板的 CMake 固定:`<构建根>/bin/<cfg>/ProjectRun.exe`
			// (与 Game 同布局);projectTarget 括号里是相对**构建根**的路径。
			const std::string runTarget = "ProjectRun.exe (bin\\" + buildType + "\\ProjectRun.exe)";

			std::string text;
			text += "{\n";
			text += "  \"version\": \"0.2.1\",\n";
			text += "  \"defaults\": {},\n";
			text += "  \"configurations\": [\n";
			text += "    {\n";
			text += "      \"type\": \"default\",\n";
			text += "      \"name\": \"" + JsonEscape(kLaunchItemName) + "\",\n";
			text += "      \"project\": \"CMakeLists.txt\",\n";
			text += "      \"projectTarget\": \"" + JsonEscape(runTarget) + "\",\n";
			text += "      \"args\": [\"--project\", \"" + JsonEscape(root) + "\"],\n";
			text += "      \"currentDir\": \"" + JsonEscape(root) + "\"\n";
			text += "    }\n";
			text += "  ]\n";
			text += "}\n";
			return text;
		}

		// `<项目>/.vs/ProjectSettings.json`:VS 自己存"当前启动项"的文件(`CurrentProjectSetting`
		// = `<launch 项名字> (<配置>)`,实测 VS 自己也是这么存的)。**合并写**:只改这一个键,
		// VS 已经写进去的其它键(工程/路径哈希等)原样保留;文件不是 JSON 对象、或该键不是
		// 字符串时**不动它**并返回 false —— 宁可让用户在 VS 里点一次下拉,也不清掉他的状态。
		bool MergeCurrentProjectSetting(const fs::path& path, const std::string& value,
			bool* created, bool* changed, std::string* error)
		{
			std::error_code existsError;
			const bool existed = fs::is_regular_file(path, existsError);
			std::string text = existed ? ReadTextFile(path) : std::string();
			const std::string quotedValue = "\"" + JsonEscape(value) + "\"";
			const std::string entry = "\"" + std::string(kProjectSettingKey) + "\": " + quotedValue;

			if (!existed || text.empty())
			{
				text = "{\n  " + entry + "\n}\n";
			}
			else
			{
				// 先确认它是 JSON 对象(容忍 UTF-8 BOM);不认识的内容一律不碰。
				std::string parseText = text;
				if (parseText.size() >= 3
					&& static_cast<unsigned char>(parseText[0]) == 0xEF
					&& static_cast<unsigned char>(parseText[1]) == 0xBB
					&& static_cast<unsigned char>(parseText[2]) == 0xBF)
					parseText.erase(0, 3);
				std::string parseError;
				const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(parseText, &parseError);
				if (!root || root->type != Wui::JsonValue::Type::Object)
				{
					*error = "existing " + path.u8string() + " is not a JSON object";
					return false;
				}

				const std::string quotedKey = "\"" + std::string(kProjectSettingKey) + "\"";
				const size_t keyPosition = text.find(quotedKey);
				bool replaced = false;
				if (keyPosition != std::string::npos)
				{
					// 键 → `:` → 字符串字面量;任一步对不上就放弃(宁可不动)。
					size_t cursor = keyPosition + quotedKey.size();
					while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
						++cursor;
					if (cursor < text.size() && text[cursor] == ':')
					{
						++cursor;
						while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
							++cursor;
						if (cursor < text.size() && text[cursor] == '"')
						{
							size_t end = cursor + 1;
							bool closed = false;
							while (end < text.size())
							{
								if (text[end] == '\\')
								{
									end += 2;   // 跳过转义序列(含 \" 与 \\)。
									continue;
								}
								if (text[end] == '"')
								{
									closed = true;
									break;
								}
								++end;
							}
							if (closed)
							{
								text.replace(cursor, end - cursor + 1, quotedValue);
								replaced = true;
							}
						}
					}
					if (!replaced)
					{
						*error = "existing " + path.u8string() + " has an unrecognized "
							+ kProjectSettingKey + " entry";
						return false;
					}
				}
				else
				{
					// 没这个键:插到第一个 `{` 之后(空对象不加尾逗号)。
					const size_t brace = text.find('{');
					if (brace == std::string::npos)
					{
						*error = "existing " + path.u8string() + " is not a JSON object";
						return false;
					}
					size_t next = brace + 1;
					while (next < text.size() && std::isspace(static_cast<unsigned char>(text[next])))
						++next;
					if (next < text.size() && text[next] == '}')
						text.insert(brace + 1, "\n  " + entry + "\n");
					else
						text.insert(brace + 1, "\n  " + entry + ",");
				}
			}

			if (existed && ReadTextFile(path) == text)
			{
				*created = false;
				*changed = false;
				return true;
			}
			std::error_code directoryError;
			fs::create_directories(path.parent_path(), directoryError);
			if (directoryError)
			{
				*error = "cannot create " + path.parent_path().u8string() + " ("
					+ directoryError.message() + ")";
				return false;
			}
			if (!WriteTextFile(path, text, error))
				return false;
			*created = !existed;
			*changed = true;
			return true;
		}

		// 写一个生成文件:内容一致 = 未改动;否则建好父目录再写。
		bool WriteGeneratedFile(const fs::path& path, const std::string& content,
			bool* created, bool* changed, std::string* error)
		{
			std::error_code ec;
			const bool existed = fs::exists(path, ec);
			if (existed && ReadTextFile(path) == content)
			{
				*created = false;
				*changed = false;
				return true;
			}
			std::error_code directoryError;
			fs::create_directories(path.parent_path(), directoryError);
			if (directoryError)
			{
				*error = "cannot create " + path.parent_path().u8string() + " ("
					+ directoryError.message() + ")";
				return false;
			}
			if (!WriteTextFile(path, content, error))
				return false;
			*created = !existed;
			*changed = true;
			return true;
		}

		// P2b/PROJ-12 的收尾步骤:根目录只放 `<项目名>-Edit.exe` / `<项目名>-Play.exe`
		// (从构建目录的 WeEdit/WePlay 覆盖刷新);exe 缺失的那个才把 `.cmd` 兜底写进
		// `<项目>/.we/`。`.we/engine-root.txt` 记录引擎根(UTF-8 无 BOM);`<项目>/.vs/launch.vs.json`
		// 写**一个** VS 启动项(PROJ-15/T1:锚定项目自己的 ProjectRun 可执行目标,默认开编辑器),
		// `.vs/ProjectSettings.json` 再把 VS 的当前启动项合并设成它;
		// 项目根遗留的旧版 `launch.vs.json`(PROJ-13 生成物)一并清掉;
		// 项目根遗留的旧版 `*-Edit.cmd`/`*-Play.cmd` 一律移除。
		// **任何失败都不影响项目本体** —— 只记警告,返回已经写成的部分。
		// PROJ12-T4 修复:向导建项目时先写 `<目标>.tmp-*` 再整体改名 ⇒ 绝对路径内容
		// (`launch.vs.json` 的 --project/currentDir)必须用**最终项目根**写,否则改名后那些路径指向
		// 已不存在的临时目录。文件本体仍写在 projectRoot(临时目录)下,只有"内容里的根"用 contentRoot。
		LaunchPlacement PlaceLaunchEntryPoints(const fs::path& projectRoot, const std::string& projectName,
			const fs::path& finalProjectRoot = {})
		{
			LaunchPlacement placement;
			const fs::path contentRoot = finalProjectRoot.empty() ? projectRoot : finalProjectRoot;
			const std::string engineRoot = ToWindowsPath(TrimTrailingSeparators(WLD_REPO_ROOT));
			const std::string buildType = TrimTrailingSeparators(WLD_BUILD_TYPE);
			const std::string outputDir = TrimTrailingSeparators(WLD_OUTPUT_DIR);
			const std::string editorExeRelative = outputDir + "/Editor/" + buildType + "/Editor.exe";
			const std::string runtimeExeRelative = outputDir + "/Runtime/" + buildType + "/Runtime.exe";
			const std::string stem = SanitizeLauncherFileStem(projectName);
			const fs::path weDirectory = projectRoot / ".we";

			// `.we/engine-root.txt`:引擎根解析顺序里"预设之后的本地来源"。
			{
				const fs::path target = weDirectory / "engine-root.txt";
				bool created = false;
				bool changed = false;
				std::string error;
				if (WriteGeneratedFile(target, engineRoot, &created, &changed, &error))
				{
					if (created)
						placement.Created.push_back(".we/engine-root.txt");
					else if (changed)
						placement.Refreshed.push_back(".we/engine-root.txt");
					else
						placement.Skipped.push_back(".we/engine-root.txt");
				}
				else
				{
					placement.Warnings.push_back("could not write .we/engine-root.txt: " + error);
					WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
						target.u8string(), error);
				}
			}

			// `.vs/launch.vs.json`:VS CMake 工程模式的**一个**启动项(机器本地;gitignore 忽略 /.vs/)。
			{
				const fs::path target = projectRoot / ".vs" / "launch.vs.json";
				bool created = false;
				bool changed = false;
				std::string error;
				if (WriteGeneratedFile(target, LaunchVsJsonText(contentRoot),
					&created, &changed, &error))
				{
					if (created)
						placement.Created.push_back(".vs/launch.vs.json");
					else if (changed)
						placement.Refreshed.push_back(".vs/launch.vs.json");
					else
						placement.Skipped.push_back(".vs/launch.vs.json");
				}
				else
				{
					placement.Warnings.push_back("could not write .vs/launch.vs.json: " + error);
					WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
						target.u8string(), error);
				}
			}

			// `.vs/ProjectSettings.json`:VS 的"当前启动项"。设成上面那一条(名字 + 本次配置),
			// 打开项目就默认选中 `Open Editor`,不用用户再去下拉里挑一次。**合并写** ——
			// VS 已经写在里面的其它键(工程/路径哈希等)逐字节保留。
			{
				const fs::path target = projectRoot / ".vs" / "ProjectSettings.json";
				const std::string currentSetting = std::string(kLaunchItemName) + " ("
					+ TrimTrailingSeparators(WLD_BUILD_TYPE) + ")";
				bool created = false;
				bool changed = false;
				std::string error;
				if (MergeCurrentProjectSetting(target, currentSetting, &created, &changed, &error))
				{
					if (created)
						placement.Created.push_back(".vs/ProjectSettings.json");
					else if (changed)
						placement.Refreshed.push_back(".vs/ProjectSettings.json");
					else
						placement.Skipped.push_back(".vs/ProjectSettings.json");
				}
				else
				{
					placement.Warnings.push_back("could not write .vs/ProjectSettings.json: " + error);
					WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
						target.u8string(), error);
				}
			}

			// PROJ-13 把同一份写进过项目根(`<项目>/launch.vs.json`):那是旧版生成物,
			// VS 的 CMake 工程模式不读它,两份并存只会让人分不清哪份生效,一律清掉。
			// 只删这一个生成文件(它本来就被 .gitignore 忽略),不动 `.vs/` 里 VS 自己的状态。
			{
				const fs::path legacy = projectRoot / "launch.vs.json";
				std::error_code existsError;
				if (fs::is_regular_file(legacy, existsError))
				{
					std::error_code removeError;
					if (fs::remove(legacy, removeError))
					{
						placement.Removed.push_back("launch.vs.json");
						WLD_CORE_INFO("[project-scaffolder] removed stale '{0}'",
							legacy.u8string());
					}
					else
						placement.Warnings.push_back("could not remove stale launch.vs.json: "
							+ removeError.message());
				}
			}

			struct EntryRequest
			{
				const char* SourceExeName;   // 构建目录里的启动器本体(名字固定)
				std::string ExeName;         // 落进项目根的名字(跟项目名)
				std::string CmdName;         // exe 缺失时的兜底脚本名(同样跟项目名,.we/ 下)
				const std::string* TargetExeRelative;
			};
			const EntryRequest requests[2] = {
				{ "WeEdit.exe", stem + "-Edit.exe", stem + "-Edit.cmd", &editorExeRelative },
				{ "WePlay.exe", stem + "-Play.exe", stem + "-Play.cmd", &runtimeExeRelative },
			};

			for (const EntryRequest& request : requests)
			{
				const fs::path rootExe = projectRoot / fs::u8path(request.ExeName);
				const fs::path weCmd = weDirectory / fs::u8path(request.CmdName);
				const fs::path legacyRootCmd = projectRoot / fs::u8path(request.CmdName);

				bool launcherInPlace = false;
				const fs::path artifact = FindLauncherArtifact(request.SourceExeName);
				if (!artifact.empty())
				{
					std::error_code existsError;
					const bool existed = fs::exists(rootExe, existsError);
					std::error_code copyError;
					fs::copy_file(artifact, rootExe, fs::copy_options::overwrite_existing, copyError);
					if (!copyError)
					{
						launcherInPlace = true;
						if (existed)
							placement.Refreshed.push_back(request.ExeName);
						else
							placement.Created.push_back(request.ExeName);
						placement.EntryPoints.push_back(request.ExeName);
						WLD_CORE_INFO("[project-scaffolder] {0} launcher '{1}' from '{2}'",
							existed ? "refreshed" : "placed", rootExe.u8string(), artifact.u8string());
					}
					else
					{
						placement.Warnings.push_back("could not refresh " + request.ExeName + ": "
							+ copyError.message());
						WLD_CORE_WARN("[project-scaffolder] could not copy '{0}' -> '{1}': {2}",
							artifact.u8string(), rootExe.u8string(), copyError.message());
					}
				}

				if (!launcherInPlace)
				{
					// 兜底只在没有 Launcher exe 时写,而且写进 `.we/`(根目录保持 P1 形态)。
					bool created = false;
					bool changed = false;
					std::string error;
					if (WriteGeneratedFile(weCmd, LauncherCmdText(*request.TargetExeRelative, true),
						&created, &changed, &error))
					{
						const std::string relative = ".we/" + request.CmdName;
						if (created)
							placement.Created.push_back(relative);
						else if (changed)
							placement.Refreshed.push_back(relative);
						else
							placement.Skipped.push_back(relative);
						placement.EntryPoints.push_back(relative);
					}
					else
					{
						placement.Warnings.push_back("could not write " + request.CmdName
							+ " in .we/: " + error);
						WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
							weCmd.u8string(), error);
					}
				}
				else
				{
					// exe 已就位:清掉上一次留下的 `.we/` 兜底(保持"兜底只在缺 exe 时存在")。
					std::error_code existsError;
					if (fs::is_regular_file(weCmd, existsError))
					{
						std::error_code removeError;
						if (fs::remove(weCmd, removeError))
							placement.Removed.push_back(".we/" + request.CmdName);
						else
							placement.Warnings.push_back("could not remove stale .we/"
								+ request.CmdName + ": " + removeError.message());
					}
				}

				// PROJ-12/T2 + PROJ-12F/T2:项目根不再放 .cmd;旧项目遗留的两个根级 .cmd
				// 在这里**先备份再移除** —— 备份落到 `<项目>/.we/legacy-<原名>.bak`
				// (同名备份已存在就加时间戳后缀,绝不复用)。引擎根已由
				// `.we/engine-root.txt` + 模板解析替代,用户可能在里面改过参数,
				// 所以根级脚本一律"备份成功才删"。
				// 例外:这一步没能放上可替代的 Launcher exe(`launcherInPlace` = false,
				// 迁移后用户就没法启动了)⇒ **不删**,保留根级脚本并给提示。
				std::error_code legacyExistsError;
				if (fs::is_regular_file(legacyRootCmd, legacyExistsError))
				{
					if (!launcherInPlace)
					{
						placement.Warnings.push_back("kept " + request.CmdName
							+ " in the project root: the engine launcher '" + request.SourceExeName
							+ "' is missing from the build output, so removing the old script would "
							"leave no way to start the project (build the Launcher target, then run "
							"this action again)");
						WLD_CORE_WARN(
							"[project-scaffolder] kept legacy root launcher '{0}': engine launcher '{1}' is missing",
							legacyRootCmd.u8string(), request.SourceExeName);
					}
					else
					{
						std::error_code directoryError;
						fs::create_directories(weDirectory, directoryError);
						const fs::path legacyBackup = MakeFreshBackupPath(weDirectory,
							"legacy-" + request.CmdName + ".bak", /*alwaysTimestamp=*/false);
						std::error_code backupError;
						if (legacyBackup.empty())
							backupError = std::make_error_code(std::errc::file_exists);
						else
							fs::copy_file(legacyRootCmd, legacyBackup, fs::copy_options::none, backupError);
						if (backupError)
						{
							// 备份失败 = 不删(用户内容优先)。
							placement.Warnings.push_back("kept " + request.CmdName
								+ " in the project root: could not back it up into .we/ ("
								+ backupError.message() + ")");
							WLD_CORE_WARN("[project-scaffolder] kept legacy root launcher '{0}': {1}",
								legacyRootCmd.u8string(), backupError.message());
						}
						else
						{
							std::error_code removeError;
							if (fs::remove(legacyRootCmd, removeError))
							{
								placement.Migrated.push_back(request.CmdName);
								placement.Backups.push_back(".we/" + legacyBackup.filename().u8string());
								WLD_CORE_INFO(
									"[project-scaffolder] migrated legacy root launcher '{0}' -> '{1}'",
									legacyRootCmd.u8string(), legacyBackup.u8string());
							}
							else
								placement.Warnings.push_back("moved legacy " + request.CmdName
									+ " to .we/ but could not remove the root copy: "
									+ removeError.message());
						}
					}
				}
			}

			std::string gitignoreError;
			// 根级 `launch.vs.json` 是 PROJ-13 旧版生成物的忽略项(PROJ-14 起落点在 /.vs/ 下),
			// 保留无害;`/.vs/` 覆盖新落点。
			const std::vector<std::string> ignoreEntries = {
				"*-Edit.exe", "*-Play.exe", "*-Edit.cmd", "*-Play.cmd",
				"launch.vs.json", "/.we/", "/.vs/", "/build/", "/local/" };
			if (!EnsureGitignoreEntries(projectRoot, ignoreEntries, &gitignoreError))
				placement.Warnings.push_back("could not update .gitignore: " + gitignoreError);

			std::sort(placement.EntryPoints.begin(), placement.EntryPoints.end());
			std::sort(placement.Created.begin(), placement.Created.end());
			std::sort(placement.Refreshed.begin(), placement.Refreshed.end());
			std::sort(placement.Skipped.begin(), placement.Skipped.end());
			std::sort(placement.Removed.begin(), placement.Removed.end());
			return placement;
		}

		// ---- PROJ-11/T1:给"已存在的项目"补齐构建/启动入口 ----

		// 模板选择:项目根的 template.json(向导会把它一起复制进项目)优先;没有就按
		// 示例内容标记判断(示例模板的资产/场景不会出现在干净模板里);最后回落 empty。
		std::string DetectBuildTemplateId(const fs::path& projectRoot)
		{
			std::error_code ec;
			const fs::path manifestPath = projectRoot / kTemplateManifestName;
			if (fs::is_regular_file(manifestPath, ec))
			{
				std::ifstream stream(manifestPath, std::ios::binary);
				if (stream)
				{
					const std::string text((std::istreambuf_iterator<char>(stream)),
						std::istreambuf_iterator<char>());
					std::string parseError;
					const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(text, &parseError);
					if (root && root->type == Wui::JsonValue::Type::Object)
					{
						if (const Wui::JsonValue* id = root->Find("id"))
						{
							const std::string value = id->AsString();
							if (IsValidTemplateId(value)
								&& fs::is_directory(ProjectScaffolder::TemplateDirectory(value), ec))
								return value;
						}
					}
				}
			}
			if (fs::is_directory(projectRoot / "assets" / "scripts" / "examples", ec)
				|| fs::is_regular_file(projectRoot / "assets" / "scenes" / "3DTest.wd", ec))
				return "example";
			return "empty";
		}

		// PROJ-12/T2 + PROJ-12F/T2 的旧版升级:目标缺失 ⇒ 写新内容;内容一致 ⇒ 跳过;
		// 不一致(旧版)⇒ 备份 `<名>.bak-<yyyyMMdd-HHmmss>` 后覆盖(同名备份已存在
		// 就补 `-2`/`-3`,**绝不复用/绝不覆盖旧备份**)。备份失败就停手
		// (宁可停在旧版,也不丢用户内容)。
		struct UpgradeOutcome
		{
			bool Ok = false;
			bool Created = false;    // 目标原先不存在
			bool Changed = false;    // 写入了新内容
			bool BackedUp = false;   // 覆盖前备份了旧内容
			std::string BackupName;  // 实际写出的备份文件名(仅文件名,BackedUp=true 时非空)
			std::string Error;
		};

		UpgradeOutcome WriteUpgradableFile(const fs::path& target, const std::string& expectedContent,
			bool backupOnUpgrade)
		{
			UpgradeOutcome outcome;
			std::error_code ec;
			const bool existed = fs::exists(target, ec);
			if (existed && ReadTextFile(target) == expectedContent)
			{
				outcome.Ok = true;
				return outcome;
			}
			if (existed && backupOnUpgrade)
			{
				const fs::path backup = MakeFreshBackupPath(target.parent_path(),
					target.filename().u8string() + ".bak", /*alwaysTimestamp=*/true);
				std::error_code backupError;
				if (backup.empty())
					backupError = std::make_error_code(std::errc::file_exists);
				else
					// copy_options::none = 目标已存在即失败:即使名字挑选与复制之间
					// 有别的进程插进来,也不可能覆盖别人的备份。
					fs::copy_file(target, backup, fs::copy_options::none, backupError);
				if (backupError)
				{
					outcome.Error = "cannot back up " + target.u8string() + " ("
						+ backupError.message() + ")";
					return outcome;
				}
				outcome.BackedUp = true;
				outcome.BackupName = backup.filename().u8string();
			}
			std::string writeError;
			if (!WriteTextFile(target, expectedContent, &writeError))
			{
				outcome.Error = writeError;
				return outcome;
			}
			outcome.Ok = true;
			outcome.Created = !existed;
			outcome.Changed = true;
			return outcome;
		}

		// CMakePresets.json:模板给可移植字段,这里补一份机器本地 `local` preset
		// (WE_ROOT + 可用的 WE_ENGINE_BUILD_DIR),让 VS 打开项目文件夹即可配置成功。
		// 模板没有该文件 / 结构不认识 ⇒ 记警告并跳过,不影响其余入口整理。
		void EnsureLocalCMakePresets(const fs::path& templateDirectory, const fs::path& projectRoot,
			bool backupOnUpgrade, std::vector<std::string>* created,
			std::vector<std::string>* upgraded, std::vector<std::string>* skipped,
			std::vector<std::string>* backups, std::vector<std::string>* warnings)
		{
			const fs::path source = templateDirectory / "CMakePresets.json";
			std::error_code ec;
			if (!fs::is_regular_file(source, ec))
			{
				warnings->push_back(
					"project template has no CMakePresets.json yet; the VS presets were skipped");
				return;
			}
			const std::string portable = ReadTextFile(source);
			const std::string engineRoot = ToWindowsPath(TrimTrailingSeparators(WLD_REPO_ROOT));
			std::string expected = InjectLocalPreset(portable, engineRoot,
				UsableEngineBuildDir(engineRoot));
			if (expected.empty())
			{
				// 模板结构不认识(或已经有 local):原样同步可移植字段,不伪造本地 preset。
				expected = portable;
				warnings->push_back(
					"CMakePresets.json template shape was not recognized; copied it without a local preset");
			}

			const fs::path target = projectRoot / "CMakePresets.json";
			const bool existed = fs::exists(target, ec);
			const bool userModified = existed && ReadTextFile(target) != portable;
			const UpgradeOutcome outcome = WriteUpgradableFile(target, expected,
				backupOnUpgrade && userModified);
			if (!outcome.Ok)
			{
				warnings->push_back("could not write CMakePresets.json: " + outcome.Error);
				WLD_CORE_WARN("[project-scaffolder] could not write '{0}': {1}",
					target.u8string(), outcome.Error);
				return;
			}
			if (outcome.Created)
				created->push_back("CMakePresets.json");
			else if (outcome.Changed)
				upgraded->push_back("CMakePresets.json");
			else
				skipped->push_back("CMakePresets.json");
			if (outcome.Changed)
				WLD_CORE_INFO("[project-scaffolder] wrote '{0}' ({1}local preset)",
					target.u8string(), outcome.BackedUp ? "backup + " : "");
			if (outcome.BackedUp)
				backups->push_back(outcome.BackupName);
		}
	}

	ProjectScaffolder::BuildEntryResult ProjectScaffolder::EnsureBuildEntryPoints(
		const std::filesystem::path& rawProjectRoot)
	{
		BuildEntryResult result;
		const auto fail = [&result](const std::string& text)
		{
			result.Error = text;
			return result;
		};

		std::error_code ec;
		if (rawProjectRoot.empty())
			return fail("no project root given");
		const fs::path projectRoot = fs::absolute(rawProjectRoot, ec).lexically_normal();
		if (ec)
			return fail("cannot resolve the project root " + rawProjectRoot.u8string()
				+ " (" + ec.message() + ")");
		if (!fs::is_directory(projectRoot, ec))
			return fail("project root is not a directory: " + projectRoot.u8string());
		if (!fs::is_regular_file(projectRoot / kManifestFileName, ec))
			return fail("not a WorldEngine project (project.we.yaml missing): "
				+ projectRoot.u8string());
		result.ProjectRoot = projectRoot;

		const std::string templateId = DetectBuildTemplateId(projectRoot);
		fs::path templateDirectory = TemplateDirectory(templateId);
		if (fs::is_directory(templateDirectory, ec))
		{
			result.TemplateId = templateId;
		}
		else
		{
			// 模板库被裁剪/改名时不要直接失败:换另一个模板,再不行才报错。
			const std::string fallbackId = templateId == "empty" ? "example" : "empty";
			const fs::path fallback = TemplateDirectory(fallbackId);
			if (!fs::is_directory(fallback, ec))
				return fail("project template is missing: " + templateDirectory.u8string());
			templateDirectory = fallback;
			result.TemplateId = fallbackId;
		}

		// 1) CMakeLists.txt / build.cmd:缺 ⇒ 复制;旧版(与模板不一致)⇒ 备份 .bak 后升级。
		const char* const templateManagedFiles[] = { "CMakeLists.txt", "build.cmd" };
		for (const char* fileName : templateManagedFiles)
		{
			const fs::path source = templateDirectory / fileName;
			std::error_code sourceError;
			if (!fs::is_regular_file(source, sourceError))
				return fail(std::string("project template is missing ") + fileName + ": "
					+ source.u8string());
			const UpgradeOutcome outcome = WriteUpgradableFile(projectRoot / fileName,
				ReadTextFile(source), /*backupOnUpgrade=*/true);
			if (!outcome.Ok)
				return fail(outcome.Error);
			if (outcome.Created)
				result.Created.push_back(fileName);
			else if (outcome.Changed)
			{
				result.Upgraded.push_back(fileName);
				WLD_CORE_INFO(
					"[project-scaffolder] upgraded '{0}' (previous version kept as '{1}')",
					(projectRoot / fileName).u8string(),
					outcome.BackedUp ? outcome.BackupName : std::string("<not backed up>"));
			}
			else
				result.Skipped.push_back(fileName);
			if (outcome.BackedUp)
				result.Backups.push_back(outcome.BackupName);
		}

		// 2) CMakePresets.json:模板的可移植字段 + 机器本地 local preset。
		EnsureLocalCMakePresets(templateDirectory, projectRoot, /*backupOnUpgrade=*/true,
			&result.Created, &result.Upgraded, &result.Skipped, &result.Backups, &result.Warnings);

		// 3) 项目根 exe 启动器 + .we/(engine-root.txt、必要时 .cmd 兜底)+ .vs/launch.vs.json
		//    + .vs/ProjectSettings.json(锚定 ProjectRun 并把当前启动项指向它;
		//    根级旧版 launch.vs.json 一并清掉)。
		LaunchPlacement placement = PlaceLaunchEntryPoints(projectRoot,
			projectRoot.filename().u8string());
		const auto append = [](std::vector<std::string>* target,
			const std::vector<std::string>& values)
		{
			target->insert(target->end(), values.begin(), values.end());
		};
		append(&result.Created, placement.Created);
		append(&result.Refreshed, placement.Refreshed);
		append(&result.Skipped, placement.Skipped);
		append(&result.Removed, placement.Removed);
		append(&result.Migrated, placement.Migrated);
		append(&result.Backups, placement.Backups);
		append(&result.Warnings, placement.Warnings);

		std::sort(result.Created.begin(), result.Created.end());
		std::sort(result.Skipped.begin(), result.Skipped.end());
		std::sort(result.Upgraded.begin(), result.Upgraded.end());
		std::sort(result.Refreshed.begin(), result.Refreshed.end());
		std::sort(result.Removed.begin(), result.Removed.end());
		std::sort(result.Migrated.begin(), result.Migrated.end());
		std::sort(result.Backups.begin(), result.Backups.end());
		WLD_CORE_INFO(
			"[project-scaffolder] build entry points for '{0}' (template '{1}'): created={2}, skipped={3}, upgraded={4}, refreshed={5}, removed={6}",
			projectRoot.u8string(), result.TemplateId, JoinNames(result.Created),
			JoinNames(result.Skipped), JoinNames(result.Upgraded), JoinNames(result.Refreshed),
			JoinNames(result.Removed));
		// 备份(时间戳名)单独一行:摘要行的字段口径保持不变,老探针照旧可用。
		WLD_CORE_INFO("[project-scaffolder] build entry backups for '{0}': migrated={1}, backups={2}",
			projectRoot.u8string(), JoinNames(result.Migrated), JoinNames(result.Backups));
		result.Ok = true;
		return result;
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
			// 模板里的 CMakePresets.json 已由 CopyTemplateTree 原样复制;这里补机器本地
			// `local` preset,让 VS 打开新项目文件夹即可配置(失败只警告,不影响创建)。
			std::vector<std::string> presetCreated;
			std::vector<std::string> presetUpgraded;
			std::vector<std::string> presetSkipped;
			std::vector<std::string> presetBackups;   // 新建项目走 backupOnUpgrade=false:恒为空
			std::vector<std::string> presetWarnings;
			EnsureLocalCMakePresets(templateInfo.Directory, temporary, /*backupOnUpgrade=*/false,
				&presetCreated, &presetUpgraded, &presetSkipped, &presetBackups, &presetWarnings);
			for (const std::string& warning : presetWarnings)
				WLD_CORE_WARN("[project-scaffolder] {0}", warning);

			// PROJ12-T4 修复:内容里的项目根用最终 target(改名后仍有效),文件本体写 temporary。
			const LaunchPlacement placement = PlaceLaunchEntryPoints(temporary, name, target);
			entryPoints = placement.EntryPoints;
			for (const std::string& warning : placement.Warnings)
				WLD_CORE_WARN("[project-scaffolder] {0}", warning);
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
