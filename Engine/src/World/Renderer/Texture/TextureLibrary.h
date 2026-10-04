#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Renderer/Texture/Texture.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace World
{
	// 2D 精灵纹理的**驻留层**(2026-10-04)。
	//
	// 为什么需要它:`SpriteComponent` 过去直接持有 `Ref<Texture2D>`(运行态对象进了组件),
	// 违反 contract.runtime-state-outside-components(组件必须平凡可拷贝)。现在组件只存
	// 驻留 `PathId`,由这里做**解析 + 驻留**,渲染时按 id O(1) 取回同一个 `Ref<Texture2D>`。
	//
	// 约定:
	//  - 键 = 驻留路径 id(同一路径的不同写法归一后同键);
	//  - 同一路径永远返回同一个实例(引用同一性),渲染侧可按实例指针做批次合并/缓存;
	//  - 空 id / 读盘失败 ⇒ nullptr:调用方回退到纯色(Renderer2D 的既有行为),不抛;
	//  - 失败路径记进表,不逐帧重试(磁盘不被刷屏);Invalidate 后可重试;
	//  - 这是 WUI/GL 侧纹理(2D 精灵);3D 材质的 RHI 纹理另有 MaterialTextureCache,两者不混用;
	//  - 设备重建(后端切换)后调用 Clear(),句柄随旧设备一起失效。
	class WLD_API TextureLibrary
	{
	public:
		static TextureLibrary& Get();
		static void Shutdown();

		// 取驻留纹理;空 id ⇒ nullptr(不记失败)。
		Ref<Texture2D> Load(PathId path, std::string* error = nullptr);

		// 内容变化/重新导入:丢掉该 id 的驻留与失败记录,下次 Load 重新读盘。
		void Invalidate(PathId path);
		void Clear();

		// 诊断:当前驻留张数(内存面板/测试)。
		std::size_t ResidentCount() const;

	private:
		TextureLibrary() = default;
		TextureLibrary(const TextureLibrary&) = delete;
		TextureLibrary& operator=(const TextureLibrary&) = delete;

		std::unordered_map<PathId, Ref<Texture2D>> m_Cache;
		std::unordered_set<PathId> m_Failed;
	};
}