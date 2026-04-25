#pragma once
#include "World/Renderer/Shader.h"
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"

#include <map>
namespace World
{
	class VulkanShader :public Shader
	{
	public:
		VulkanShader(Ref<VulkanDevice> device);

		virtual ~VulkanShader();
		virtual void AddShader(const std::string& path, ShaderType type) override;
		virtual void Compile()override;
		virtual const std::string& GetName() const override { return m_Name; }
		virtual void Bind() const override { WLD_CORE_WARN("VulkanShader::Bind() is not implemented yet"); };
		virtual void Unbind() const override { WLD_CORE_WARN("VulkanShader::Unbind() is not implemented yet"); };

		const std::vector<VkPipelineShaderStageCreateInfo>& GetShaderStages() const { return m_ShaderStages; }
	private:
		VkShaderModule CreateShaderModule(const std::vector<char>& code);
		std::string GetEntryPointForType(ShaderType type);
		std::string GetProfileForType(ShaderType type);

	private:
		Ref<VulkanDevice> m_Device;
		std::string m_Name;

		// Save the file paths for each shader type, so we can compile them later in Compile()
		std::map<ShaderType, std::string> m_ShaderFilePaths;

		// Save the created shader modules and their corresponding stage info for pipeline creation
		std::vector<VkShaderModule> m_ShaderModules;
		std::vector<VkPipelineShaderStageCreateInfo> m_ShaderStages;
	};
}