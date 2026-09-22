#include "MaterialSurface.h"

#include "World/Core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace World
{
	namespace
	{
		namespace fs = std::filesystem;

		// 2 = M4-S2:包装源码加入注解参数块,并且每次编译都落一份 SPIR-V 汇编(-Fc)供反射。
		constexpr uint32_t kSurfaceCacheVersion = 2;
		constexpr const char* kSurfaceEntryPoint = "PSMain";
		constexpr const char* kUserSourceFileName = "surface_user.hlsl";
		constexpr const char* kAssemblyFileName = "surface.asm";

		std::atomic<size_t> s_CacheHits { 0 };
		std::atomic<size_t> s_CacheMisses { 0 };
		std::atomic<size_t> s_ToolInvocations { 0 };

		std::mutex s_CompileMutex;
		std::mutex s_LastGoodMutex;
		std::unordered_map<std::string, SurfaceArtifact> s_LastGood;

		uint64_t Fnv1a64(const void* data, size_t size)
		{
			uint64_t hash = 14695981039346656037ULL;
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		uint64_t Fnv1a64String(const std::string& text)
		{
			return Fnv1a64(text.data(), text.size());
		}

		std::string Hex(uint64_t value)
		{
			char buffer[24];
			std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
			return buffer;
		}

		uint64_t Mix(uint64_t hash, const std::string& text)
		{
			hash ^= Fnv1a64String(text);
			hash *= 1099511628211ULL;
			return hash;
		}

		std::string Trim(const std::string& text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return text.substr(begin, end - begin + 1);
		}

		std::string SanitizeComment(const std::string& text)
		{
			std::string out = text;
			for (char& ch : out)
			{
				if (ch == '\r' || ch == '\n')
					ch = ' ';
			}
			return out;
		}

		std::string NormalizeSlashes(std::string text)
		{
			for (char& ch : text)
			{
				if (ch == '\\')
					ch = '/';
			}
			return text;
		}

		std::string LowerAscii(std::string text)
		{
			for (char& ch : text)
			{
				if (ch >= 'A' && ch <= 'Z')
					ch = static_cast<char>(ch - 'A' + 'a');
			}
			return text;
		}

		bool EndsWith(const std::string& text, const std::string& suffix)
		{
			return text.size() >= suffix.size() &&
				text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		bool ReadAllText(const fs::path& path, std::string& out)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return false;
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			out = buffer.str();
			return true;
		}

		bool ReadAllBytes(const fs::path& path, std::vector<uint8_t>& out)
		{
			std::ifstream stream(path, std::ios::ate | std::ios::binary);
			if (!stream)
				return false;
			const std::streamsize size = stream.tellg();
			if (size <= 0)
				return false;
			out.resize(static_cast<size_t>(size));
			stream.seekg(0);
			stream.read(reinterpret_cast<char*>(out.data()), size);
			return static_cast<bool>(stream);
		}

		bool WriteAllText(const fs::path& path, const std::string& text)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;
			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			return static_cast<bool>(stream);
		}

		// 工具身份:可执行文件大小做代理(与 ShaderCompiler 同一口径);dxc 与 dxcompiler.dll
		// 一起参与,避免只换 DLL 时命中旧缓存。
		uint64_t ToolIdentity(const std::string& toolPath)
		{
			std::error_code ec;
			const uint64_t size = static_cast<uint64_t>(fs::file_size(toolPath, ec));
			return ec ? 0 : size;
		}

		std::string BackendKey(SurfaceShaderBackend backend)
		{
			switch (backend)
			{
				case SurfaceShaderBackend::VulkanSpirV: return "vulkan-spirv";
				case SurfaceShaderBackend::OpenGLGlsl: return "opengl-glsl";
			}
			return "unknown";
		}

		std::string LastGoodKey(const std::string& permutationKey, SurfaceShaderBackend backend)
		{
			return BackendKey(backend) + "\n" + permutationKey;
		}

		void StoreLastGood(const std::string& permutationKey, SurfaceShaderBackend backend,
			const SurfaceArtifact& artifact)
		{
			std::lock_guard<std::mutex> lock(s_LastGoodMutex);
			s_LastGood[LastGoodKey(permutationKey, backend)] = artifact;
		}

#ifdef _WIN32
		bool RunToolCapture(const std::string& exe, const std::string& arguments, const std::string& logPath,
			int& exitCode, std::string& output)
		{
			SECURITY_ATTRIBUTES attributes {};
			attributes.nLength = sizeof(attributes);
			attributes.bInheritHandle = TRUE;

			HANDLE logHandle = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (logHandle == INVALID_HANDLE_VALUE)
			{
				exitCode = -1;
				return false;
			}
			HANDLE nulHandle = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				&attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

			STARTUPINFOA startup {};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdOutput = logHandle;
			startup.hStdError = logHandle;
			startup.hStdInput = (nulHandle == INVALID_HANDLE_VALUE) ? GetStdHandle(STD_INPUT_HANDLE) : nulHandle;

			PROCESS_INFORMATION process {};
			std::string commandLine = "\"" + exe + "\" " + arguments;
			const BOOL launched = CreateProcessA(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
				0, nullptr, nullptr, &startup, &process);
			if (nulHandle != INVALID_HANDLE_VALUE)
				CloseHandle(nulHandle);
			if (!launched)
			{
				CloseHandle(logHandle);
				exitCode = -1;
				return false;
			}

			WaitForSingleObject(process.hProcess, INFINITE);
			DWORD code = 0;
			GetExitCodeProcess(process.hProcess, &code);
			CloseHandle(process.hProcess);
			CloseHandle(process.hThread);
			CloseHandle(logHandle);

			exitCode = static_cast<int>(code);
			return ReadAllText(fs::path(logPath), output);
		}
#else
		bool RunToolCapture(const std::string& exe, const std::string& arguments, const std::string& logPath,
			int& exitCode, std::string& output)
		{
			const std::string commandLine = "\"" + exe + "\" " + arguments + " > \"" + logPath + "\" 2>&1";
			exitCode = std::system(commandLine.c_str());
			return ReadAllText(fs::path(logPath), output);
		}
#endif

		std::vector<SurfaceDiagnostic> ParseDiagnostics(const std::string& toolOutput)
		{
			std::vector<SurfaceDiagnostic> diagnostics;
			const std::regex parenthesized(
				R"(^\s*(.+?)\((\d+),(\d+)\)\s*:\s*(error|warning)\s*:?\s*(.*)$)",
				std::regex::ECMAScript);
			const std::regex colonized(
				R"(^\s*(.+?):(\d+):(\d+)\s*:\s*(error|warning)\s*:?\s*(.*)$)",
				std::regex::ECMAScript);

			const std::string userFile = LowerAscii(NormalizeSlashes(kUserSourceFileName));
			std::istringstream stream(toolOutput);
			std::string line;
			while (std::getline(stream, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (Trim(line).empty())
					continue;

				SurfaceDiagnostic diagnostic;
				std::smatch match;
				if (std::regex_match(line, match, parenthesized) ||
					std::regex_match(line, match, colonized))
				{
					diagnostic.Severity = match[4].str();
					diagnostic.Message = line;
					diagnostic.File = match[1].str();
					diagnostic.Line = static_cast<uint32_t>(std::stoul(match[2].str()));
					diagnostic.Column = static_cast<uint32_t>(std::stoul(match[3].str()));

					const std::string reported = LowerAscii(NormalizeSlashes(diagnostic.File));
					if (reported == userFile || EndsWith(reported, "/" + userFile))
					{
						diagnostic.InUserSource = true;
						diagnostic.UserLine = diagnostic.Line;
						diagnostic.UserColumn = diagnostic.Column;
					}
				}
				else
				{
					diagnostic.Message = line;
					diagnostic.Severity = (line.find("error") != std::string::npos) ? "error" : "";
				}
				diagnostics.push_back(std::move(diagnostic));
			}
			return diagnostics;
		}

		bool HasErrorDiagnostic(const std::vector<SurfaceDiagnostic>& diagnostics)
		{
			for (const SurfaceDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Severity == "error")
					return true;
			}
			return false;
		}

		std::string BuildDefaultSurfaceFunction()
		{
			std::string text;
			text += "// Engine standard surface: every Surface field gets a default assignment.\n";
			text += "// User code only changes the fields it cares about.\n";
			text += "Surface MakeDefaultSurface()\n{\n";
			text += "    Surface surface;\n";
			for (const MaterialSurfaceContract::FieldInfo& field : MaterialSurfaceContract::SurfaceFields)
			{
				text += "    surface.";
				text += field.Name;
				text += " = ";
				text += field.DefaultExpression;
				text += ";\n";
			}
			text += "    return surface;\n}\n";
			return text;
		}

		// 参数类型 → 参数块里的 HLSL 类型。注意 HLSL 的 bool 在 SPIR-V 里是 uint32
		// (反射校验按 uint 认它,见 MaterialParams.cpp 的 ReflectedTypeMatches)。
		const char* ParamHlslType(ParamType type)
		{
			switch (type)
			{
				case ParamType::Float: return "float";
				case ParamType::Vec2: return "float2";
				case ParamType::Vec3: return "float3";
				case ParamType::Vec4: return "float4";
				case ParamType::Color: return "float4";
				case ParamType::Int: return "int";
				case ParamType::Bool: return "bool";
				case ParamType::Texture2D: return "Texture2D";
			}
			return "float";
		}

		// 注解 → 参数块源码。标量/向量进 `cbuffer MaterialParams`(register b2, space1),
		// 贴图按注解顺序占 space2 的 t4、t5…(每张同时声明配套 SamplerState)。
		// 布局(偏移/大小)不在这里手写:编译后用 dxc 的 SPIR-V 汇编反射出来。
		std::string BuildParamBlockText(const std::vector<MaterialParamDecl>& params)
		{
			std::string members;
			std::string textures;
			uint32_t textureIndex = 0;
			for (const MaterialParamDecl& param : params)
			{
				if (IsTextureParamType(param.Type))
				{
					const uint32_t binding = ParamTextureBaseBinding() + textureIndex;
					++textureIndex;
					textures += "[[vk::combinedImageSampler]] Texture2D ";
					textures += param.Name;
					textures += " : register(t" + std::to_string(binding) + ", space2);\n";
					textures += "[[vk::combinedImageSampler]] SamplerState ";
					textures += param.Name + "Sampler";
					textures += " : register(s" + std::to_string(binding) + ", space2);\n";
					continue;
				}
				members += "    ";
				members += ParamHlslType(param.Type);
				members += " ";
				members += param.Name;
				members += ";\n";
			}

			std::string text;
			text += "\n// ---- M4-S2 material parameters (generated from //! param annotations) ----\n";
			if (!members.empty())
			{
				text += "// 参数值由编辑器/运行时按反射出的偏移写入;这里只声明参数块。\n";
				text += "cbuffer ";
				text += ParamCbufferName();
				text += " : register(b";
				text += std::to_string(ParamCbufferBinding());
				text += ", space1)\n{\n";
				text += members;
				text += "};\n";
			}
			if (!textures.empty())
			{
				text += "// 贴图参数(采样器与贴图同名 + Sampler 后缀):\n";
				text += textures;
			}
			return text;
		}

		// 引擎模板:顶点/光照/阴影/实例化/蒙皮/雾钩子由引擎提供。
		// 注意:雾目前只有一个恒等钩子 —— 当前全局 UBO 里没有雾参数,不伪造接口;
		// S2/S3 接入雾 uniform 时替换 ApplyEngineFog() 即可。
		const char* kSurfaceTemplatePrefix = R"WESURFACE(
// ============================================================================
// Engine wrapper below: vertex stage / lighting / shadows / instancing /
// skinning are provided by WorldEngine. Only Evaluate() comes from the user.
// ============================================================================

struct SurfaceVSInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
};

struct SurfaceVSOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 WorldNormal : NORMAL;
    [[vk::location(1)]] float3 WorldPosition : POSITION1;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] nointerpolation float EntityId : ENTITYID;
};

