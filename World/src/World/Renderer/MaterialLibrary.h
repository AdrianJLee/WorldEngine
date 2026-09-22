#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/AssetHotReload.h"

#include <cstddef>
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
	//  - M3:.wmat 是材质实例(覆盖字段 + Parent)。Load 会**递归解析父级链**(深度上限 8),
	//    合并结果写进 GetDesc();循环引用 → 拒绝 + 可读链路;父级缺失/坏 → 退化成引擎内置默认
	//    + WLD_CORE_WARN(子材质仍可用,ParentWarning() 里能看到原因)。
	//    保存时只写覆盖字段 + Parent(全字段 + 无父级仍然写 v1,老文件逐字节不变)。
	class WLD_API MaterialLibrary
	{
	public:
		static MaterialLibrary& Get();
		static void Shutdown();

		// 加载 .wmat(含父级链解析);失败返回 nullptr(错误经 error 输出),调用方据此显示警告。
		// 成功但带警告(字段夹紧 / 父级退化)时 error 里是可读警告,不是失败。
		// 命中缓存时直接返回同一实例(不重新读盘)。
		Ref<Material> Load(const std::string& path, std::string* error = nullptr);

		// 新建未落盘材质(路径为空,IsDirty = true):没有父级,只有 Name 是覆盖字段
		// (其余字段继承引擎内置默认)。老调用点(整份 SetDesc)行为不变。
		Ref<Material> CreateDefault(const std::string& name = "New Material");

		// M3:新建未落盘的材质**实例**(路径为空,IsDirty = true)。
		//  - parentPath 为空 = 父级是引擎内置默认(文件里不写 Parent);
		//  - parentPath 指向已有 .wmat = 继承它,只写覆盖字段;父级缺失/坏 → 与 Load 同口径
		//    退化成引擎默认 + 可读警告(实例仍可用);
		//  - name 非空 = 立刻作为 Name 覆盖(会被写进文件);空 = Name 也跟随父级。
		// 新建向导(选父级)与 Save As 变体(父级 = 当前材质)都用它。
		Ref<Material> CreateInstance(const std::string& parentPath,
			const std::string& name = std::string(), std::string* error = nullptr);

		// 保存到材质当前路径;路径为空 = 另存为,必须显式给 path。
		// 写出内容 = 覆盖字段 + Parent(没有父级时不写 Parent);回读校验覆盖集/父级/值。
		// 成功后:清脏标记、更新缓存键、Revision 自增(渲染侧会重建贴图)。
		bool Save(const Ref<Material>& material, const std::string& path, std::string* error = nullptr);

		// 重新从磁盘读取该路径(含父级链)并**原地更新**已有实例(保留编辑器里的引用)。
		bool Reload(const std::string& path, std::string* error = nullptr);

		// 磁盘文件比内存态新(外部编辑器改动)时返回 true;供面板提示/自动重载。
		bool IsFileNewer(const Material& material) const;

		// W5-L1:帧边界轮询外部改动(内容哈希 + debounce;材质 150ms、贴图 500ms,互相独立)。
		//  - 监听集合 = 当前缓存的材质 + 它们引用的贴图(Albedo/Normal);首次见到即建立基线,不报告;
		//  - clean 材质:原地 Reload(实例同一性保持,Revision 前进)→ ReloadedMaterials;
		//    dirty 材质:SkippedDirtyMaterials(只报告,绝不覆盖未保存修改);
		//    读取/解析失败:FailedMaterials(保留旧内存态,详见 GetLoadWarning);
		//  - M3:材质指纹含父级链(见 FingerprintAsset),所以父级改动会让子材质一起变化;
		//    同一轮按"父级先、子级后"重载,链式继承当轮就能生效;
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

		// 父级链深度上限(方案:递归,深度上限 8)。超过 → 拒绝(与循环引用同一类结构错误)。
		static constexpr std::size_t kMaxParentDepth = 8;

		// 一次解析尝试的结果:
		//  - Instance 非空 = 成功(Warning 里可能带可读警告);
		//  - ChainError = 父级链结构错误(循环/超深)——必须向上拒绝,不做"退化"处理;
		//    其余失败(找不到/坏文件)在**父级位置**上可退化成引擎默认。
		struct Resolution
		{
			Ref<Material> Instance;
			std::string Error;
			std::string Warning;
			bool ChainError = false;
		};
		Resolution Resolve(const std::string& key);
		// Reload 的原地更新:值/覆盖集/父级/时间戳照抄解析结果,实例同一性保持。
		static void AdoptResolved(Material& target, const Material& source);

		std::unordered_map<std::string, Ref<Material>> m_Cache;
		std::unordered_map<std::string, std::string> m_Warnings;
		// 正在解析的路径栈(父级链):循环检测 + 深度上限。
		std::vector<std::string> m_ResolvingStack;
		// W5-L1 轮询节拍(方案 §W5-L1:材质/场景 150ms、贴图 500ms)。
		static constexpr double kMaterialDebounceSeconds = AssetFileWatch::kDefaultDebounceSeconds;
		static constexpr double kTextureDebounceSeconds = 0.5;
		AssetFileWatch m_MaterialWatch { kMaterialDebounceSeconds };
		AssetFileWatch m_TextureWatch { kTextureDebounceSeconds };
	};
}
