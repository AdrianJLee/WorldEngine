#pragma once
#include "World/Core/Export.h"
#include "World/RHI/RhiShader.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace World
{
	// HLSL → (SPIR-V / GLSL) 编译与烘焙产物解析。
	//
	// 解析顺序(P1 收尾:shader 烘焙缓存):
	//   1) 宿主注册的烘焙产物解析器(cooked 包 / VFS)——发行形态走这里,
	//      不依赖源码树与 dxc;
	//   2) 源码树按需编译(dxc + spirv-cross),结果写入内容寻址缓存,
	//      同一内容不重复调用工具——开发形态走这里。
	class WLD_API ShaderCompiler
	{
	public:
		// 编译或加载指定的 HLSL 文件，返回对应平台的字节码（SPV 或 GLSL）
		static std::vector<char> CompileOrLoad(const std::string& hlslPath, const std::string& entryPoint, const std::string& profile);
		// 按当前后端填充规范的 RHI 着色器阶段(Vulkan 填 SPIR-V,OpenGL 填 GLSL)。
		// T1 试点:OpenGL 在具备 GL_ARB_gl_spirv 且 WLD_GL_SPIRV_DIR 命中时改填 GL 目标
		// SPIR-V(见 .cpp 的 TryLoadGlSpirVStage),此时 GLSL 字段留空。
		static Rhi::ShaderStageSource CompileStage(Rhi::ShaderStage stage, const std::string& hlslPath,
			const std::string& entryPoint, const std::string& profile);

		// ---- 烘焙产物解析(宿主挂载 VFS 后注册) ----
		// 逻辑路径形如 shaders/<stem>.<EntryPoint>.spv | .glsl(与打包产物一一对应)。
		using ArtifactResolver = std::function<bool(const std::string& logicalPath, std::vector<uint8_t>& out)>;
		static void SetArtifactResolver(ArtifactResolver resolver);
		static void ClearArtifactResolver();
		// 烘焙产物逻辑路径(打包与运行时共用同一命名规则)。
		static std::string ArtifactLogicalPath(const std::string& hlslPath, const std::string& entryPoint, bool vulkan);

		// 诊断计数:验收口径为发行形态 CookedHits>0 且 ToolInvocations==0。
		static size_t CookedHitCount();
		static size_t CacheHitCount();
		static size_t ToolInvocationCount();
		static void ResetCounters();

		struct BakeResult
		{
			size_t Shaders = 0;    // 处理的 HLSL 文件数
			size_t Artifacts = 0;  // 写出的产物数(SPV + GLSL)
			size_t Failed = 0;
			std::string Error;     // 首个错误(便于打包失败时报出)
		};
		// 把一个目录下的全部 .hlsl 烘焙为 shaders/<stem>.<entry>.spv|glsl 写入 outputDir,
		// 供打包流程调用(两个入口 x 两个后端)。使用同一内容寻址缓存,重复调用不重编译。
		static BakeResult BakeDirectory(const std::filesystem::path& sourceDir,
			const std::filesystem::path& outputDir);

	private:
		static bool CompileToSpv(const std::string& hlslAbsPath, const std::string& entryPoint, const std::string& profile, const std::string& spvAbsPath);
		static bool CrossCompileToGlsl(const std::string& spvAbsPath, const std::string& glslAbsPath);

		struct CachedArtifacts
		{
			std::string Spv;
			std::string Glsl;
		};
		// 内容寻址缓存:同一(源内容 + 入口 + profile + 工具)只编译一次。
		static bool EnsureCached(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
			const std::string& profile, CachedArtifacts& out, std::string& error);
		static bool WriteBinaryFile(const std::string& filename, const std::vector<char>& data);

		// 从文件中读取二进制数据
		static std::vector<char> ReadBinaryFile(const std::string& filename);

		static const std::string dxcAbsPath;
		static const std::string spirvCrossAbsPath;
		static const std::string cacheDirAbsPath;
	};
}