struct SurfaceInstanceInput
{
    [[vk::location(3)]] float4 Row0 : INSTANCE0;
    [[vk::location(4)]] float4 Row1 : INSTANCE1;
    [[vk::location(5)]] float4 Row2 : INSTANCE2;
    [[vk::location(6)]] float4 Row3 : INSTANCE3;
    [[vk::location(7)]] float4 Color : INSTANCE4;
    [[vk::location(8)]] float4 EntityId : INSTANCE5;
};

struct SurfaceSkinnedInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal : NORMAL;
    [[vk::location(2)]] float2 UV : TEXCOORD0;
    [[vk::location(3)]] float4 Joints : JOINTS;
    [[vk::location(4)]] float4 Weights : WEIGHTS;
};

struct SurfacePSOutput
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

cbuffer CameraUniforms : register(b0)
{
    float4x4 u_ViewProjection;
};

struct GpuLight
{
    float4 PositionType;
    float4 ColorIntensity;
    float4 DirectionRange;
};

cbuffer LightUniforms : register(b2)
{
    float4x4 u_ShadowViewProjection;
    float4 u_ShadowParams;
    float4 u_Ambient;
    uint4 u_LightCounts;
    GpuLight u_Lights[8];
};

[[vk::combinedImageSampler]] Texture2D u_ShadowMap : register(t3, space0);
[[vk::combinedImageSampler]] SamplerState u_ShadowSampler : register(s3, space0);

