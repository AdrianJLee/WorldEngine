#include "ShaderUtils.h"

#include "World/Renderer/Renderer.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace World
{

	const std::string ShaderCompiler::dxcAbsPath = WLD_ROOT_DIR + std::string("vendor/dxc/dxc.exe"); // 定位到你的 dxc
	const std::string ShaderCompiler::spirvCrossAbsPath = WLD_ROOT_DIR + std::string("vendor/SPIRV-Cross/spirv-cross.exe"); // 定位到你的 spirv-cross
	const std::string ShaderCompiler::outPutAbsPath = WLD_INTERMEDIATE_DIR + std::string("World/");

	std::vector<char> ShaderCompiler::CompileOrLoad(const std::string& hlslRelativePath, const std::string& entryPoint, const std::string& profile)
	{
		// 使用 std::filesystem::path 处理和拼接路径
		std::filesystem::path relativePath(hlslRelativePath);
		std::filesystem::path sourceAbsPath = std::filesystem::absolute(std::filesystem::path(WLD_WORLD_DIR) / relativePath);

		// 获取文件名并去掉后缀 (stem)
		std::string shaderStem = relativePath.stem().string();

		// 拼接出输出目录
		std::filesystem::path shaderOutputDir = std::filesystem::absolute(std::filesystem::path(outPutAbsPath) / relativePath.parent_path());

		// 生成相应文件的绝对路径
		std::filesystem::path hlslAbsPath = shaderOutputDir / (shaderStem + ".hlsl");
		std::filesystem::path spvAbsPath = shaderOutputDir / (shaderStem + entryPoint + ".spv");
		std::filesystem::path glslAbsPath = shaderOutputDir / (shaderStem + entryPoint + ".glsl");

		{
			// 确保中间目录存在
			std::filesystem::create_directories(shaderOutputDir);
		}

		// 注意将 std::filesystem::path 转为 std::string 以兼容内部已有的接受 string 类型的函数
		if (ShouldCompile(sourceAbsPath.string(), hlslAbsPath.string()))
		{
			std::filesystem::copy_file(sourceAbsPath, hlslAbsPath, std::filesystem::copy_options::update_existing);
		}

		if (ShouldCompile(sourceAbsPath.string(), spvAbsPath.string()))
		{
			CompileToSpv(sourceAbsPath.string(), entryPoint, profile, spvAbsPath.string());
		}

		if (ShouldCompile(spvAbsPath.string(), glslAbsPath.string()))
		{
			CrossCompileToGlsl(spvAbsPath.string(), glslAbsPath.string());
		}

		switch (World::Renderer::GetAPI())
		{
			case RendererAPI::API::None:
				WLD_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
				return {};
			case RendererAPI::API::OpenGL:
				return ReadBinaryFile(glslAbsPath.string());
			case RendererAPI::API::Vulkan:
				return ReadBinaryFile(spvAbsPath.string());
			default:
				WLD_CORE_ASSERT(false, "Unknown RendererAPI!");
				return {};
		}
	}

	bool ShaderCompiler::CompileToSpv(const std::string& hlslAbsPath, const std::string& entryPoint, const std::string& profile, const std::string& spvAbsPath)
	{
		//dxc.exe -spirv -T vs_6_0 -E VS Test.vert.hlsl -Fo Test.3.vert.spv
		std::string cmd = "\"\"" + dxcAbsPath + "\"" +
			" -spirv " +
			" -T " + profile +
			" -E " + entryPoint +
			" \"" + hlslAbsPath + "\"" +
			" -Fo \"" + spvAbsPath + "\"\"";

		int result = std::system(cmd.c_str());
		if (result != 0)
		{
			WLD_CORE_ASSERT(false, "Shader compilation failed!");
		}
		return result == 0;
	}

	bool ShaderCompiler::CrossCompileToGlsl(const std::string& spvAbsPath, const std::string& glslAbsPath)
	{
		std::string cmd = "\"\"" + spirvCrossAbsPath + "\"" +
			" --version 450 " +
			" --output \"" + glslAbsPath + "\" " +
			" \"" + spvAbsPath + "\"\"";
		int result = std::system(cmd.c_str());
		if (result != 0)
		{
			WLD_CORE_ASSERT(false, "SPIRV-Cross failed!");
		}
		return result == 0;
	}

	bool ShaderCompiler::ShouldCompile(const std::string& sourceFile, const std::string& targetFile)
	{
		if (!std::filesystem::exists(targetFile))
			return true;
		return std::filesystem::last_write_time(sourceFile) > std::filesystem::last_write_time(targetFile);
	}

	std::vector<char> ShaderCompiler::ReadBinaryFile(const std::string& filename)
	{
		// 1. 以二进制模式打开，并将指针移动到文件末尾 (ate)
		std::ifstream file(filename, std::ios::ate | std::ios::binary);

		// 2. 检查文件是否成功打开
		if (!file.is_open())
		{
			WLD_CORE_ERROR("Failed to open file: {0}", filename);
			return {}; // 必须立即返回空，防止后续 tellg 报错
		}

		// 3. 获取文件大小
		size_t fileSize = (size_t)file.tellg();

		// 4. 处理空文件情况
		if (fileSize == 0)
		{
			WLD_CORE_WARN("File is empty: {0}", filename);
			return {};
		}

		// 5. 分配内存并读取
		std::vector<char> buffer(fileSize);
		file.seekg(0); // 重新回到文件开头
		file.read(buffer.data(), fileSize);
		file.close();

		return buffer;
	}

}