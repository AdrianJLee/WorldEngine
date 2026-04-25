#include "wldpch.h"
#include "VulkanShader.h"
#include "World/Renderer/ShaderUtils.h"

namespace World
{
	VulkanShader::VulkanShader(Ref<VulkanDevice> device)
		: m_Device(device)
	{

	}
	VulkanShader::~VulkanShader()
	{
		WLD_PROFILE_FUNCTION();
		// Clean up shader modules
		for (auto shaderModule : m_ShaderModules)
		{
			vkDestroyShaderModule(m_Device->GetLogicalDevice(), shaderModule, nullptr);
		}
		m_ShaderModules.clear();
		m_ShaderStages.clear();
	}
	void VulkanShader::AddShader(const std::string& path, ShaderType type)
	{
		m_Name = path;
		m_ShaderFilePaths[type] = path;
	}
	void VulkanShader::Compile()
	{
		WLD_PROFILE_FUNCTION();

		// For each shader type and its corresponding file path, we need to compile or load the shader, create a Vulkan shader module, and prepare the stage info for pipeline creation.
		for (const auto& [type, filepath] : m_ShaderFilePaths)
		{
			std::string entryPoint = GetEntryPointForType(type);
			std::string profile = GetProfileForType(type);

			// Load or compile the shader to get the SPIR-V bytecode
			std::vector<char> spvData = ShaderCompiler::CompileOrLoad(filepath, entryPoint, profile);
			WLD_CORE_ASSERT(!spvData.empty(), "Failed to get SPIR-V data for shader");

			VkShaderModule shaderModule = CreateShaderModule(spvData);
			m_ShaderModules.push_back(shaderModule);

			// Prepare the shader stage info for pipeline creation
			VkPipelineShaderStageCreateInfo stageInfo {};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

			if (type == ShaderType::Vertex)
				stageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
			else if (type == ShaderType::Fragment)
				stageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;

			stageInfo.module = shaderModule;
			// It is always "main" in SPIR-V
			stageInfo.pName = "main";

			m_ShaderStages.push_back(stageInfo);
		}
	}
	VkShaderModule VulkanShader::CreateShaderModule(const std::vector<char>& code)
	{
		WLD_PROFILE_FUNCTION();
		VkShaderModuleCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = code.size();
		createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(m_Device->GetLogicalDevice(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create Vulkan shader module!");
		}

		return shaderModule;
	}
	std::string VulkanShader::GetEntryPointForType(ShaderType type)
	{
		WLD_PROFILE_FUNCTION();
		switch (type)
		{
			case World::Shader::ShaderType::Vertex:
				return "VSMain";
			case World::Shader::ShaderType::Fragment:
				return "PSMain";
			default:
				WLD_CORE_ASSERT(false, "Unsupported shader type!");
				return "";
		}
	}
	std::string VulkanShader::GetProfileForType(ShaderType type)
	{
		WLD_PROFILE_FUNCTION();
		switch (type)
		{
			case ShaderType::Vertex:
				return "vs_6_0";
			case ShaderType::Fragment:
				return "ps_6_0";
			default:
				WLD_CORE_ASSERT(false, "Unknown shader type!");
				return "";
		}
	}
}