cbuffer ObjectUniforms : register(b1, space1)
{
    float4x4 u_Model;
    float4 u_BaseColor;
    float4 u_MetallicRoughness;
    float4 u_Emissive;
    float4 u_Flags;
    int4 u_EntityId;
};

cbuffer BoneUniforms : register(b3, space1)
{
    float4x4 u_Bones[128];
};

[[vk::combinedImageSampler]] Texture2D u_AlbedoTexture : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState u_AlbedoSampler : register(s1, space2);
[[vk::combinedImageSampler]] Texture2D u_NormalTexture : register(t2, space2);
[[vk::combinedImageSampler]] SamplerState u_NormalSampler : register(s2, space2);

float3 WeLinearizeColor(float3 srgb)
{
    return pow(saturate(srgb), 2.2f);
}

float3 WeDefaultNormal()
{
    return float3(0.0f, 0.0f, 1.0f);
}

float4x4 ComputeSkinPalette(SurfaceSkinnedInput input)
{
    const int4 joints = (int4)round(input.Joints);
    float4x4 blended = (float4x4)0;
    [unroll] for (int index = 0; index < 4; ++index)
    {
        const uint boneIndex = (uint)clamp(joints[index], 0, 127);
        blended += u_Bones[boneIndex] * input.Weights[index];
    }
    return blended;
}

