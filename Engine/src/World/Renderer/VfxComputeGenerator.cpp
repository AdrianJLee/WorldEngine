#include "wldpch.h"
#include "World/Renderer/VfxComputeGenerator.h"
#include "World/Renderer/ShaderUtils.h"

#include <sstream>
#include <iomanip>
#include <set>

namespace World
{
	std::string VfxComputeGenerator::GenerateComputeShaderSource(
		const std::string& emitterName,
		const std::vector<VfxModule>& modules,
		const std::unordered_map<std::string, float>& userParamsFloat,
		const std::unordered_map<std::string, glm::vec4>& userParamsVec4)
	{
		std::ostringstream ss;

		ss << "// ==============================================================================\n";
		ss << "// WorldEngine 2.0 - Generated Slang VFX Compute Shader\n";
		ss << "// Emitter: " << (emitterName.empty() ? "MainEmitter" : emitterName) << "\n";
		ss << "// Target: SPIR-V 1.3+ / Vulkan 1.3 & OpenGL 4.6 Core\n";
		ss << "// ==============================================================================\n\n";

		// 1. 结构体定义 (64 字节对齐紧凑布局)
		ss << "struct GpuParticle\n";
		ss << "{\n";
		ss << "    float3 Position;\n";
		ss << "    float  Life;\n";
		ss << "    float3 Velocity;\n";
		ss << "    float  MaxLife;\n";
		ss << "    float4 Color;\n";
		ss << "    float2 Size;\n";
		ss << "    float  Rotation;\n";
		ss << "    float  AngularVelocity;\n";
		ss << "    uint   SubUVFrame;\n";
		ss << "    uint   Flags;\n";
		ss << "    uint   EmitterIndex;\n";
		ss << "    uint   Padding;\n";
		ss << "};\n\n";

		// 2. 资源描述符绑定
		ss << "[[vk::binding(0, 0)]] RWStructuredBuffer<GpuParticle> InParticles  : register(u0);\n";
		ss << "[[vk::binding(1, 0)]] RWStructuredBuffer<GpuParticle> OutParticles : register(u1);\n";
		ss << "[[vk::binding(2, 0)]] RWStructuredBuffer<uint>        AliveIndices : register(u2);\n";
		ss << "[[vk::binding(3, 0)]] RWStructuredBuffer<uint>        IndirectArgs : register(u3);\n\n";

		// 3. 动态聚合 Uniform 常量缓冲 (收集所有模块声明的参数与系统暴露变量)
		ss << "[[vk::binding(4, 0)]] cbuffer SimulationUniforms : register(b0)\n";
		ss << "{\n";
		ss << "    float  dt;\n";
		ss << "    float  u_GlobalTime;\n";
		ss << "    uint   u_MaxParticles;\n";
		ss << "    float  u_Padding0;\n";

		std::set<std::string> declaredUniforms;

		for (const auto& mod : modules)
		{
			for (const auto& p : mod.Parameters)
			{
				const std::string uName = p.GetUniformName();
				if (declaredUniforms.insert(uName).second)
				{
					if (p.Type == VfxParamType::Float)
					{
						ss << "    float  " << uName << ";\n";
					}
					else if (p.Type == VfxParamType::Vec2)
					{
						ss << "    float2 " << uName << ";\n";
					}
					else if (p.Type == VfxParamType::Vec3)
					{
						ss << "    float4 " << uName << "; // float3 packed with 16B alignment\n";
					}
					else if (p.Type == VfxParamType::Vec4)
					{
						ss << "    float4 " << uName << ";\n";
					}
					else if (p.Type == VfxParamType::Int)
					{
						ss << "    int    " << uName << ";\n";
					}
					else if (p.Type == VfxParamType::Bool)
					{
						ss << "    uint   " << uName << ";\n";
					}
				}
			}
		}

		for (const auto& [name, val] : userParamsFloat)
		{
			const std::string uName = "u_" + name;
			if (declaredUniforms.insert(uName).second)
			{
				ss << "    float  " << uName << ";\n";
			}
		}

		for (const auto& [name, val] : userParamsVec4)
		{
			const std::string uName = "u_" + name;
			if (declaredUniforms.insert(uName).second)
			{
				ss << "    float4 " << uName << ";\n";
			}
		}

		ss << "};\n\n";

		// 4. 数学与噪波辅助工具库 (3D Curl Noise, PRNG Hash)
		ss << R"(
float Hash1D(uint n)
{
    n = (n << 13U) ^ n;
    n = n * (n * n * 15731U + 789221U) + 1376312589U;
    return float(n & 0x7fffffffU) / float(0x7fffffff);
}

float3 Hash3D(uint n)
{
    return float3(Hash1D(n * 3u + 0u), Hash1D(n * 3u + 1u), Hash1D(n * 3u + 2u));
}

float3 Noise3D(float3 p)
{
    float3 s = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float n = s.x + s.y * 157.0f + 113.0f * s.z;
    return lerp(
        lerp(lerp(Hash3D(uint(n + 0.0f)),   Hash3D(uint(n + 1.0f)),   f.x),
             lerp(Hash3D(uint(n + 157.0f)), Hash3D(uint(n + 158.0f)), f.x), f.y),
        lerp(lerp(Hash3D(uint(n + 113.0f)), Hash3D(uint(n + 114.0f)), f.x),
             lerp(Hash3D(uint(n + 270.0f)), Hash3D(uint(n + 271.0f)), f.x), f.y), f.z
    ) * 2.0f - 1.0f;
}

float3 CurlNoise3D(float3 p)
{
    const float eps = 0.1f;
    float3 dx = float3(eps, 0.0f, 0.0f);
    float3 dy = float3(0.0f, eps, 0.0f);
    float3 dz = float3(0.0f, 0.0f, eps);

    float3 n1 = Noise3D(p + dy);
    float3 n2 = Noise3D(p - dy);
    float3 n3 = Noise3D(p + dz);
    float3 n4 = Noise3D(p - dz);
    float3 n5 = Noise3D(p + dx);
    float3 n6 = Noise3D(p - dx);

    float x = (n1.z - n2.z) - (n3.y - n4.y);
    float y = (n3.x - n4.x) - (n5.z - n6.z);
    float z = (n5.y - n6.y) - (n1.x - n2.x);
    return normalize(float3(x, y, z) / (2.0f * eps));
}
)";
		ss << "\n";

