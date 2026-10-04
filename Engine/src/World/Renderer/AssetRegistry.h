#pragma once

#include "World/Core/Export.h"
#include "World/Core/AssetRef.h"
#include "World/Core/StringPool.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Texture/Texture.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	class WorldContext;
	class AssetCatalog;

	// ---------------------------------------------------------------------------
	// L2 资产驻留层(2026-10-04)。
	//
	// 解决的问题:此前"谁还被用着"没有名字 —— `.wmodel` / 精灵纹理的进程内缓存**只增不减**,
	// 换场景 / 关文档 / 关掉 UI 面板之后,旧资产的顶点缓冲与纹理永远驻留。
	//
	// 本表把「驻留 + 使用记账 + LRU 卸载」集中到一处,并按帧记账:
	//   * 解析(`Resolve*`)刷新该资产的 `LastUseFrame`;
	//   * 渲染器每帧开始调用 `BeginFrame()`(SceneRenderer::BeginScene);
	//   * `CollectGarbage()` 释放"连续 > kResidentIdleFrames 帧未被取用且未 Pin"的条目,
	//     并且**真的让出 GPU 资源**(Mesh 走 Renderer3D::PurgeMeshGpu,纹理走 TextureLibrary)。
	//
	// 归属:它是 `WorldContext::Resources()` 里的**世界级服务**,不是进程单例 —— 世界单例
	// 必须不随 registry 复制(见 Core/ResourceTable.h 与 contract.asset-identity-and-strings)。
	//
	// 边界(明确记账,不是遗漏):
	//   * 材质不在此处卸载。`MaterialLibrary` 同时是编辑器的文档模型(父级链 + 覆盖字段 +
	//     热重载警告 + 未保存脏状态),它的释放时机属于编辑器文档生命周期,不是渲染缓存 ——
	//     强行按帧 LRU 卸载会丢掉未保存修改。材质的驻留/释放随 T4(AssetId)与编辑器文档重做。
	//   * 异步加载(prefetch)不在本层;本层先把"驻留与释放"做成**唯一入口**,异步属于 T5c,
	//     需要"工作线程准备 CPU 数据 + 主线程提交 GPU"的两段式,单独任务做。
	// ---------------------------------------------------------------------------
	class WLD_API AssetRegistry
	{
	public:
		// 连续空闲多少帧后允许回收(~10 秒 @60fps)。编辑器可调;0 = 本帧就回收(测试用)。
		static constexpr uint64_t kResidentIdleFrames = 600;

		// 解析:命中驻留返回同一实例;未命中加载并驻留。失败返回 nullptr 且原因进 error。
		// 收**组件形态的引用**(AssetRef):路径失效时按稳定身份查 `AssetCatalog` 找回 ——
		// 这就是"资产改名/移动之后引用不断链"的落地点。
		Ref<Mesh> ResolveMesh(const AssetRef& asset, std::string* error = nullptr);
		Ref<Texture2D> ResolveTexture(const AssetRef& asset, std::string* error = nullptr);
		// 材质:走 MaterialLibrary(见上面的边界说明),但仍在此处记账,便于诊断/未来接管。
		Ref<class Material> ResolveMaterial(const AssetRef& asset, std::string* error = nullptr);

		// 绑定世界上下文:之后"路径失效 → 按身份查目录找回"才可用(不绑定 = 只用路径)。
		void BindContext(WorldContext* context) { m_Context = context; }

		// 帧节拍:渲染器每帧调一次(BeginScene)。返回推进后的帧号。
		uint64_t BeginFrame();

		// 回收:释放超期未用且未 Pin 的驻留项。返回真正释放的条目数。
		// 由帧循环在**帧边界**调用(不能与在飞命令缓冲抢资源;内部走延迟释放)。
		std::size_t CollectGarbage();

		// 内容变化的资产必须留着(编辑器正在编辑、脚本已获取引用等)。
		void Pin(PathId path);
		void Unpin(PathId path);
		bool IsPinned(PathId path) const;

		// 内容变化/重新导入:立刻丢驻留,下次 Resolve 重新读盘。
		void Invalidate(PathId path);
		void Clear();

		std::size_t ResidentCount() const { return m_Entries.size(); }
		uint64_t Frame() const { return m_Frame; }
		// 诊断:每个驻留项的 "type path idle=帧" 摘要(内存面板/测试)。
		std::vector<std::string> Describe() const;

	private:
		enum class Kind : uint8_t { Mesh, Texture, Material };

		struct Entry
		{
			Kind Type = Kind::Mesh;
			uint64_t LastUse = 0;
		};

		Entry& Touch(PathId path, Kind kind);
		bool Evict(PathId path, Kind kind);
		// 目录服务(惰性从 WorldContext::Resources() 取;没有目录时就退化为"只用路径")。
		AssetCatalog* Catalog();
		// 按身份查目录找**当前**路径(未登记/无目录 ⇒ 无效)。这是"改名/移动后仍能引用"的依据。
		PathId RemapByIdentity(const AssetRef& asset, const char* kindLabel);

		WorldContext* m_Context = nullptr;
		uint64_t m_Frame = 0;
		std::unordered_map<PathId, Entry> m_Entries;
		std::unordered_set<PathId> m_Pinned;
	};
}