float SampleDirectionalShadow(float3 worldPosition, float3 normal, float3 toLightDirection)
{
    const float4 shadowPosition = mul(u_ShadowViewProjection, float4(worldPosition, 1.0f));
    if (shadowPosition.w <= 0.0f)
        return 1.0f;
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float depth = (u_LightCounts.z > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float2 uv = projected.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;
    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float bias = max(u_ShadowParams.y * (1.0f - cosTheta), u_ShadowParams.y * 0.25f);
    const float2 texel = u_ShadowParams.w / max(u_ShadowParams.z, 1.0f);
    float visible = 0.0f;
    [unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            const float sampledDepth = u_ShadowMap.Sample(u_ShadowSampler,
                uv + float2(offsetX, offsetY) * texel).r;
            visible += (depth - bias <= sampledDepth) ? 1.0f : 0.0f;
        }
    }
    return visible / 9.0f;
}

SurfaceVSOutput BuildVSOutput(float4 worldPosition, float3 worldNormal, float2 uv, float entityId)
{
    SurfaceVSOutput output;
    output.Position = mul(u_ViewProjection, worldPosition);
    output.WorldNormal = worldNormal;
    output.WorldPosition = worldPosition.xyz;
    output.UV = uv;
    output.EntityId = entityId;
    return output;
}

SurfaceVSOutput VSMain(SurfaceVSInput input)
{
    const float4 worldPosition = mul(u_Model, float4(input.Position, 1.0f));
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)u_Model);
    return BuildVSOutput(worldPosition, mul(normalMatrix, input.Normal), input.UV,
        (float)u_EntityId.x);
}

SurfaceVSOutput VSMainInstanced(SurfaceVSInput input, SurfaceInstanceInput instance)
{
    const float4x4 model = float4x4(instance.Row0, instance.Row1, instance.Row2, instance.Row3);
    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)model);
    return BuildVSOutput(worldPosition, mul(normalMatrix, input.Normal), input.UV,
        instance.EntityId.x);
}

SurfaceVSOutput VSMainSkinned(SurfaceSkinnedInput input)
{
    const float4x4 palette = ComputeSkinPalette(input);
    const float4 skinnedPosition = mul(palette, float4(input.Position, 1.0f));
    const float3 skinnedNormal = mul((float3x3)palette, input.Normal);
    const bool hasWeights = any(input.Weights != 0.0f) && all(isfinite(skinnedPosition));
    const float4 localPosition = hasWeights ? skinnedPosition : float4(input.Position, 1.0f);
    const float3 localNormal = hasWeights ? skinnedNormal : input.Normal;
    const float4 worldPosition = mul(u_Model, localPosition);
    const float3x3 normalMatrix = (float3x3)transpose((float3x3)u_Model);
    return BuildVSOutput(worldPosition, mul(normalMatrix, localNormal), input.UV,
        (float)u_EntityId.x);
}

MaterialInputs BuildMaterialInputs(SurfaceVSOutput input)
{
    MaterialInputs materialInputs;
    materialInputs.UV = input.UV;
    materialInputs.WorldPosition = input.WorldPosition;
    materialInputs.WorldNormal = normalize(input.WorldNormal);
    const float3 dpdx = ddx(input.WorldPosition);
    const float3 dpdy = ddy(input.WorldPosition);
    const float2 duvdx = ddx(input.UV);
    const float2 duvdy = ddy(input.UV);
    float3 tangent = dpdx * duvdy.y - dpdy * duvdx.y;
    if (dot(tangent, tangent) < 1e-12f)
    {
        const float3 axis = abs(materialInputs.WorldNormal.y) < 0.99f
            ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        tangent = cross(axis, materialInputs.WorldNormal);
    }
    materialInputs.WorldTangent = normalize(tangent);
    const float3 bitangent = cross(materialInputs.WorldNormal, materialInputs.WorldTangent);
    materialInputs.WorldBitangent = dot(bitangent, bitangent) > 1e-12f
        ? normalize(bitangent) : float3(0.0f, 0.0f, 0.0f);
    return materialInputs;
}

