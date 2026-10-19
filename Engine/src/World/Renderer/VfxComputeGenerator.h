#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/VfxModule.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace World
{
	struct WLD_API VfxComputeShaderPackage
	{
		std::string SlangSource;
		std::string EmitterName;
		uint64_t SourceHash = 0;
		bool HasCustomCode = false;
	};

	class WLD_API VfxComputeGenerator
	{
	public:
		// 依据发射器装配的模块堆栈与暴露参数，动态生成完整的统一 Slang Compute Shader 源码
		static std::string GenerateComputeShaderSource(
			const std::string& emitterName,
			const std::vector<VfxModule>& modules,
			const std::unordered_map<std::string, float>& userParamsFloat = {},
			const std::unordered_map<std::string, glm::vec4>& userParamsVec4 = {}
		);

		// 即时编译与语法诊断验证 (开发期热重载与编辑器语法检查)
		static bool ValidateAndCompile(const std::string& slangSource, std::string* outError = nullptr);
	};
}
