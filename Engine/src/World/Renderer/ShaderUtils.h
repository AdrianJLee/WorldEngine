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
	// HLSL/Slang → (SPIR-V / GLSL) 编译与烘焙产物解析。
	//
	// Slang-T2:引擎着色器(`Engine/assets/shaders/**`)是**Slang 单源**形态(组合采样器
	// `Sampler2D` + 显式 `[[vk::binding(N,S)]]`),每个 stage 编出两份 SPIR-V permutation:
	//   - Vulkan:`-profile <stage>_6_0`(Slang 直出 Vulkan SPIR-V);
	//   - OpenGL:`-profile <stage>_5_0+spirv_1_0`(ARB_gl_spirv 只接受 SPIR-V 1.0,
	//     且不接受 OpTypeSampler,必须组合采样器 —— 依据 T1 实测)。
	//
	// 解析顺序(P1 收尾:shader 烘焙缓存;Slang-T2:GL 也走 SPIR-V):
	//   1) 宿主注册的烘焙产物解析器(cooked 包 / VFS)——发行形态走这里,
	//      不依赖源码树与编译器;
	//   2) 内容寻址缓存(键 = 源内容 + 入口 + profile + 目标后端 + 工具身份 + 契约版本),
	//      未命中时用 slangc 现场编译 —— 开发形态走这里。
	//
	// 过渡期兜底(用户已定 T6 删除):OpenGL 拿不到 GL 目标 SPIR-V 时退回 GLSL 文本
	// (由 spirv-cross 从 Vulkan 目标 SPIR-V 产出),并在日志里写清"哪条路径、为什么"。
	class WLD_API ShaderCompiler
	{
	public:
		// 编译或加载指定的 HLSL 文件，返回对应平台的字节码（SPV 或 GLSL）
		static std::vector<char> CompileOrLoad(const std::string& hlslPath, const std::string& entryPoint, const std::string& profile);
		// 按当前后端填充规范的 RHI 着色器阶段:
		//   - Vulkan → 填 Vulkan 目标 SPIR-V(Vulkan profile);
		//   - OpenGL → 具备 GL_ARB_gl_spirv 时填 GL 目标 SPIR-V(Slang-T2 起是**默认**路径),
		//     否则填 GLSL 文本兜底(ERROR 日志说明原因)。
		static Rhi::ShaderStageSource CompileStage(Rhi::ShaderStage stage, const std::string& hlslPath,
			const std::string& entryPoint, const std::string& profile);

		// ---- 烘焙产物解析(宿主挂载 VFS 后注册) ----
		// 逻辑路径形如 shaders/<stem>.<EntryPoint>.spv(Vulkan)| .glsl(GLSL 兜底);
		// GL 目标 SPIR-V 的常规产物名是 shaders/<stem>.<EntryPoint>.gl.spv(见 .cpp)。
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
		// Slang-T2:编译器换成 slangc(Vulkan 目标 SPIR-V + spirv-cross GLSL 兜底);
		// GL 目标 SPIR-V 仍由运行时从内容寻址缓存取(T5 的 FETCH/bake 再把它写进发行包),
		// 产物计数保持 4 个/着色器(契约由 tests/World/ShaderPipelineTests.cpp 冻结)。
		static BakeResult BakeDirectory(const std::filesystem::path& sourceDir,
			const std::filesystem::path& outputDir);

	private:
		// 目标后端的 SPIR-V(内容寻址缓存;未命中时 slangc 现场编译)。
		static bool EnsureModule(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
			const std::string& profile, bool glTarget, std::string& outPath, std::string& error);
		// 过渡期 GLSL 兜底:Vulkan 目标 SPIR-V → spirv-cross → GLSL(同一缓存键)。
		static bool EnsureGlslFallback(const std::filesystem::path& sourceAbs, const std::string& entryPoint,
			const std::string& profile, std::string& outPath, std::string& error);
		// GL 目标 SPIR-V 的解析(烘焙产物 → 缓存),失败时给出人类可读原因。
		static bool TryLoadGlSpirVStage(const std::string& hlslPath, const std::string& entryPoint,
			const std::string& profile, std::vector<uint8_t>& out, std::string& reason);
		// T2 起引擎着色器不再走 dxc;保留到 T6 与工具链一并删除(过渡期定点回退用)。
		static bool CompileToSpvWithDxc(const std::string& hlslAbsPath, const std::string& entryPoint,
			const std::string& profile, const std::string& spvAbsPath);
		static bool CrossCompileToGlsl(const std::string& spvAbsPath, const std::string& glslAbsPath);

		static bool WriteBinaryFile(const std::string& filename, const std::vector<char>& data);

		// 从文件中读取二进制数据
		static std::vector<char> ReadBinaryFile(const std::string& filename);

		static const std::string dxcAbsPath;
		static const std::string spirvCrossAbsPath;
		static const std::string cacheDirAbsPath;
	};
}