float3 ResolveShadingNormal(SurfaceVSOutput input, MaterialInputs materialInputs, Surface surface)
{
    float3 tangentNormal = float3(0.0f, 0.0f, 1.0f);
    if (u_Flags.y > 0.5f)
        tangentNormal = u_NormalTexture.Sample(u_NormalSampler, input.UV).xyz * 2.0f - 1.0f;
    // Surface.Normal 是"在引擎采样结果之上的切空间扰动";默认 (0,0,1) 保持采样值。
    float3 combined = float3(tangentNormal.xy + surface.Normal.xy, surface.Normal.z);
    if (dot(combined, combined) < 1e-12f)
        combined = float3(0.0f, 0.0f, 1.0f);
    tangentNormal = normalize(combined);
    const float3 shading = materialInputs.WorldTangent * tangentNormal.x
        + materialInputs.WorldBitangent * tangentNormal.y
        + materialInputs.WorldNormal * tangentNormal.z;
    const float shadingLengthSquared = dot(shading, shading);
    return shadingLengthSquared > 1e-12f ? shading * rsqrt(shadingLengthSquared)
        : materialInputs.WorldNormal;
}

float3 EvaluateEngineLighting(MaterialInputs materialInputs, Surface surface, float3 shadingNormal)
{
    float3 albedo = max(surface.BaseColor, 0.0f);
    if (u_Flags.x > 0.5f)
        albedo *= u_AlbedoTexture.Sample(u_AlbedoSampler, materialInputs.UV).rgb;

    const float metallic = saturate(surface.Metallic);
    const float roughness = saturate(surface.Roughness);
    const float3 viewDirection = float3(0.0f, 0.0f, 1.0f);
    float3 litColor = albedo * u_Ambient.rgb * u_Ambient.a * saturate(surface.AmbientOcclusion);

    const uint lightCount = min(u_LightCounts.x + u_LightCounts.y, 8u);
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const GpuLight light = u_Lights[lightIndex];
        const bool isDirectional = light.PositionType.w > 0.5f;
        float3 toLight;
        float attenuation = 1.0f;
        if (isDirectional)
        {
            toLight = -normalize(light.DirectionRange.xyz);
        }
        else
        {
            const float3 offset = light.PositionType.xyz - materialInputs.WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            const float falloff = saturate(1.0f - distanceToLight / max(light.DirectionRange.x, 1e-4f));
            attenuation = falloff * falloff;
        }

        const float lambert = saturate(dot(shadingNormal, toLight));
        float visibility = 1.0f;
        if (isDirectional && u_ShadowParams.x > 0.5f)
            visibility = SampleDirectionalShadow(materialInputs.WorldPosition, shadingNormal, toLight);

        const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation * visibility;
        const float diffuse = lambert * lerp(1.0f, 0.35f, metallic);
        const float3 halfVector = normalize(toLight + viewDirection + 1e-5f);
        const float specularPower = lerp(8.0f, 128.0f, saturate(1.0f - roughness));
        const float specular = lambert > 0.0f
            ? pow(saturate(dot(shadingNormal, halfVector)), specularPower) * lerp(0.04f, 1.0f, metallic)
            : 0.0f;
        litColor += albedo * radiance * diffuse + radiance * specular;
    }

    litColor += max(surface.Emissive, 0.0f);
    return litColor;
}

// 雾钩子:当前全局 UBO 没有雾参数,引擎也没有雾通道,所以这里保持恒等。
// S2/S3 接入 u_FogParams/u_FogColor 后,只替换这一个函数即可。
float3 ApplyEngineFog(float3 color, MaterialInputs materialInputs)
{
    return color;
}
)WESURFACE";

		const char* kSurfaceTemplateSuffix = R"WESURFACE(
