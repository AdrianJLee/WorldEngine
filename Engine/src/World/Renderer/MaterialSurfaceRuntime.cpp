#include "World/Renderer/MaterialSurfaceRuntime.h"

#include "World/Renderer/MaterialSurfaceRuntimeInternal.h"

#include "World/Core/Log.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Renderer.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace World
{
	namespace
	{
		// 一个键的**已发布快照**:版本号 + 四个变体管线 + 参数布局。
		// 发布是原子的:要么整份换新,要么保留上一份(见 Install)。
		struct SurfacePipelineEntry
		{
			size_t Version = 0;
			Rhi::Handle<Rhi::Pipeline> Pipelines[kSurfacePipelineVariantCount];
			MaterialParamLayout Params;
		};

		std::mutex s_Mutex;
		std::unordered_map<std::string, SurfacePipelineEntry> s_Entries;
		size_t s_PublishCount = 0;
		SurfacePipelineEnvironment s_Environment;

		const char* EntryPointForVariant(SurfacePipelineVariant variant)
		{
			switch (variant)
			{
				case SurfacePipelineVariant::Solid: return "VSMain";
				case SurfacePipelineVariant::Transparent: return "VSMain";
				case SurfacePipelineVariant::Instanced: return "VSMainInstanced";
				case SurfacePipelineVariant::Skinned: return "VSMainSkinned";
				default: return nullptr;
			}
		}

		const char* VariantName(SurfacePipelineVariant variant)
		{
			switch (variant)
			{
				case SurfacePipelineVariant::Solid: return "solid";
				case SurfacePipelineVariant::Transparent: return "transparent";
				case SurfacePipelineVariant::Instanced: return "instanced";
				case SurfacePipelineVariant::Skinned: return "skinned";
				default: return "unknown";
			}
		}

		const MeshVertexLayout& LayoutForVariant(const SurfacePipelineEnvironment& environment,
			SurfacePipelineVariant variant)
		{
			switch (variant)
			{
				case SurfacePipelineVariant::Instanced: return environment.InstancedLayout;
				case SurfacePipelineVariant::Skinned: return environment.SkinnedLayout;
				default: return environment.SolidLayout;
			}
		}

		bool HasVertexEntry(const SurfaceArtifact& artifact, const std::string& entryPoint)
		{
			const SurfaceVertexStage* stage = artifact.FindVertexStage(entryPoint.c_str());
			return stage != nullptr && !stage->Bytecode.empty();
		}

		// ---- Slang-T4a:表面产物必须与**当前设备后端**一致 ----
		// 同一份材质着色器源有两个目标(Vulkan profile → SPIR-V 1.3;GL profile → SPIR-V 1.0),
		// 裁剪空间/深度约定不同,混用会在驱动层失败或画出错误结果。这里给出"本设备需要的
		// 目标名"(与 MaterialSurfaceCompiler::BackendName 同一套字符串),由 Install 校验;
		// 不匹配 = 可读错误,不在运行时偷偷换成另一份产物。
		const char* ArtifactBackendForCurrentDevice()
		{
			return Renderer::GetAPI() == RendererAPI::API::Vulkan ? "vulkan-spirv" : "opengl-spirv";
		}

		bool SpirvLooksValid(const std::vector<uint8_t>& bytes)
		{
			if (bytes.size() < 20 || (bytes.size() % 4) != 0)
				return false;
			uint32_t magic = 0;
			std::memcpy(&magic, bytes.data(), sizeof(magic));
			return magic == 0x07230203u;
		}

		// GL(ARB_gl_spirv)只接受 SPIR-V 1.0,且不接受分离采样器形态(OpTypeSampler):
		// T1 实测这种模块交给 glShaderBinary 后 NVIDIA 驱动会在第一次采样时崩
		// (nvoglv64+0x75abcb,14/14 同一签名)。命中任一条 = 这份产物不能用于 GL,
		// 给可读原因而不是把坏模块丢给驱动。
		bool SpirvModuleFitsGlSpirv(const std::vector<uint8_t>& bytes, std::string* reason)
		{
			if (!SpirvLooksValid(bytes))
			{
				if (reason)
					*reason = "not a SPIR-V module (bad magic or not 32-bit aligned)";
				return false;
			}
			uint32_t version = 0;
			std::memcpy(&version, bytes.data() + 4, sizeof(version));
			if (version != 0x00010000u)
			{
				if (reason)
				{
					std::ostringstream text;
					text << "SPIR-V version word 0x" << std::hex << version
						<< " is not 1.0 (ARB_gl_spirv needs SPIR-V 1.0)";
					*reason = text.str();
				}
				return false;
			}
			// 头部 5 个字;之后每条指令 = (wordCount << 16) | opcode,OpTypeSampler = 26。
			const size_t words = bytes.size() / 4;
			for (size_t offset = 5; offset < words;)
			{
				uint32_t word = 0;
				std::memcpy(&word, bytes.data() + offset * 4, sizeof(word));
				const uint32_t wordCount = word >> 16;
				if (wordCount == 0)
					break;
				if ((word & 0xFFFFu) == 26u)
				{
					if (reason)
						*reason = "module declares OpTypeSampler (separate sampler); GL needs the combined "
							"`Sampler2D` form";
					return false;
				}
				offset += wordCount;
			}
			return true;
		}

		// Slang-T3:从编译产物的反射 JSON(`-reflection-json`,与 artifact 同键)+ artifact 自带的
		// SPIR-V 反射参数布局("成员真的被读"看 SPIR-V 里的 OpAccessChain)。
		// 反射是纯函数,不再调用编译器;JSON 缺失 = 该 artifact 不能用于运行时上传。
		//
		// Slang-T6a:反射 JSON 有两处来源,顺序固定 ——
		//  1) 开发形态:编译器写的仓内文件(`MaterialSurfaceCompiler::ReflectionPath`,
		//     与 artifact 的内容键同目录);
		//  2) 打包形态:包内烘好的 `<键>.PSMain[.gl].reflection.json`(命名见
		//     MaterialLibrary::SurfaceReflectionLogicalPath;读取走 VFS,与 .wmat/材质着色器同一口径)。
		// 打包运行时没有编译器,所以第 2 条是它的唯一来源。
		bool ReflectArtifactLayout(const std::string& key, const SurfaceArtifact& artifact,
			MaterialParamLayout* out, std::string* error)
		{
			std::string reflectionJson;
			const std::string reflectionPath = MaterialSurfaceCompiler::ReflectionPath(artifact);
			if (!reflectionPath.empty())
			{
				std::ifstream stream(reflectionPath, std::ios::binary);
				if (stream)
				{
					std::ostringstream buffer;
					buffer << stream.rdbuf();
					reflectionJson = buffer.str();
				}
			}

			std::string cookedLogical;
			if (reflectionJson.empty() && !key.empty() && key.find('#') == std::string::npos)
			{
				// 与产物同一套命名:GL 目标(SPIR-V 1.0)是 `.gl.` 中缀。
				cookedLogical = MaterialLibrary::SurfaceReflectionLogicalPath(key,
					artifact.Backend == "opengl-spirv");
				if (!MaterialIO::ReadFileText(cookedLogical, reflectionJson))
					reflectionJson.clear();
			}
			if (reflectionJson.empty())
			{
				if (error)
				{
					*error = "surface artifact has no Slang reflection JSON (-reflection-json); "
						"cannot reflect parameter layout (compiler cache: '"
						+ (reflectionPath.empty() ? std::string("<none>") : reflectionPath)
						+ "', cooked: '"
						+ (cookedLogical.empty() ? std::string("<none>") : cookedLogical) + "')";
				}
				return false;
			}
			return ReflectParamLayoutFromReflectionJson(reflectionJson, artifact.Bytecode, out, error);
		}

		// 一个变体的完整管线(顶点入口 + 像素入口都来自 artifact)。
		Rhi::Handle<Rhi::Pipeline> CreateVariantPipeline(const SurfacePipelineEnvironment& environment,
			const SurfaceArtifact& artifact, SurfacePipelineVariant variant)
		{
			const char* entryPoint = EntryPointForVariant(variant);
			if (!entryPoint)
				return nullptr;
			const SurfaceVertexStage* vertexStage = artifact.FindVertexStage(entryPoint);
			if (!vertexStage || vertexStage->Bytecode.empty() || artifact.Bytecode.empty())
				return nullptr;

			Rhi::ShaderDesc shaderDesc;
			shaderDesc.DebugName = std::string("SurfaceMaterial-") + entryPoint;
			Rhi::ShaderStageSource vertex;
			vertex.Stage = Rhi::ShaderStage::Vertex;
			vertex.EntryPoint = vertexStage->EntryPoint;
			vertex.SpirV = vertexStage->Bytecode;
			Rhi::ShaderStageSource fragment;
			fragment.Stage = Rhi::ShaderStage::Fragment;
			fragment.EntryPoint = artifact.EntryPoint.empty() ? "PSMain" : artifact.EntryPoint;
			fragment.SpirV = artifact.Bytecode;
			shaderDesc.Stages = { std::move(vertex), std::move(fragment) };
			const Rhi::Handle<Rhi::Shader> shader = Renderer::GetDevice()->CreateShader(shaderDesc);
			if (!shader)
				return nullptr;

			const MeshVertexLayout& layout = LayoutForVariant(environment, variant);
			Rhi::PipelineDesc desc;
			desc.Shader = shader;
			desc.RenderPass = environment.RenderPass;
			// set 0 = 相机/灯光/阴影(与引擎管线同一份),set 1 = 对象 + 骨骼 + 参数块,
			// set 2 = 表面材质(贴图)。表面模板的 cbuffer 声明与这份布局逐条对应。
			desc.DescriptorSetLayouts = {
				environment.GlobalLayout,
				environment.ObjectLayout,
				environment.SurfaceMaterialLayout,
			};
			desc.VertexBindings = layout.Bindings;
			desc.VertexAttributes = layout.Attributes;
			desc.Topology = Rhi::PrimitiveTopology::TriangleList;
			desc.Cull = Rhi::CullMode::Back;
			desc.Front = environment.Front;
			desc.Samples = environment.Samples;
			desc.DepthStencil.DepthTest = true;
			desc.DepthStencil.DepthWrite = variant != SurfacePipelineVariant::Transparent;
			desc.DepthStencil.DepthCompare = Rhi::CompareOp::LessOrEqual;
			if (variant == SurfacePipelineVariant::Transparent)
			{
				// 与引擎透明管线同款:颜色附件混合 + 不写深度;entity-id 附件必须关混合,
				// 否则视口拾取读到的 id 会被 alpha 混坏。
				Rhi::BlendAttachmentState blend;
				blend.BlendEnable = true;
				blend.SrcColor = Rhi::BlendFactor::SrcAlpha;
				blend.DstColor = Rhi::BlendFactor::OneMinusSrcAlpha;
				blend.ColorOp = Rhi::BlendOp::Add;
				blend.SrcAlpha = Rhi::BlendFactor::One;
				blend.DstAlpha = Rhi::BlendFactor::OneMinusSrcAlpha;
				blend.AlphaOp = Rhi::BlendOp::Add;
				Rhi::BlendAttachmentState idBlend;
				idBlend.BlendEnable = false;
				desc.Blends = { blend, idBlend };
			}
			desc.DebugName = std::string("SurfaceMaterial.") + VariantName(variant);
			return Renderer::GetDevice()->CreatePipeline(desc);
		}
	}

	void RegisterSurfacePipelineEnvironment(const SurfacePipelineEnvironment& environment)
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		s_Environment = environment;
	}

	void ClearSurfacePipelineEnvironment()
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		s_Environment = SurfacePipelineEnvironment {};
	}

	size_t SurfacePublishedVersion(const std::string& key)
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		const auto found = s_Entries.find(key);
		return found == s_Entries.end() ? 0 : found->second.Version;
	}

	bool FetchSurfacePipeline(const std::string& key, SurfacePipelineVariant variant,
		Rhi::Handle<Rhi::Pipeline>* out)
	{
		if (!out)
			return false;
		const size_t index = static_cast<size_t>(variant);
		if (index >= kSurfacePipelineVariantCount)
			return false;
		std::lock_guard<std::mutex> lock(s_Mutex);
		const auto found = s_Entries.find(key);
		if (found == s_Entries.end())
			return false;
		*out = found->second.Pipelines[index];
		return *out != nullptr;
	}

	bool FetchSurfaceParamLayout(const std::string& key, MaterialParamLayout* out)
	{
		if (!out)
			return false;
		std::lock_guard<std::mutex> lock(s_Mutex);
		const auto found = s_Entries.find(key);
		if (found == s_Entries.end())
			return false;
		*out = found->second.Params;
		return true;
	}

	MaterialSurfaceRuntime::InstallResult MaterialSurfaceRuntime::Install(const std::string& key,
		const SurfaceArtifact& artifact)
	{
		InstallResult result;
		if (key.empty())
		{
			result.Error = "surface key is empty";
			return result;
		}
		if (artifact.Bytecode.empty())
		{
			result.Error = "surface artifact has no pixel bytecode";
			return result;
		}
		const Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		if (!device)
		{
			result.Error = "no RHI device";
			return result;
		}
		// Slang-T4a:装配哪个后端的管线由**当前设备后端**决定 —— artifact 必须就是该目标的
		// 产物(两个目标的裁剪空间/深度约定不同,SPIR-V 版本与采样器形态也不同)。
		// 不匹配时给可读原因,不静默换用另一份产物。
		const std::string deviceBackend = Renderer::GetBackendName();
		const std::string expectedBackend = ArtifactBackendForCurrentDevice();
		if (artifact.Backend != expectedBackend)
		{
			result.Error = "surface artifact targets '" + artifact.Backend
				+ "' but the current device backend '" + deviceBackend + "' needs '" + expectedBackend
				+ "'; recompile the material shader for this backend (GL uses <stage>_5_0+spirv_1_0, "
				"Vulkan uses <stage>_6_0)";
			return result;
		}
		if (expectedBackend == "opengl-spirv")
		{
			// GL 的 SPIR-V 摄入前提(GL 4.6 core + GL_ARB_gl_spirv)与模块形态都要先验证:
			// 否则 OpenGLPipeline 只会在 glShaderBinary/glSpecializeShader 处失败。
			if (!device->GetCapabilities().SpirVShaderModules)
			{
				result.Error = "GL_SPIRV capability missing (need GL 4.6 core + GL_ARB_gl_spirv and a "
					"live device): surface SPIR-V modules cannot be ingested; switch to the Vulkan "
					"backend or update the GL driver";
				return result;
			}
			std::string reason;
			if (!SpirvModuleFitsGlSpirv(artifact.Bytecode, &reason))
			{
				result.Error = "surface pixel stage cannot be ingested by GL: " + reason;
				return result;
			}
			for (const SurfaceVertexStage& stage : artifact.VertexStages)
			{
				if (stage.Bytecode.empty())
					continue;
				if (!SpirvModuleFitsGlSpirv(stage.Bytecode, &reason))
				{
					result.Error = "surface vertex stage '" + stage.EntryPoint
						+ "' cannot be ingested by GL: " + reason;
					return result;
				}
			}
		}

		SurfacePipelineEnvironment environment;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			environment = s_Environment;
		}
		if (!environment.Valid || !environment.RenderPass || !environment.ObjectLayout
			|| !environment.SurfaceMaterialLayout || !environment.GlobalLayout)
		{
			result.Error = "surface pipeline environment is not registered "
				"(Renderer3D::Init must run before Install)";
			return result;
		}
		if (!HasVertexEntry(artifact, "VSMain"))
		{
			result.Error = "surface artifact has no VSMain vertex stage (solid pipeline is required)";
			return result;
		}

		MaterialParamLayout params;
		std::string reflectError;
		if (!ReflectArtifactLayout(key, artifact, &params, &reflectError))
		{
			result.Error = "cannot reflect material parameter layout: " + reflectError;
			return result;
		}

		// ---- 先把本次需要的东西全部建好(不动已发布状态) ----
		SurfacePipelineEntry next;
		next.Params = params;
		try
		{
			for (size_t index = 0; index < kSurfacePipelineVariantCount; ++index)
			{
				const SurfacePipelineVariant variant = static_cast<SurfacePipelineVariant>(index);
				// 变体没在 artifact 里 / 建不出来 → 该变体留空,渲染侧回退引擎管线
				// (D6:不静默降级成错误的管线状态,失败有可读原因)。
				const Rhi::Handle<Rhi::Pipeline> pipeline = CreateVariantPipeline(environment, artifact,
					variant);
				if (pipeline)
				{
					next.Pipelines[index] = pipeline;
					++result.Pipelines;
				}
				else if (variant == SurfacePipelineVariant::Solid)
				{
					// Solid 是表面材质的最低要求:建不出来 = 整次安装失败(保留上一份)。
					result.Success = false;
					result.Pipelines = 0;
					result.Error = "failed to create the solid surface pipeline for key '" + key + "'";
					return result;
				}
			}
		}
		catch (const std::exception& error)
		{
			result.Success = false;
			result.Pipelines = 0;
			result.Error = std::string("surface pipeline creation threw: ") + error.what();
			return result;   // next 里的半成品句柄在这里析构(从未被任何命令引用)
		}

		// ---- 原子发布:整份换新;旧管线按 QueueRelease 延迟释放 ----
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			const auto existing = s_Entries.find(key);
			next.Version = existing != s_Entries.end() ? existing->second.Version + 1 : 1;
			result.ReplacedExisting = existing != s_Entries.end();
			if (existing != s_Entries.end())
			{
				for (const Rhi::Handle<Rhi::Pipeline>& pipeline : existing->second.Pipelines)
				{
					if (!pipeline)
						continue;
					Renderer::QueueRelease([pipeline]() { /* 句柄在这里释放 */ });
				}
			}
			s_Entries[key] = std::move(next);
			++s_PublishCount;
		}
		result.Success = true;
		return result;
	}

	bool MaterialSurfaceRuntime::Uninstall(const std::string& key)
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		const auto found = s_Entries.find(key);
		if (found == s_Entries.end())
			return false;
		const SurfacePipelineEntry removed = std::move(found->second);
		s_Entries.erase(found);
		for (const Rhi::Handle<Rhi::Pipeline>& pipeline : removed.Pipelines)
		{
			if (!pipeline)
				continue;
			Renderer::QueueRelease([pipeline]() { /* 句柄在这里释放 */ });
		}
		return true;
	}

	bool MaterialSurfaceRuntime::HasPipeline(const std::string& key)
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		const auto found = s_Entries.find(key);
		if (found == s_Entries.end())
			return false;
		return found->second.Pipelines[static_cast<size_t>(SurfacePipelineVariant::Solid)] != nullptr;
	}

	size_t MaterialSurfaceRuntime::PublishedVersion(const std::string& key)
	{
		return SurfacePublishedVersion(key);
	}

	size_t MaterialSurfaceRuntime::InstalledKeyCount()
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		return s_Entries.size();
	}

	size_t MaterialSurfaceRuntime::PublishCount()
	{
		std::lock_guard<std::mutex> lock(s_Mutex);
		return s_PublishCount;
	}

	void MaterialSurfaceRuntime::Shutdown()
	{
		// 关闭设备**之前**由 Renderer3D::Shutdown 调用:此时已 WaitIdle,直接放句柄
		// (队列里的延迟释放反而会让句柄活过设备销毁,见 RHI 句柄的生命周期口径)。
		// 建管线环境不在这里清(它属于 Renderer3D;重复 Install 的调用方不该把环境清掉),
		// 由 Renderer3D::Shutdown 显式 ClearSurfacePipelineEnvironment。
		std::lock_guard<std::mutex> lock(s_Mutex);
		s_Entries.clear();
	}
}
