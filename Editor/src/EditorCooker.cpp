#include "wldpch.h"
#include "EditorCooker.h"

#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/ShaderUtils.h"
#include "World/Renderer/TextureCompiler.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace World::Editor
{
	namespace fs = std::filesystem;

	namespace
	{
		// Slang-T5/Slang-B1:表面材质(`.slang`)的烘焙 —— 发行包里的表面产物同样是"只读产物"。
		// 每份着色器源烘两个后端(装配时只取与当前设备后端一致的那一份):
		//   <内容根相对路径去扩展名>.PSMain.spv | .gl.spv
		//   <…>.VSMain|VSMainInstanced|VSMainSkinned.spv | .gl.spv
		//   <…>.PSMain.reflection.json | .gl.reflection.json
		// (都写在 `shaders/surface/` 下;命名只有一处实现 ——
		//  `MaterialLibrary::SurfaceArtifactLogicalPath` / `SurfaceReflectionLogicalPath`,
		//  运行时按同一份规则从包里读,见 MaterialLibrary::EnsureCookedSurfacePipeline。)
		// 反射 JSON 是参数布局(偏移/类型/绑定)的事实源:与 SPIR-V 成对发布,
		// 打包形态因此不需要任何编译器。
		struct SurfaceBakeResult
		{
			size_t Shaders = 0;
			size_t Artifacts = 0;
			size_t Failed = 0;
			// MAT-FN3:被跳过的材质函数库(`shaders/lib/**/*.slang`)—— 它们不是材质资产,
			// 不单独烘焙(随引用它们的材质一起烘)。
			size_t Libraries = 0;
			std::string Error;
		};

		// MAT-FN3:材质函数库 = 内容根下 `shaders/lib/**` 的 `.slang`
		// (docs/dev/shader-contract.md §9;与材质编辑器的库文件识别同一口径)。
		bool IsMaterialLibraryShader(const fs::path& relativePath)
		{
			const std::string normalized = relativePath.generic_string();
			const std::string marker = "shaders/lib/";
			size_t at = normalized.find(marker);
			while (at != std::string::npos)
			{
				if (at == 0 || normalized[at - 1] == '/')
					return true;
				at = normalized.find(marker, at + 1);
			}
			return false;
		}

		// MAT-FN3:表面材质编译器的 include 根(绝对路径)。顺序 = 文档 §9 的解析顺序:
		// **先材质自身目录、再项目 `assets/shaders` 根**(`#include "lib/pattern.slang"` 命中的
		// 就是这一层)。只收集真实存在的目录;顺序固定,根本身不进缓存键
		// (缓存键含被包含文件的路径 + 内容哈希,见 MaterialSurface.cpp)。
		std::vector<fs::path> SurfaceIncludeRoots(const fs::path& contentRoot, const fs::path& shaderFile)
		{
			std::vector<fs::path> roots;
			const auto add = [&roots](const fs::path& candidate)
			{
				if (candidate.empty())
					return;
				std::error_code ec;
				if (!fs::is_directory(candidate, ec))
					return;
				const fs::path absolute = fs::absolute(candidate, ec);
				if (ec || absolute.empty())
					return;
				const fs::path normalized = absolute.lexically_normal();
				if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
					roots.push_back(normalized);
			};
			add(shaderFile.parent_path());
			add(contentRoot / "shaders");
			return roots;
		}

		bool WriteFileBytes(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error)
		{
			std::error_code dirEc;
			fs::create_directories(path.parent_path(), dirEc);
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				error = "cannot write " + path.string();
				return false;
			}
			if (!bytes.empty())
				stream.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			return static_cast<bool>(stream);
		}

		// 表面材质烘焙失败时的**用户可读**消息。
		//
		// Slang 的 rich 诊断是两行(`error[E20002]: syntax error` + `--> 文件:行:列`),
		// 位置那行指向**中间目录里的包装源码**(surface_user.slang)。打包失败要让用户看到的
		// 是"哪份 `.slang`、第几行第几列、什么错",不是那个中间路径 —— 所以这里把
		// "指向用户源的诊断"重新映射回内容根相对路径。
		std::string FormatSurfaceFailure(const World::SurfaceCompileResult& compiled,
			const std::string& shaderPath)
		{
			std::string message;
			std::string fallbackMessage;   // 第一个非空 error 文本(注解解析失败没有 E 码,走这条)
			std::string location;
			// 两条判据结论一致,合起来用(实测有只带位置、不带 InUserSource 标记的形态)。
			const auto pointsAtUserSource = [](const World::SurfaceDiagnostic& diagnostic)
			{
				// Slang-B1:包装源码的文件名是 `surface_user.slang`(内核侧 B1 改名后的唯一名字)。
				return diagnostic.InUserSource
					|| diagnostic.File.find("surface_user.slang") != std::string::npos;
			};
			for (const World::SurfaceDiagnostic& diagnostic : compiled.Diagnostics)
			{
				if (diagnostic.Severity != "error")
					continue;
				if (message.empty() && diagnostic.Message.find("error[") != std::string::npos)
					message = diagnostic.Message;   // 带 E 码的整行,便于搜索
				if (fallbackMessage.empty() && !diagnostic.Message.empty())
					fallbackMessage = diagnostic.Message;   // 如 `material param annotation: <行>:<列>: <原因>`
				if (location.empty() && pointsAtUserSource(diagnostic))
				{
					const uint32_t line = diagnostic.InUserSource && diagnostic.UserLine != 0
						? diagnostic.UserLine : diagnostic.Line;
					const uint32_t column = diagnostic.InUserSource && diagnostic.UserColumn != 0
						? diagnostic.UserColumn : diagnostic.Column;
					if (line != 0)
					{
						location = " at " + shaderPath + ":" + std::to_string(line)
							+ ":" + std::to_string(column);
					}
				}
			}
			if (location.empty())
			{
				// 用户源没在诊断里出现过 = 错误在引擎包装模板/契约里 —— 这时才报包装位置。
				for (const World::SurfaceDiagnostic& diagnostic : compiled.Diagnostics)
				{
					if (diagnostic.Severity != "error" || diagnostic.File.empty() || diagnostic.Line == 0)
						continue;
					location = " (engine wrapper " + diagnostic.File + ":"
						+ std::to_string(diagnostic.Line) + ":" + std::to_string(diagnostic.Column) + ")";
					break;
				}
			}
			// 注解解析失败时没有 E 码、也没有工具输出(工具根本没被调用)——先把解析器给的真实原因
			// 抬上来,否则用户只会看到下面那句无用的 "Slang reported no diagnostics"。
			if (message.empty() && !fallbackMessage.empty())
				message = fallbackMessage;
			if (message.empty())
			{
				// 诊断里只有位置行时(引擎把 `error[E…]` 那行并进了位置诊断)→ 取工具输出的
				// **第一行**:它正是 `error[E20002]: syntax error`,单行、带稳定码、可直接搜。
				// (整段原始输出会把源码片段/插入符塞进一行日志里,反而不好读。)
				std::string firstLine = compiled.RawToolOutput;
				const size_t newline = firstLine.find('\n');
				if (newline != std::string::npos)
					firstLine.erase(newline);
				if (!firstLine.empty() && firstLine.back() == '\r')
					firstLine.pop_back();
				message = firstLine.empty() ? std::string("Slang reported no diagnostics") : firstLine;
			}
			if (message.empty())
				message = fallbackMessage;   // 注解解析失败:把解析器给的真实原因抬上来
			return message + location;
		}

		SurfaceBakeResult BakeSurfaceMaterials(const fs::path& contentRoot, const fs::path& outputDir)
		{
			SurfaceBakeResult result;
			std::error_code ec;
			if (!fs::is_directory(contentRoot, ec))
				return result;   // 没有内容根 = 没有表面材质,不是失败

			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(
				contentRoot, fs::directory_options::skip_permission_denied, ec))
			{
				// Slang-B1:表面材质源的唯一扩展名是 `.slang`。
				const std::string extension = entry.path().extension().string();
				if (!entry.is_regular_file(ec) || extension != ".slang")
					continue;

				std::ifstream stream(entry.path(), std::ios::binary);
				if (!stream)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot read surface shader: " + entry.path().string();
					continue;
				}
				std::ostringstream buffer;
				buffer << stream.rdbuf();
				const std::string source = buffer.str();
				if (source.empty())
					continue;

				std::error_code relEc;
				fs::path relative = fs::relative(entry.path(), contentRoot, relEc);
				if (relEc || relative.empty())
					relative = entry.path().filename();
				// 内容根相对路径(**带**扩展名)—— 与 Material::SurfaceKey()/运行时的消费键同一口径;
				// 同时当作排列键:编辑器与 cooker 对同一份着色器源因此落在**同一个缓存条目**上
				// (cook 不重编编辑器刚编过的东西,产物也一致)。
				const std::string shaderPath = relative.generic_string();

				// MAT-FN3:材质函数库(`shaders/lib/**`)不是材质资产 —— 它没有 `Evaluate` 入口,
				// **不单独编译/烘焙**(随引用它的材质一起烘,见 docs/dev/shader-contract.md §9)。
				// 只跳过并计数,不算失败。
				if (IsMaterialLibraryShader(relative))
				{
					++result.Libraries;
					continue;
				}
				++result.Shaders;

				// MAT-FN3:材质 `#include` 的解析根(材质自身目录 + 项目 `assets/shaders` 根),
				// 绝对路径 —— 与材质编辑器的实时预览同一个口径。
				const std::vector<fs::path> includeRoots = SurfaceIncludeRoots(contentRoot, entry.path());
				for (const World::SurfaceShaderBackend backend :
					{ World::SurfaceShaderBackend::VulkanSpirV, World::SurfaceShaderBackend::OpenGLSpirV })
				{
					const bool gl = backend == World::SurfaceShaderBackend::OpenGLSpirV;
					const World::SurfaceCompileResult compiled =
						World::MaterialSurfaceCompiler::CompileSurface(source, shaderPath, backend, includeRoots);
					if (!compiled.Success)
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = "surface shader '" + shaderPath + "' ("
								+ World::MaterialSurfaceCompiler::BackendName(backend) + ") failed: "
								+ FormatSurfaceFailure(compiled, shaderPath);
						continue;
					}

					std::string writeError;
					const fs::path pixelOut = outputDir / World::MaterialLibrary::SurfaceArtifactLogicalPath(
						shaderPath, "PSMain", gl);
					if (!WriteFileBytes(pixelOut, compiled.Artifact.Bytecode, writeError))
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = writeError;
						continue;
					}
					++result.Artifacts;

					for (const World::SurfaceVertexStage& stage : compiled.Artifact.VertexStages)
					{
						const fs::path vertexOut = outputDir / World::MaterialLibrary::SurfaceArtifactLogicalPath(
							shaderPath, stage.EntryPoint, gl);
						if (!WriteFileBytes(vertexOut, stage.Bytecode, writeError))
						{
							++result.Failed;
							if (result.Error.empty())
								result.Error = writeError;
							break;
						}
						++result.Artifacts;
					}

					const std::string reflectionPath =
						World::MaterialSurfaceCompiler::ReflectionPath(compiled.Artifact);
					std::ifstream reflectionStream(reflectionPath, std::ios::binary);
					if (reflectionPath.empty() || !reflectionStream)
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = "surface shader '" + shaderPath
								+ "' has no Slang reflection JSON (-reflection-json)";
						continue;
					}
					std::ostringstream reflectionBuffer;
					reflectionBuffer << reflectionStream.rdbuf();
					const std::string reflectionText = reflectionBuffer.str();
					const std::vector<uint8_t> reflectionBytes(reflectionText.begin(), reflectionText.end());
					const fs::path reflectionOut = outputDir /
						World::MaterialLibrary::SurfaceReflectionLogicalPath(shaderPath, gl);
					if (!WriteFileBytes(reflectionOut, reflectionBytes, writeError))
					{
						++result.Failed;
						if (result.Error.empty())
							result.Error = writeError;
						continue;
					}
					++result.Artifacts;
				}
			}
			return result;
		}

		std::string LowercaseExtension(const fs::path& path)
		{
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension;
		}

		// M4-TEX P3:保证 cooked 里有内容根的源图与 `.wtex` sidecar(只补**缺失**的那些)。
		//
		// 为什么需要:`CookPipeline` 的增量只认 `cook.db.json`(源字节指纹),它不会发现 cooked 里的文件
		// 被人删掉了 —— 而 `--strip-source-textures` 正是"删掉 cooked 里源图"的合法动作。没有这一步,
		// 剥离之后的**下一次普通 cook** 会静默产出"只剩产物"的开发包(与"默认保留源图"的契约相反)。
		// 代价:只有紧跟剥离的那一次 cook 会真的拷文件(其余情况目标已存在,一个字节都不动)。
		struct CookedSourceCopyResult
		{
			size_t Sources = 0;    // 补回的源图数
			size_t Sidecars = 0;   // 补回的 `.wtex` 数
			size_t Failed = 0;
			std::string Error;
		};

		CookedSourceCopyResult EnsureCookedSourceCopies(const fs::path& contentRoot,
			const fs::path& cookedContentDir)
		{
			CookedSourceCopyResult result;
			std::error_code walkEc;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(contentRoot,
				fs::directory_options::skip_permission_denied, walkEc))
			{
				if (walkEc)
					break;
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc))
					continue;
				const std::string extension = LowercaseExtension(entry.path());
				const bool isSource = World::TextureCompiler::IsTextureSourceExtension(extension);
				const bool isSidecar = extension == ".wtex";
				if (!isSource && !isSidecar)
					continue;
				std::error_code relativeEc;
				const fs::path relative = fs::relative(entry.path(), contentRoot, relativeEc);
				if (relativeEc || relative.empty())
					continue;
				const fs::path target = cookedContentDir / relative;
				std::error_code existsEc;
				if (fs::is_regular_file(target, existsEc))
					continue;
				std::error_code copyEc;
				fs::create_directories(target.parent_path(), copyEc);
				if (!copyEc)
					fs::copy_file(entry.path(), target, fs::copy_options::none, copyEc);
				if (copyEc)
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot restore " + target.string() + ": " + copyEc.message();
					continue;
				}
				if (isSource)
					++result.Sources;
				else
					++result.Sidecars;
			}
			return result;
		}

		// M4-TEX P3:`--strip-source-textures` —— 发行包只带烘焙产物(`.wtexc`)。
		//
		// 事实源是**内容根**(不是 cooked 目录):逐张源图按"内容根相对路径"算出产物与源图在
		// `cooked/cooked/` 里的落点,因此引擎着色器(`shaders/**` 来自引擎目录)、本地化、
		// 任何非内容根产物都不可能被误删。有源图但没有产物 = 记失败并**保留**该源图
		// (宁可包里多一张源图,也不静默删掉唯一能读的数据)。
		// 三遍:①内容根逐张要求产物在场并删源图/sidecar;②按 cooked 里的每张 `.wtexc` 反推它的
		// 源图+sidecar 落点(清"成对的孤儿");③清**已删资产**在 cooked 里的残留拷贝 —— 判据是
		// "既不在内容根、也不在引擎资产树的同相对路径上",所以引擎自带内容与仍在内容根的源图都不会碰。
		struct TextureStripResult
		{
			size_t Sources = 0;     // 内容根下的源图数
			size_t Artifacts = 0;   // 确认存在、保留的 `.wtexc` 数
			size_t Stripped = 0;    // 实际删掉的文件数(源图 + `.wtex` sidecar)
			size_t Orphans = 0;     // 源图已不在内容根、只剩 cooked 旧拷贝的清理数(第二/三遍)
			size_t Unattributed = 0; // 导入器产物等"非内容根源图":只审计、不删
			size_t Failed = 0;
			std::string Error;
		};

		TextureStripResult StripSourceTextures(const fs::path& contentRoot, const fs::path& cookedContentDir)
		{
			TextureStripResult result;
			std::error_code rootEc;
			if (!fs::is_directory(contentRoot, rootEc))
			{
				++result.Failed;
				result.Error = "content root is not a directory: " + contentRoot.string();
				return result;
			}

			// 删除这两条路径(源图 + sidecar)。不存在 = 不是错误(没设置 / 已经删过)。
			const auto removePair = [&result](const fs::path& sourcePath, const fs::path& sidecarPath) -> size_t
			{
				size_t removed = 0;
				for (const fs::path& target : { sourcePath, sidecarPath })
				{
					std::error_code removeEc;
					if (fs::remove(target, removeEc))
						++removed;
					else if (removeEc && result.Error.empty())
						result.Error = "cannot remove " + target.string() + ": " + removeEc.message();
				}
				return removed;
			};

			// 第一遍(内容根侧):逐张源图要求产物在场 —— 有源图没有产物 = 失败并保留源图。
			std::error_code walkEc;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(contentRoot,
				fs::directory_options::skip_permission_denied, walkEc))
			{
				if (walkEc)
					break;
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc))
					continue;
				const std::string extension = LowercaseExtension(entry.path());
				if (!World::TextureCompiler::IsTextureSourceExtension(extension))
					continue;
				++result.Sources;

				std::error_code relativeEc;
				const fs::path relative = fs::relative(entry.path(), contentRoot, relativeEc);
				if (relativeEc || relative.empty())
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "cannot relativize " + entry.path().string();
					continue;
				}
				const std::string logical = relative.generic_string();
				const fs::path artifactPath = cookedContentDir / (logical + ".wtexc");
				std::error_code existsEc;
				if (!fs::is_regular_file(artifactPath, existsEc))
				{
					++result.Failed;
					if (result.Error.empty())
						result.Error = "source texture has no baked artifact: " + logical
							+ " (expected " + artifactPath.string() + ")";
					continue;   // 没有产物 ⇒ 不删这张源图
				}
				++result.Artifacts;
				result.Stripped += removePair(cookedContentDir / logical, cookedContentDir / (logical + ".wtex"));
			}

			// 第二遍(cooked 侧):清**孤儿**拷贝。CookPipeline 只覆盖/新增、从不回收 —— 源图被删掉后
			// cooked 里的旧拷贝会一直留着,只扫源树就会漏掉它,发行包于是仍然带源图。这里逐张 `.wtexc`
			// 反推它的源图 + sidecar 落点(仍是"与产物成对"的文件),因此引擎/本地化等非纹理产物不受影响。
			std::error_code bakedWalkEc;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(cookedContentDir,
				fs::directory_options::skip_permission_denied, bakedWalkEc))
			{
				if (bakedWalkEc)
					break;
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc) || entry.path().extension() != ".wtexc")
					continue;
				const fs::path sourcePath = entry.path().parent_path() / entry.path().filename().stem();
				const fs::path sidecarPath = sourcePath.parent_path()
					/ (sourcePath.filename().string() + ".wtex");
				const size_t removed = removePair(sourcePath, sidecarPath);
				result.Stripped += removed;
				result.Orphans += removed;
			}

			// 第三遍(cooked 侧**审计,不删**):上面两类之外仍留在 cooked 里的源图扩展名文件/sidecar
			// 不是内容根的资产,而是**导入器产物** —— 例如 glTF 导入按"扁平 `models/<源 stem>.wmodel` +
			// materials/ + textures/"布局生成 `textures/<源 stem>_0.png`,材质直接引用它。删掉它会打断
			// 引用它的材质,所以这里只计数、告警,不越界删除。剥离的保证因此精确表述为:
			// **内容根里的源图与 `.wtex` sidecar 不进包**(它们要么有 `.wtexc`,要么在内容根缺席)。
			const fs::path engineAssetRoot = fs::path(WLD_WORLD_DIR) / "assets";
			std::string unattributedSample;
			std::error_code sweepEc;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(cookedContentDir,
				fs::directory_options::skip_permission_denied, sweepEc))
			{
				if (sweepEc)
					break;
				std::error_code fileEc;
				if (!entry.is_regular_file(fileEc))
					continue;
				const std::string extension = LowercaseExtension(entry.path());
				if (extension != ".wtex" && !World::TextureCompiler::IsTextureSourceExtension(extension))
					continue;
				std::error_code relativeEc;
				const fs::path relative = fs::relative(entry.path(), cookedContentDir, relativeEc);
				if (relativeEc || relative.empty())
					continue;
				std::error_code existsEc;
				if (fs::is_regular_file(contentRoot / relative, existsEc)
					|| fs::is_regular_file(engineAssetRoot / relative, existsEc))
					continue;
				++result.Unattributed;
				if (result.Unattributed <= 5)
					unattributedSample += (unattributedSample.empty() ? "" : ", ") + relative.generic_string();
			}
			if (result.Unattributed)
			{
				WLD_CORE_WARN("[tex] {0} file(s) with texture-source extensions are importer outputs, not "
					"content sources (kept): {1}", result.Unattributed, unattributedSample);
			}
			return result;
		}

		// `--cook --check` 的一次性输出目录:CookPipeline 的产物与 cook.db.json 都写进这里,
		// 函数返回(含异常路径)时整体删除 —— check 因此不会触碰 build 树里的增量判定事实源。
		class CookCheckScratch
		{
		public:
			CookCheckScratch()
			{
				const fs::path tempRoot = fs::temp_directory_path();
				for (int attempt = 0; attempt < 64; ++attempt)
				{
					const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
					const fs::path candidate = tempRoot
						/ ("wld-cook-check-" + std::to_string(::GetCurrentProcessId()) + "-"
							+ std::to_string(tick) + "-" + std::to_string(attempt));
					std::error_code createEc;
					if (fs::create_directory(candidate, createEc) && !createEc)
					{
						m_Path = candidate;
						return;
					}
				}
				throw std::runtime_error("cannot create cook check scratch directory under "
					+ tempRoot.string());
			}

			~CookCheckScratch()
			{
				std::error_code removeEc;
				fs::remove_all(m_Path, removeEc);
			}

			CookCheckScratch(const CookCheckScratch&) = delete;
			CookCheckScratch& operator=(const CookCheckScratch&) = delete;

			const fs::path& Path() const { return m_Path; }

		private:
			fs::path m_Path;
		};

		// WLD-L10N-S2:发行包的语言子集(§11.1 打包布局)。
		//
		// 层序 = 运行时的注册序:engine(`WLD_WORLD_DIR/assets/localization`,库自带件/引擎键)
		// → project(`WLD_PROJECT_DIR/assets/localization`,项目覆盖 + 项目文案);**编辑器层不进游戏包**
		// (`Editor/assets/localization` 是编辑器 UI 的语言包,游戏里没有它的消费者)。
		// 落点:`<publish>/localization/<engine|project>/<语言>/**`,层内相对路径(含域子目录)原样保留;
		// 只拷域文件 `*.json`(编译产物 `catalog.json` 同样以 `.json` 落在语言根,一起拷走)。
		struct LocalizationCopyResult
		{
			std::string Error;
			std::vector<std::string> Languages;   // 实际带上内容的语言(按输入序)
			std::vector<std::string> Skipped;     // 选中但两层都没有内容的语言
			size_t Files = 0;
		};

		// 一层目录下扫描到的语言子目录名(排序去重);目录不存在 = 空(不是错误)。
		std::vector<std::string> ScanLocalizationLanguages(const fs::path& layerDirectory)
		{
			std::vector<std::string> languages;
			std::error_code ec;
			if (!fs::is_directory(layerDirectory, ec))
				return languages;
			for (const fs::directory_entry& entry : fs::directory_iterator(layerDirectory,
				fs::directory_options::skip_permission_denied, ec))
			{
				std::error_code entryEc;
				if (entry.is_directory(entryEc))
					languages.push_back(entry.path().filename().string());
			}
			std::sort(languages.begin(), languages.end());
			languages.erase(std::unique(languages.begin(), languages.end()), languages.end());
			return languages;
		}

		// 把一层的一门语言按相对路径拷进发行目录;返回拷贝的文件数。
		// 层目录/语言目录不存在 = 0(该层这门语言没内容,不是失败);拷贝失败写入 error 并就地停止。
		size_t CopyLocalizationLanguage(const fs::path& layerDirectory, const std::string& language,
			const fs::path& destination, std::string& error)
		{
			const fs::path source = layerDirectory / language;
			std::error_code ec;
			if (!fs::is_directory(source, ec))
				return 0;
			size_t copied = 0;
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source,
				fs::directory_options::skip_permission_denied, ec))
			{
				std::error_code entryEc;
				if (!entry.is_regular_file(entryEc) || entry.path().extension() != ".json")
					continue;   // 只拷域文件(与产物 catalog.json):不放工具/术语表等其它形态
				const fs::path target = destination / entry.path().lexically_relative(source);
				std::error_code directoryEc;
				fs::create_directories(target.parent_path(), directoryEc);
				if (directoryEc)
				{
					error = "cannot create " + target.parent_path().string() + ": " + directoryEc.message();
					return copied;
				}
				std::error_code copyEc;
				fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, copyEc);
				if (copyEc)
				{
					error = "cannot copy " + entry.path().string() + ": " + copyEc.message();
					return copied;
				}
				++copied;
			}
			if (ec && error.empty())
				error = "cannot scan " + source.string() + ": " + ec.message();
			return copied;
		}

		LocalizationCopyResult CopyLocalizationSubset(const CookOptions& options)
		{
			LocalizationCopyResult result;
			const fs::path engineLayer = fs::path(WLD_WORLD_DIR) / "assets" / "localization";
			const fs::path projectLayer = fs::path(WLD_PROJECT_DIR) / "assets" / "localization";

			// 空列表 = 扫描到的全部语言(engine ∪ project);显式列表原样尊重(不存在的语言记 skipped)。
			std::vector<std::string> languages = options.Languages;
			if (languages.empty())
			{
				languages = ScanLocalizationLanguages(engineLayer);
				for (const std::string& language : ScanLocalizationLanguages(projectLayer))
					languages.push_back(language);
				std::sort(languages.begin(), languages.end());
				languages.erase(std::unique(languages.begin(), languages.end()), languages.end());
			}

			for (const std::string& language : languages)
			{
				if (language.empty())
					continue;
				const size_t engineFiles = CopyLocalizationLanguage(engineLayer, language,
					options.PublishDir / "localization" / "engine" / language, result.Error);
				const size_t projectFiles = CopyLocalizationLanguage(projectLayer, language,
					options.PublishDir / "localization" / "project" / language, result.Error);
				if (!result.Error.empty())
					return result;
				if (engineFiles == 0 && projectFiles == 0)
					result.Skipped.push_back(language);
				else
					result.Languages.push_back(language);
				result.Files += engineFiles + projectFiles;
			}
			return result;
		}
	}

	CookResult CookProject(const CookOptions& options)
	{
		CookResult result;
		try
		{
			// W7-2 `--check`:只做资产预检(脚本在这里就会被编译把关),不碰发行目录。
			if (options.CheckOnly)
				WLD_CORE_INFO("Check-only cook: shader bake, package, runtime copy and release manifest are skipped");
			else
				fs::create_directories(options.PublishDir);

			// 1. 项目清单(单一事实源)。
			const fs::path projectManifestPath = std::string(WLD_PROJECT_DIR) + "project.we.yaml";
			World::Asset::ProjectManifest manifest;
			std::string manifestError;
			if (!World::Asset::ProjectManifest::Load(projectManifestPath, &manifest, &manifestError))
				throw std::runtime_error("Project manifest load failed: " + manifestError);
			if (manifest.Packages.empty())
				throw std::runtime_error("Project manifest declares no packages");

			// 2. 启动场景覆盖(编辑器里当前打开的场景优先)。
			if (!options.StartSceneOverride.empty())
				manifest.StartScene = options.StartSceneOverride.generic_string();

			// 3. 增量资产烘焙。正常 cook 用构建期缓存目录;check 用一次性 scratch,
			//    跑完(含失败/异常)即删除,不写 cooked/、不写 cook.db.json。
			fs::path cookedDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "cooked");
			std::unique_ptr<CookCheckScratch> checkScratch;
			if (options.CheckOnly)
			{
				checkScratch = std::make_unique<CookCheckScratch>();
				cookedDir = checkScratch->Path();
				WLD_CORE_INFO("Check-only cook: artifacts go to scratch directory {0} (removed on exit)",
					cookedDir.string());
			}
			World::Asset::CookPipeline pipeline(World::Asset::DefaultImporters());
			World::Asset::CookSummary summary;
			const std::vector<World::Asset::CookEntryResult> results =
				pipeline.Cook(manifest, projectManifestPath, cookedDir, false, &summary);
			std::string failedList;
			for (const World::Asset::CookEntryResult& entry : results)
				if (entry.Failed)
				{
					WLD_CORE_ERROR("Cook failed: {0}: {1}", entry.Path, entry.Error);
					if (!failedList.empty())
						failedList += "; ";
					failedList += entry.Path + ": " + entry.Error;
				}
			WLD_CORE_INFO("Cooked {0} assets ({1} changed, {2} skipped, {3} failed)",
				summary.Total, summary.Changed, summary.Skipped, summary.Failed);
			if (summary.Failed)
				throw std::runtime_error("Asset cooking failed (" + std::to_string(summary.Failed)
					+ " failed): " + failedList);
			result.AssetsTotal = summary.Total;
			result.AssetsChanged = summary.Changed;
			result.AssetsSkipped = summary.Skipped;
			if (options.CheckOnly)
			{
				// 预检成功:摘要已填好,不创建发行目录、不烘焙着色器、不打包、不拷运行时。
				result.Ok = true;
				return result;
			}

			// 4. 着色器烘焙:引擎 Slang 源 → 产物写入 cooked 目录,随内容包发布 ——
			// 发行版 Runtime 不再依赖源码树与任何编译器。
			//   4a. 老口径(BakeDirectory):每个入口 .spv(Vulkan)+ .gl.spv(GL 目标 SPIR-V;旧 .glsl 兜底已随 T6 删除);
			//   4b. Slang-T5 双目标(BakeDistributionTargets):每个入口 .spv + .gl.spv
			//       —— GL 4.6 直吃 GL 目标 SPIR-V,发行形态不再回落到 GLSL 文本;
			//   4c. Slang-T5 表面材质:内容根下的每份 .slang → 双目标 SPIR-V + 反射 JSON。
			const fs::path engineShaderDir = fs::absolute(std::string(WLD_WORLD_DIR) + "assets/shaders");
			const fs::path cookedContentDir = cookedDir / "cooked";
			const World::ShaderCompiler::BakeResult baked =
				World::ShaderCompiler::BakeDirectory(engineShaderDir, cookedContentDir);
			WLD_CORE_INFO("Baked {0} shaders ({1} artifacts, {2} failed)",
				baked.Shaders, baked.Artifacts, baked.Failed);
			if (baked.Failed)
				throw std::runtime_error("Shader baking failed: " + baked.Error);

			const World::ShaderCompiler::BakeResult distribution =
				World::ShaderCompiler::BakeDistributionTargets(engineShaderDir, cookedContentDir);
			WLD_CORE_INFO("Baked distribution shader targets: {0} shaders, {1} SPIR-V artifacts, "
				"{2} failed", distribution.Shaders, distribution.Artifacts, distribution.Failed);
			if (distribution.Failed)
				throw std::runtime_error("Distribution shader baking failed: " + distribution.Error);

			const fs::path surfaceContentRoot = manifest.ResolveContentRoot(projectManifestPath);
			const SurfaceBakeResult surfaces = BakeSurfaceMaterials(surfaceContentRoot, cookedContentDir);
			WLD_CORE_INFO("Baked surface materials: {0} shaders, {1} artifacts ({2} failed, "
				"{3} material functions skipped)",
				surfaces.Shaders, surfaces.Artifacts, surfaces.Failed, surfaces.Libraries);
			if (surfaces.Failed)
				throw std::runtime_error("Surface material baking failed: " + surfaces.Error);

			result.DistributionArtifacts = distribution.Artifacts;
			result.SurfaceShaders = surfaces.Shaders;
			result.SurfaceArtifacts = surfaces.Artifacts;
			result.ShaderArtifacts = baked.Artifacts + distribution.Artifacts + surfaces.Artifacts;

			// 4d. 贴图烘焙(M4-TEX P3):内容根下的源图 + `.wtex` sidecar → `cooked/<逻辑路径>.wtexc`。
			//     读取路径 = 内容根(与表面材质同一解析),产物落点 = 内容根相对路径 + `.wtexc`;
			//     打包阶段(step 5)按目录遍历,`.wtexc` 自动进包(PackageProvider 不需要改)。
			//     缓存 = `<build>/texture-cache/<源sha256>-<设置hash>-v<产物版本>.wtexc`,跨 cook 复用;
			//     命中缓存但产物缺失时内核会补齐产物,所以"删掉 cooked/ 再 cook"也是全命中。
			//     注意 `--check` 在 step 3 就返回(与着色器烘焙同一口径),不会走到这里。
			const fs::path textureCacheDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "texture-cache");
			const World::TextureBakeStats textures = World::TextureCompiler::BakeDirectory(
				surfaceContentRoot, cookedContentDir, textureCacheDir, World::TextureBakeOptions {});
			WLD_CORE_INFO("[tex] baked={0} uptodate={1} skipped={2} failed={3}",
				textures.Baked, textures.UpToDate, textures.Skipped, textures.Failed);
			// 内核把"读不了的源""没产物的源"也写进 Errors(计数在 skipped/failed 上),逐条打出来:
			// 打包日志是用户唯一能看到的地方。
			for (const std::string& textureError : textures.Errors)
				WLD_CORE_ERROR("[tex] {0}", textureError);
			if (textures.Failed)
				throw std::runtime_error("Texture baking failed (" + std::to_string(textures.Failed)
					+ " failed): "
					+ (textures.Errors.empty() ? std::string("unknown error") : textures.Errors.front()));
			result.TextureBaked = textures.Baked;
			result.TextureUpToDate = textures.UpToDate;
			result.TextureSkipped = textures.Skipped;

			// 4d-1. 源图/sidecar 在场性自愈(见 EnsureCookedSourceCopies:剥离之后的下一次
			//       普通 cook 必须重新带上源图,否则"默认保留源图"的开发包契约会静默失效)。
			const CookedSourceCopyResult restored =
				EnsureCookedSourceCopies(surfaceContentRoot, cookedContentDir);
			WLD_CORE_INFO("[tex] restored source copies: sources={0} sidecars={1} failed={2}",
				restored.Sources, restored.Sidecars, restored.Failed);
			if (restored.Failed)
				throw std::runtime_error("Restoring cooked source copies failed: " + restored.Error);
			result.RestoredSourceCopies = restored.Sources + restored.Sidecars;

			// 4e. `--strip-source-textures`(默认关):发货包只带 `.wtexc`,源图与 sidecar 从 cooked 目录删掉。
			//     删除范围限定在"内容根映射过来的路径"内(见 StripSourceTextures),打包前一步完成。
			if (options.StripSourceTextures)
			{
				const TextureStripResult stripped = StripSourceTextures(surfaceContentRoot, cookedContentDir);
				WLD_CORE_INFO("[tex] strip-source-textures: sources={0} artifacts={1} removed={2} "
					"(orphans={3}) unattributed={4} failed={5}",
					stripped.Sources, stripped.Artifacts, stripped.Stripped, stripped.Orphans,
					stripped.Unattributed, stripped.Failed);
				if (!stripped.Error.empty())
					WLD_CORE_ERROR("[tex] {0}", stripped.Error);
				if (stripped.Failed)
					throw std::runtime_error("Source texture stripping failed ("
						+ std::to_string(stripped.Failed) + " failed): " + stripped.Error);
				result.StrippedSourceTextures = stripped.Stripped;
			}

			// 5. 打包 cooked 产物为发行包。
			const fs::path outPakFile = options.PublishDir / manifest.Packages[0];
			fs::create_directories(outPakFile.parent_path());
			std::error_code pakEc;
			if (!World::Vfs::PackageProvider::BuildFromDirectory(cookedDir / "cooked", outPakFile, pakEc))
				throw std::runtime_error("Package build failed: " +
					(pakEc ? pakEc.message() : outPakFile.string()));

			// 6. 拷贝运行时:Runtime.exe + 单份 WorldRuntime.dll + bin/Game.dll。
			const fs::path srcRuntimeOutputDir = fs::absolute(std::string(WLD_OUTPUT_DIR) + "Runtime/" + WLD_BUILD_TYPE);
			const fs::path srcRuntimeExe = srcRuntimeOutputDir / "Runtime.exe";
			if (!fs::is_regular_file(srcRuntimeExe))
				throw std::runtime_error("Runtime.exe could not be located: " + srcRuntimeExe.string());
			fs::copy_file(srcRuntimeExe, options.PublishDir / "Runtime.exe", fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied Runtime executable from: {0}", srcRuntimeExe.string());

			const fs::path srcRuntimeDll = srcRuntimeOutputDir / "WorldRuntime.dll";
			if (!fs::is_regular_file(srcRuntimeDll))
				throw std::runtime_error("WorldRuntime.dll could not be located: " + srcRuntimeDll.string());
			fs::copy_file(srcRuntimeDll, options.PublishDir / "WorldRuntime.dll", fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied WorldRuntime.dll from: {0}", srcRuntimeDll.string());

			const fs::path srcGameDll = fs::absolute(std::string(WLD_OUTPUT_DIR) +
				"bin/" + WLD_BUILD_TYPE + "/Game/" + WLD_BUILD_TYPE + "/Game.dll");
			if (!fs::is_regular_file(srcGameDll))
				throw std::runtime_error("Game.dll could not be located: " + srcGameDll.string());
			fs::create_directories(options.PublishDir / "bin");
			fs::copy_file(srcGameDll, options.PublishDir / "bin" / "Game.dll",
				fs::copy_options::overwrite_existing);
			WLD_CORE_INFO("Copied Game.dll into bin/");

			// 7. 语言包:发行包只带选中的语言(engine / project 两层;编辑器层不进包,§11.1)。
			const LocalizationCopyResult localization = CopyLocalizationSubset(options);
			if (!localization.Error.empty())
				throw std::runtime_error("Localization copy failed: " + localization.Error);
			for (const std::string& language : localization.Skipped)
				WLD_CORE_WARN("Localization subset skipped: 语言 {0} 在 engine/project 两层都没有内容", language);
			{
				std::string languagesText;
				for (const std::string& language : localization.Languages)
				{
					if (!languagesText.empty())
						languagesText += ", ";
					languagesText += language;
				}
				WLD_CORE_INFO("Copied localization: {0} languages ({1}), {2} files",
					localization.Languages.size(), languagesText, localization.Files);
			}
			result.LocalizationLanguages = localization.Languages.size();
			result.LocalizationFiles = localization.Files;

			// 8. 写发行清单。
			std::string saveError;
			if (!World::Asset::ProjectManifest::Save(options.PublishDir / "project.we.yaml", manifest, &saveError))
				throw std::runtime_error("Project manifest save failed: " + saveError);

			WLD_CORE_INFO("Game Cooked Successfully to {0}", options.PublishDir.string());
			result.Ok = true;
		}
		catch (const std::exception& error)
		{
			result.Error = error.what();
			WLD_CORE_ERROR("Game cooking failed: {0}", result.Error);
		}
		catch (...)
		{
			result.Error = "Unknown background cooking error.";
			WLD_CORE_ERROR("{0}", result.Error);
		}
		return result;
	}
}