SurfacePSOutput PSMain(SurfaceVSOutput input)
{
    SurfacePSOutput output;
    const MaterialInputs materialInputs = BuildMaterialInputs(input);
    Surface surface = Evaluate(materialInputs);
    const float3 shadingNormal = ResolveShadingNormal(input, materialInputs, surface);
    float3 litColor = EvaluateEngineLighting(materialInputs, surface, shadingNormal);
    litColor = ApplyEngineFog(litColor, materialInputs);
    output.Color = float4(pow(saturate(litColor), 1.0f / 2.2f), saturate(surface.Opacity));
    output.EntityID = (int)round(input.EntityId);
    return output;
}
)WESURFACE";

		std::string BuildWrapperSource(const std::string& contractText, const std::string& userSource,
			const std::string& permutationKey, SurfaceShaderBackend backend,
			const std::vector<MaterialParamDecl>& params)
		{
			std::string wrapper;
			wrapper.reserve(contractText.size() + userSource.size() + 16384);
			wrapper += "// WorldEngine M4-S1 generated material surface wrapper. Do not edit.\n";
			wrapper += "// backend: ";
			wrapper += BackendKey(backend);
			wrapper += "\n// permutation: ";
			wrapper += SanitizeComment(permutationKey);
			wrapper += "\n\n// ---- MaterialSurfaceContract.hlsli (embedded) ----\n";
			wrapper += contractText;
			if (wrapper.empty() || wrapper.back() != '\n')
				wrapper += '\n';
			wrapper += kSurfaceTemplatePrefix;
			wrapper += "\n// ---- generated default surface (from the shared field table) ----\n";
			wrapper += BuildDefaultSurfaceFunction();
			wrapper += BuildParamBlockText(params);
			wrapper += "\n// ---- user surface function ----\n#include \"";
			wrapper += kUserSourceFileName;
			wrapper += "\"\n";
			wrapper += kSurfaceTemplateSuffix;
			return wrapper;
		}
	}

	const char* MaterialSurfaceCompiler::BackendName(SurfaceShaderBackend backend)
	{
		switch (backend)
		{
			case SurfaceShaderBackend::VulkanSpirV: return "vulkan-spirv";
			case SurfaceShaderBackend::OpenGLGlsl: return "opengl-glsl";
		}
		return "unknown";
	}

	std::string MaterialSurfaceCompiler::ContractHeaderPath()
	{
		return std::string(WLD_WORLD_DIR) + "src/World/Renderer/MaterialSurfaceContract.hlsli";
	}

	std::string MaterialSurfaceCompiler::DefaultSurfaceFunctionSource()
	{
		return
			"// Engine standard surface. Change only the fields this material needs.\n"
			"// MaterialInputs is filled by the engine; Surface defaults come from the\n"
			"// engine's standard shader and stay complete even if you change nothing.\n"
			"Surface Evaluate(MaterialInputs input)\n"
			"{\n"
			"    Surface surface = MakeDefaultSurface();\n"
			"    // Examples:\n"
			"    // surface.BaseColor = float3(1.0f, 0.35f, 0.2f);\n"
			"    // surface.Roughness = 0.25f;\n"
			"    // surface.Normal = float3(0.0f, 0.0f, 1.0f);\n"
			"    return surface;\n"
			"}\n";
	}

	std::string MaterialSurfaceCompiler::WrapSurfaceSource(const std::string& userSource,
		SurfaceShaderBackend backend)
	{
		// 注解坏了时只丢参数块(这里是诊断用的"看包装源码"入口),编译入口会返回结构化错误。
		std::vector<MaterialParamDecl> params;
		std::string parseError;
		if (!ParseMaterialParams(userSource, &params, &parseError))
			params.clear();
		return WrapSurfaceSourceWithParams(userSource, params, backend);
	}

	std::string MaterialSurfaceCompiler::WrapSurfaceSourceWithParams(const std::string& userSource,
		const std::vector<MaterialParamDecl>& params, SurfaceShaderBackend backend)
	{
		std::string contractText;
		if (!ReadAllText(fs::path(ContractHeaderPath()), contractText))
			return {};
		const std::string effectiveSource = Trim(userSource).empty()
			? DefaultSurfaceFunctionSource() : userSource;
		return BuildWrapperSource(contractText, effectiveSource, "", backend, params);
	}

	std::string MaterialSurfaceCompiler::BuildParamBlockSource(const std::vector<MaterialParamDecl>& params)
	{
		return BuildParamBlockText(params);
	}

	std::string MaterialSurfaceCompiler::AssemblyPath(const SurfaceArtifact& artifact)
	{
		if (artifact.CacheKey.empty())
			return {};
		const fs::path path = fs::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache"
			/ artifact.CacheKey / kAssemblyFileName;
		std::error_code ec;
		if (!fs::is_regular_file(path, ec))
			return {};
		const uintmax_t size = fs::file_size(path, ec);
		if (ec || size == 0)
			return {};
		return path.string();
	}

	size_t MaterialSurfaceCompiler::CacheHitCount()
	{
		return s_CacheHits.load(std::memory_order_relaxed);
	}

	size_t MaterialSurfaceCompiler::CacheMissCount()
	{
		return s_CacheMisses.load(std::memory_order_relaxed);
	}

	size_t MaterialSurfaceCompiler::ToolInvocationCount()
	{
		return s_ToolInvocations.load(std::memory_order_relaxed);
	}

	void MaterialSurfaceCompiler::ResetCounters()
	{
		s_CacheHits.store(0, std::memory_order_relaxed);
		s_CacheMisses.store(0, std::memory_order_relaxed);
		s_ToolInvocations.store(0, std::memory_order_relaxed);
	}

	void MaterialSurfaceCompiler::ClearLastGood()
	{
		std::lock_guard<std::mutex> lock(s_LastGoodMutex);
		s_LastGood.clear();
	}

	bool MaterialSurfaceCompiler::LastGood(const std::string& permutationKey, SurfaceShaderBackend backend,
		SurfaceArtifact& out)
	{
		std::lock_guard<std::mutex> lock(s_LastGoodMutex);
		const auto found = s_LastGood.find(LastGoodKey(permutationKey, backend));
		if (found == s_LastGood.end())
			return false;
		out = found->second;
		return true;
	}

	SurfaceCompileResult MaterialSurfaceCompiler::CompileSurface(const std::string& source,
		const std::string& permutationKey, SurfaceShaderBackend backend)
	{
		// M4-S2:注解是参数的事实源。解析失败 = 结构化诊断(带用户源行列号),不调用 dxc。
		std::vector<MaterialParamDecl> params;
		std::string parseError;
		if (!ParseMaterialParams(source, &params, &parseError))
		{
			SurfaceCompileResult result;
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = "material param annotation: " + parseError;
			diagnostic.File = kUserSourceFileName;
			diagnostic.InUserSource = true;
			// 解析错误串是 `<行>:<列>: <原因>`;行列拿给编辑器定位。
			const size_t firstColon = parseError.find(':');
			const size_t secondColon = firstColon == std::string::npos ? std::string::npos
				: parseError.find(':', firstColon + 1);
			if (firstColon != std::string::npos && secondColon != std::string::npos)
			{
				try
				{
					diagnostic.UserLine = static_cast<uint32_t>(std::stoul(parseError.substr(0, firstColon)));
					diagnostic.UserColumn = static_cast<uint32_t>(
						std::stoul(parseError.substr(firstColon + 1, secondColon - firstColon - 1)));
					diagnostic.Line = diagnostic.UserLine;
					diagnostic.Column = diagnostic.UserColumn;
				}
				catch (const std::exception&)
				{
					diagnostic.UserLine = 0;
					diagnostic.UserColumn = 0;
				}
			}
			result.Diagnostics.push_back(std::move(diagnostic));
			SurfaceArtifact ignoredLastGood;
			result.LastGoodAvailable = LastGood(permutationKey, backend, ignoredLastGood);
			return result;
		}
		return CompileSurfaceWithParams(source, params, permutationKey, backend);
	}

	SurfaceCompileResult MaterialSurfaceCompiler::CompileSurfaceWithParams(const std::string& source,
		const std::vector<MaterialParamDecl>& params, const std::string& permutationKey,
		SurfaceShaderBackend backend)
	{
		SurfaceCompileResult result;
		const auto started = std::chrono::steady_clock::now();
		const auto finish = [&result, started]()
		{
			result.ElapsedMilliseconds =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			return result;
		};

		try
		{
			SurfaceArtifact ignoredLastGood;
			result.LastGoodAvailable = LastGood(permutationKey, backend, ignoredLastGood);

			if (backend != SurfaceShaderBackend::VulkanSpirV)
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "surface shader backend '" + std::string(BackendName(backend)) +
					"' is not supported yet (M4-S4); this build only compiles Vulkan/SPIR-V";
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			// 参数表的轻量自检(调用方可能直接给手工表):名字合法/唯一/不与引擎保留名冲突。
			{
				std::unordered_map<std::string, bool> seen;
				for (const MaterialParamDecl& param : params)
				{
					const bool valid = IsUsableParamName(param.Name);
					if (!valid || seen.count(param.Name) != 0)
					{
						SurfaceDiagnostic diagnostic;
						diagnostic.Severity = "error";
						diagnostic.Message = valid
							? "material param '" + param.Name + "' is declared twice"
							: "material param name '" + param.Name + "' is not a usable HLSL identifier "
								"(reserved engine name or invalid characters)";
						result.Diagnostics.push_back(std::move(diagnostic));
						return finish();
					}
					seen.emplace(param.Name, true);
				}
			}

			const std::string effectiveSource = Trim(source).empty()
				? DefaultSurfaceFunctionSource() : source;

			std::string contractText;
			if (!ReadAllText(fs::path(ContractHeaderPath()), contractText))
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "surface contract header missing: " + ContractHeaderPath();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			const std::string wrapperSource = BuildWrapperSource(contractText, effectiveSource,
				permutationKey, backend, params);
			const uint64_t sourceHash = Fnv1a64String(effectiveSource);
			uint64_t keyHash = Fnv1a64String(wrapperSource);
			// 包装源码把用户源作为 #include 引用,所以用户源内容必须显式进键;
			// 否则"只改用户代码"会错误命中同一份 artifact(实测 2026-09-22)。
			keyHash = Mix(keyHash, Hex(sourceHash));
			keyHash = Mix(keyHash, BackendKey(backend));
			keyHash = Mix(keyHash, permutationKey);
			keyHash = Mix(keyHash, std::to_string(kSurfaceCacheVersion));
			keyHash = Mix(keyHash, std::to_string(ToolIdentity(std::string(WLD_DXC_DIR) + "dxc.exe")));
			keyHash = Mix(keyHash, std::to_string(ToolIdentity(std::string(WLD_DXC_DIR) + "dxcompiler.dll")));
			const std::string keyHex = Hex(keyHash);

			std::lock_guard<std::mutex> lock(s_CompileMutex);

			const fs::path cacheRoot = fs::path(WLD_INTERMEDIATE_DIR) / "SurfaceShaderCache";
			const fs::path keyDir = cacheRoot / keyHex;
			const fs::path spvPath = keyDir / "surface.spv";
			const fs::path asmPath = keyDir / kAssemblyFileName;

			std::error_code ec;
			// 汇编(-Fc)是反射的输入:产物在但汇编丢了 → 当成未命中,重新编译补上。
			if (fs::is_regular_file(spvPath, ec) && fs::file_size(spvPath, ec) > 0
				&& fs::is_regular_file(asmPath, ec) && fs::file_size(asmPath, ec) > 0)
			{
				std::vector<uint8_t> cached;
				if (ReadAllBytes(spvPath, cached) && !cached.empty())
				{
					s_CacheHits.fetch_add(1, std::memory_order_relaxed);
					result.Success = true;
					result.CacheHit = true;
					result.Artifact.Bytecode = std::move(cached);
					result.Artifact.CacheKey = keyHex;
					result.Artifact.Backend = BackendName(backend);
					result.Artifact.EntryPoint = kSurfaceEntryPoint;
					result.Artifact.SourceHash = sourceHash;
					StoreLastGood(permutationKey, backend, result.Artifact);
					return finish();
				}
			}

			s_CacheMisses.fetch_add(1, std::memory_order_relaxed);
			fs::create_directories(keyDir, ec);
			if (ec)
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "cannot create surface cache dir: " + ec.message();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			const fs::path userPath = keyDir / kUserSourceFileName;
			const fs::path wrapperPath = keyDir / "surface_wrapper.hlsl";
			const fs::path logPath = keyDir / "surface_compile.log";
			if (!WriteAllText(userPath, effectiveSource) || !WriteAllText(wrapperPath, wrapperSource))
			{
				SurfaceDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.Message = "cannot write generated surface source under " + keyDir.string();
				result.Diagnostics.push_back(std::move(diagnostic));
				return finish();
			}

			fs::remove(spvPath, ec);
			fs::remove(asmPath, ec);
			const std::string dxcPath = std::string(WLD_DXC_DIR) + "dxc.exe";
			// -fvk-use-gl-layout:参数块用 std140 对齐。实测引擎现有 4 个 cbuffer 的成员偏移
			// 在 DX 布局与 GL 布局下逐条相同(全 vec4/mat4 对齐),所以对既有管线零影响;
			// 换来的是"标量/短向量参数块"在 Vulkan 与将来 GL(spirv-cross std140)下同一套偏移。
			// -Fc:同时落一份 SPIR-V 汇编,参数反射(MaterialParams.cpp)直接读它。
			const std::string arguments = "-spirv -fvk-use-gl-layout -T ps_6_0 -E "
				+ std::string(kSurfaceEntryPoint) + " \"" + wrapperPath.string() + "\" -Fo \""
				+ spvPath.string() + "\" -Fc \"" + asmPath.string() + "\"";
			int exitCode = -1;
			std::string toolOutput;
			s_ToolInvocations.fetch_add(1, std::memory_order_relaxed);
			const bool launched = RunToolCapture(dxcPath, arguments, logPath.string(), exitCode, toolOutput);
			result.RawToolOutput = toolOutput;
			result.Diagnostics = ParseDiagnostics(toolOutput);

			std::vector<uint8_t> bytecode;
			const bool artifactReady = launched && exitCode == 0 &&
				fs::is_regular_file(spvPath, ec) && fs::file_size(spvPath, ec) > 0 &&
				ReadAllBytes(spvPath, bytecode) && !bytecode.empty();

			if (!artifactReady)
			{
				if (!launched)
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = "cannot launch dxc: " + dxcPath;
					result.Diagnostics.push_back(std::move(diagnostic));
					result.RawToolOutput += "\ncannot launch dxc: " + dxcPath + "\n";
				}
				else if (!HasErrorDiagnostic(result.Diagnostics))
				{
					SurfaceDiagnostic diagnostic;
					diagnostic.Severity = "error";
					diagnostic.Message = toolOutput.empty()
						? "dxc failed with exit code " + std::to_string(exitCode)
						: toolOutput;
					result.Diagnostics.push_back(std::move(diagnostic));
				}
				fs::remove(spvPath, ec);
				return finish();
			}

			result.Success = true;
			result.Artifact.Bytecode = std::move(bytecode);
			result.Artifact.CacheKey = keyHex;
			result.Artifact.Backend = BackendName(backend);
			result.Artifact.EntryPoint = kSurfaceEntryPoint;
			result.Artifact.SourceHash = sourceHash;
			StoreLastGood(permutationKey, backend, result.Artifact);
			return finish();
		}
		catch (const std::exception& error)
		{
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = std::string("surface compile threw: ") + error.what();
			result.Diagnostics.push_back(std::move(diagnostic));
			result.Success = false;
			return finish();
		}
		catch (...)
		{
			SurfaceDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = "surface compile threw an unknown exception";
			result.Diagnostics.push_back(std::move(diagnostic));
			result.Success = false;
			return finish();
		}
	}
}
