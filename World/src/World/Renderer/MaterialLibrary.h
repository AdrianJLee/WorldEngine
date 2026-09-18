#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/AssetHotReload.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	// D3:材质系统核心。材质实例的唯一持有者:加载/缓存/保存/热重载。
	//
	// 约定:
	//  - 路径以 Game/assets 为根(与场景/网格引用一致),内部做规范化后作为缓存键;
	//  - 同一路径永远返回同一个实例(引用同一性),渲染侧可以按实例指针缓存 GPU 资源;
	//  - 实例是引用计数的 shared_ptr:外部只持有 Ref,不要在别处 new;
	//  - 保存走的顺序是"写盘 → 回读校验 → 更新实例",避免半截文件污染内存态。
	class WLD_API MaterialLibrary
	{
	public:
		static MaterialLibrary& Get();
		static void Shutdown();

		// 加载 .wmat;失败返回 nullptr(错误经 error 输出),调用方据此显示警告。
		// 命中缓存时直接返回同一实例(不重新读盘)。
		Ref<Material> Load(const std::string& path, std::string* error = nullptr);

		// 新建未落盘材质(路径为空,IsDirty = true)。
		Ref<Material> CreateDefault(const std::string& name = "New Material");

		// 保存到材质当前路径;路径为空 = 另存为,必须显式给 path。
		// 成功后:清脏标记、更新缓存键、Revision 自增(渲染侧会重建贴图)。
		bool Save(const Ref<Material>& material, const std::string& path, std::string* error = nullptr);

		// 重新从磁盘读取该路径并**原地更新**已有实例(保留编辑器里的引用)。
		bool Reload(const std::string& path, std::string* error = nullptr);

		// 磁盘文件比内存态新(外部编辑器改动)时返回 true;供面板提示/自动重载。
		bool IsFileNewer(const Material& material) const;

		// W5-L1:帧边界轮询外部改动(内容哈希 + debounce;材质 150ms、贴图 500ms,互相独立)。
		//  - 监听集合 = 当前缓存的材质 + 它们引用的贴图(Albedo/Normal);首次见到即建立基线,不报告;
		//  - clean 材质:原地 Reload(实例同一性保持,Revision 前进)→ ReloadedMaterials;
		//    dirty 材质:SkippedDirtyMaterials(只报告,绝不覆盖未保存修改);
		//    读取/解析失败:FailedMaterials(保留旧内存态,详见 GetLoadWarning);
		//  - 贴图内容变化:MaterialTextureCache::Invalidate(path) + 引用它的材质
		//    InvalidateTextures() → InvalidatedTextures(旧句柄按 Renderer::QueueRelease 延迟释放);
		//  - 环境开关:WLD_ASSET_HOTRELOAD=0 整体关闭(不建立、不推进监听);
		//    WLD_ASSET_HOTRELOAD_TRACE=1 打 [asset-hot-reload] 日志。
		void PollAssetChanges(double deltaSeconds, AssetHotReloadReport& report);

		// 规范化:统一分隔符为 '/'、去掉前导 "./"。
		static std::string NormalizePath(const std::string& path);

		// 最近一次加载该材质的警告(字段缺失/夹紧/未知取值);成功且无警告时为空。
		// 编辑器面板用它显示黄色/红色提示。
		std::string GetLoadWarning(const std::string& path) const;

		// 内容根下所有 .wmat 的相对路径(升序,用于编辑器下拉列表/搜索)。
		std::vector<std::string> ScanMaterials() const;

	private:
		MaterialLibrary() = default;
		~MaterialLibrary() = default;

		std::unordered_map<std::string, Ref<Material>> m_Cache;
		std::unordered_map<std::string, std::string> m_Warnings;
		// W5-L1 轮询节拍(方案 §W5-L1:材质/场景 150ms、贴图 500ms)。
		static constexpr double kMaterialDebounceSeconds = AssetFileWatch::kDefaultDebounceSeconds;
		static constexpr double kTextureDebounceSeconds = 0.5;
		AssetFileWatch m_MaterialWatch { kMaterialDebounceSeconds };
		AssetFileWatch m_TextureWatch { kTextureDebounceSeconds };
	};
}