		// 5. 阶段 A: 生成内核 (Spawn Kernel)
		ss << "// ------------------------------------------------------------------------------\n";
		ss << "// 阶段 A: 粒子生成计算内核 (Particle Spawn Compute Stage)\n";
		ss << "// ------------------------------------------------------------------------------\n";
		ss << "[numthreads(256, 1, 1)]\n";
		ss << "void CS_Spawn(uint3 dispatchThreadId : SV_DispatchThreadID)\n";
		ss << "{\n";
		ss << "    uint particleId = dispatchThreadId.x;\n";
		ss << "    if (particleId >= u_MaxParticles) return;\n\n";
		ss << "    GpuParticle p = InParticles[particleId];\n";
		ss << "    p.Life = 2.0f;\n";
		ss << "    p.MaxLife = 2.0f;\n";
		ss << "    p.Color = float4(1.0f);\n";
		ss << "    p.Size = float2(0.3f, 0.3f);\n";
		ss << "    p.Flags = 1u;\n\n";

		// 内联展开所有启用的 Spawn 模块源码
		for (const auto& mod : modules)
		{
			if (mod.Stage == VfxModuleStage::Spawn && mod.Enabled && !mod.SlangSource.empty())
			{
				ss << "    // ---- 模块: " << mod.Name << " ----\n";
				ss << mod.SlangSource << "\n";
			}
		}

		ss << "\n    OutParticles[particleId] = p;\n";
		ss << "}\n\n";

		// 6. 阶段 B: 动力学更新内核 (Update Kernel)
		ss << "// ------------------------------------------------------------------------------\n";
		ss << "// 阶段 B: 粒子物理动力学模拟内核 (Particle Update Simulation Stage)\n";
		ss << "// ------------------------------------------------------------------------------\n";
		ss << "[numthreads(256, 1, 1)]\n";
		ss << "void CS_Update(uint3 dispatchThreadId : SV_DispatchThreadID)\n";
		ss << "{\n";
		ss << "    uint particleId = dispatchThreadId.x;\n";
		ss << "    if (particleId >= u_MaxParticles) return;\n\n";
		ss << "    GpuParticle p = InParticles[particleId];\n";
		ss << "    if (p.Life <= 0.0f) return;\n\n";

		// 内联展开所有启用的 Update 模块源码
		for (const auto& mod : modules)
		{
			if (mod.Stage == VfxModuleStage::Update && mod.Enabled && !mod.SlangSource.empty())
			{
				ss << "    // ---- 模块: " << mod.Name << " ----\n";
				ss << mod.SlangSource << "\n";
			}
		}

		ss << "\n    OutParticles[particleId] = p;\n";
		ss << "}\n";

		return ss.str();
	}

	bool VfxComputeGenerator::ValidateAndCompile(const std::string& slangSource, std::string* outError)
	{
		if (slangSource.empty())
		{
			if (outError) *outError = "Slang source is empty.";
			return false;
		}

		// 开发期语法与语义校验
		// 通过临时文件调用底层 TryCompileOrLoad 或静态语法检查
		std::error_code ec;
		std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
		if (ec) tempDir = ".";

		const std::filesystem::path tempFile = tempDir / "worldengine_vfx_preview.slang";
		{
			std::ofstream ofs(tempFile, std::ios::binary);
			if (!ofs.is_open())
			{
				if (outError) *outError = "Cannot create temporary file for Slang validation.";
				return false;
			}
			ofs.write(slangSource.data(), slangSource.size());
		}

		std::vector<char> bytecode;
		std::string err;
		const bool ok = ShaderCompiler::TryCompileOrLoad(tempFile.string(), "CS_Update", "cs_6_0", bytecode, &err);
		
		std::filesystem::remove(tempFile, ec);

		if (!ok)
		{
			if (outError) *outError = err;
			return false;
		}

		return true;
	}
}
