#pragma once

// M4-S3:表面管线运行时的**引擎内部**接口(Renderer3D ↔ MaterialSurfaceRuntime)。
// 公开头(`MaterialSurfaceRuntime.h`)只有 Install/Uninstall/查询;这里补的是
// "渲染侧怎么选管线 / 怎么拿参数布局"以及"建管线需要哪些引擎资源"。
// 两边的边界由本头固定:Runtime 不直接访问 Renderer3D 的私有 state,
// Renderer3D 也不直接建表面管线。

#include "World/RHI/Rhi.h"
#include "World/Renderer/MaterialParams.h"
#include "World/Renderer/Mesh.h"

#include <cstdint>
#include <string>

namespace World
{
	// 表面管线的四种变体(与 SurfaceArtifact::VertexStages 的入口一一对应):
	//  - Solid / Transparent 用 "VSMain"(静态网格布局);
	//  - Instanced 用 "VSMainInstanced"(静态布局 + per-instance 绑定);
	//  - Skinned 用 "VSMainSkinned"(蒙皮布局,布局 2)。
	enum class SurfacePipelineVariant : uint8_t
	{
		Solid = 0,
		Transparent = 1,
		Instanced = 2,
		Skinned = 3,
		Count = 4,
	};

	constexpr size_t kSurfacePipelineVariantCount = static_cast<size_t>(SurfacePipelineVariant::Count);

	// Renderer3D::Init 注册的"建管线环境"。Runtime 只用这份数据建管线:
	// RenderPass / 描述符布局 / 顶点布局 / 采样数 / 正面绕序。
	struct SurfacePipelineEnvironment
	{
		bool Valid = false;
		Rhi::Handle<Rhi::RenderPass> RenderPass;
		Rhi::Handle<Rhi::DescriptorSetLayout> GlobalLayout;           // set 0:相机/灯光/阴影
		// set 1:1 = 对象 UBO、3 = 骨骼调色板、4 = 材质参数块(register b4, space1)。
		Rhi::Handle<Rhi::DescriptorSetLayout> ObjectLayout;
		// set 2:1 = albedo、2 = normal、4..11 = 注解声明的贴图参数(space2 的 t4..t11)。
		Rhi::Handle<Rhi::DescriptorSetLayout> SurfaceMaterialLayout;
		MeshVertexLayout SolidLayout;       // 静态网格(布局 1)
		MeshVertexLayout InstancedLayout;   // 静态 + 实例绑定(binding 1,PerInstance)
		MeshVertexLayout SkinnedLayout;     // 蒙皮网格(布局 2)
		Rhi::SampleCount Samples = Rhi::SampleCount::Count1;
		Rhi::FrontFace Front = Rhi::FrontFace::CounterClockwise;
	};

	// 渲染线程调用(Renderer3D::Init / Shutdown)。
	void RegisterSurfacePipelineEnvironment(const SurfacePipelineEnvironment& environment);
	void ClearSurfacePipelineEnvironment();

	// 渲染侧查询(渲染线程)。key 不透明,约定见 MaterialSurfaceRuntime.h。
	// SurfacePublishedVersion 返回 0 = 该键从未成功安装。
	size_t SurfacePublishedVersion(const std::string& key);
	// 取该键某个变体的已发布管线;false = 没有该键 / 该变体没建出来(调用方回退引擎管线)。
	bool FetchSurfacePipeline(const std::string& key, SurfacePipelineVariant variant,
		Rhi::Handle<Rhi::Pipeline>* out);
	// 取该键已发布版本的参数布局(反射自编译产物;渲染侧据此打包参数 / 绑贴图槽)。
	bool FetchSurfaceParamLayout(const std::string& key, MaterialParamLayout* out);
}
