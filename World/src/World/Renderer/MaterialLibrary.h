#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Material.h"

#include <string>
#include <unordered_map>

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

		// 规范化:统一分隔符为 '/'、去掉前导 "./"。
		static std::string NormalizePath(const std::string& path);

		// 最近一次加载该材质的警告(字段缺失/夹紧/未知取值);成功且无警告时为空。
		// 编辑器面板用它显示黄色/红色提示。
		std::string GetLoadWarning(const std::string& path) const;

	private:
		MaterialLibrary() = default;
		~MaterialLibrary() = default;

		std::unordered_map<std::string, Ref<Material>> m_Cache;
		std::unordered_map<std::string, std::string> m_Warnings;
	};
}
