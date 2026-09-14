#pragma once
#include "World/Core/Export.h"
#include "World/RHI/RhiShader.h"
#include <string>
#include <vector>

namespace World
{
	class ShaderCompiler
	{
	public:
		// 编译或加载指定的 HLSL 文件，返回对应平台的字节码（SPV 或 GLSL）
		static std::vector<char> CompileOrLoad(const std::string& hlslPath, const std::string& entryPoint, const std::string& profile);
		// 按当前后端填充规范的 RHI 着色器阶段(Vulkan 填 SPIR-V,OpenGL 填 GLSL)。
		static Rhi::ShaderStageSource CompileStage(Rhi::ShaderStage stage, const std::string& hlslPath,
			const std::string& entryPoint, const std::string& profile);

	private:
		static bool CompileToSpv(const std::string& hlslAbsPath, const std::string& entryPoint, const std::string& profile, const std::string& spvAbsPath);
		static bool CrossCompileToGlsl(const std::string& spvAbsPath, const std::string& glslAbsPath);

		// 判断是否需要重新编译：如果目标文件不存在，或者源文件比目标文件新，则需要编译
		static bool ShouldCompile(const std::string& sourceFile, const std::string& targetFile);

		// 从文件中读取二进制数据
		static std::vector<char> ReadBinaryFile(const std::string& filename);

		static WLD_API const std::string dxcAbsPath;
		static WLD_API const std::string spirvCrossAbsPath;
		static WLD_API const std::string outPutAbsPath;
	};
}
