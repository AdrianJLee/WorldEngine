#pragma once

#include "World/Core/Core.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Renderer3D.h"

#include <cstdint>
#include <entt.hpp>
#include <glm/glm.hpp>
#include <vector>

namespace World
{
	// 一条待提交的网格绘制(**相机无关**:不含剔除结果/阴影判定,那些在提交侧按视图算)。
	struct FrameMeshDraw
	{
		entt::entity Entity = entt::null;
		const glm::mat4* Model = nullptr;
		Ref<Mesh> MeshAsset;
		// D5:UINT32_MAX = 整网格提交(内置 primitive / 无 submesh 的资产);
		// 否则只提交该 submesh(独立对象槽位 + 该 submesh 槽位的材质)。
		uint32_t SubmeshIndex = UINT32_MAX;
		Ref<Material> MaterialAsset;
		glm::vec4 Color { 1.0f };
		bool Transparent = false;
		// D5c-4a:蒙皮绘制(走 Renderer3D::SubmitSkinned/SubmitShadowSkinned;不进实例化合批)。
		// Palette 是 AnimationSystem 当帧缓存的该实体调色板;nullptr = 该实体本帧没有蒙皮结果。
		bool Skinned = false;
		const std::vector<glm::mat4>* Palette = nullptr;
		// D8a:世界空间 AABB(逐子网格;视锥剔除用)。
		glm::vec3 WorldMin { 0.0f };
		glm::vec3 WorldMax { 0.0f };
	};

	// 一帧的渲染抽取结果(相机无关部分)。两次抽取之间有效。
	// 指针成员(Model / Palette)指向注册表与 AnimationSystem 的当帧缓存 ——
	// 抽取(PreRender)到提交(同帧稍后)之间没有东西会改它们。
	struct FrameExtract
	{
		std::vector<FrameMeshDraw> Draws;
		// 灯光(方向光/点光/环境光已折算成 Renderer3D 的 uniform);**尚未**应用阴影矩阵与
		// `rendering.shadows` 开关 —— 那两步在提交侧(依赖本帧全部 draw 的包围盒与后端约定)。
		LightRig Lights;
		// P6:插值后的渲染矩阵(仅物理插值开启时非空)。
		// `FrameMeshDraw::Model` 可能指向这里的元素 ⇒ 抽取期间必须 reserve 到位,不能中途重分配。
		// 与 Draws 同样的生命周期:抽取(PreRender)→ 提交(同帧稍后)之间有效。
		std::vector<glm::mat4> InterpolatedModels;
		// 全部 draw 的世界包围盒并集(方向光阴影的正交矩阵要覆盖它)。HasBounds=false = 无网格。
		glm::vec3 BoundsMin { 0.0f };
		glm::vec3 BoundsMax { 0.0f };
		bool HasBounds = false;

		void Clear()
		{
			Draws.clear();
			InterpolatedModels.clear();
			Lights = LightRig {};
			BoundsMin = glm::vec3(0.0f);
			BoundsMax = glm::vec3(0.0f);
			HasBounds = false;
		}
	};
}
