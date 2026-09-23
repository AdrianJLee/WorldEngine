#include "World/Renderer/MaterialSurfaceRuntime.h"

#include "World/Renderer/MaterialSurfaceRuntimeInternal.h"

#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"

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

		// Slang-T3:从编译产物的反射 JSON(`-reflection-json`,与 artifact 同键)+ artifact 自带的
		// SPIR-V 反射参数布局("成员真的被读"看 SPIR-V 里的 OpAccessChain)。
		// 反射是纯函数,不再调用编译器;JSON 缺失 = 该 artifact 不能用于运行时上传。
		bool ReflectArtifactLayout(const SurfaceArtifact& artifact, MaterialParamLayout* out,
			std::string* error)
		{
			const std::string reflectionPath = MaterialSurfaceCompiler::ReflectionPath(artifact);
			if (reflectionPath.empty())
			{
				if (error)
					*error = "surface artifact has no Slang reflection JSON (-reflection-json); "
						"cannot reflect parameter layout";
				return false;
			}
			std::ifstream stream(reflectionPath, std::ios::binary);
			if (!stream)
			{
				if (error)
					*error = "cannot read surface reflection JSON: " + reflectionPath;
				return false;
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return ReflectParamLayoutFromReflectionJson(buffer.str(), artifact.Bytecode, out, error);
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
		// D4:接口后端无关,但本批只落 Vulkan(OpenGL 走同一份 SPIR-V 的 spirv-cross 归 M4-S4)。
		if (Renderer::GetBackendName() != "vulkan")
		{
			result.Error = "surface material pipelines need the Vulkan backend in this build "
				"(backend='" + Renderer::GetBackendName() + "'; OpenGL is M4-S4)";
			return result;
		}
		if (!Renderer::GetDevice())
		{
			result.Error = "no RHI device";
			return result;
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
		if (!ReflectArtifactLayout(artifact, &params, &reflectError))
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
