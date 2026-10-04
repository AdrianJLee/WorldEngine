#pragma once

#include "World/Core/AssetId.h"
#include "World/Core/Export.h"
#include "World/Core/StringPool.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	// ---------------------------------------------------------------------------
	// 资产目录(L1 层,2026-10-04,contract.asset-identity-and-strings)。
	//
	// 职责:建立 `AssetId ↔ 逻辑路径` 的双向映射 —— 它是"路径失效时按身份找回"的唯一依据。
	//
	// 数据来源是**资产文件自身**(id 写在 .wmodel 元数据 / .wmat 与 .wtex 的头里),
	// 不是旁路 sidecar:见 traps/wimport-removed.md,同一件事两份真相被明确否决。
	//
	// 使用:
	//   * 启动/换项目时 `Scan(contentRoot)` 一次(读每个资产的头部,不加载几何/文本全量);
	//   * 之后 `FindPath(id)` 是纯查表;目录随编辑器保存/导入调用 `Refresh*` 增量更新。
	//
	// 归属:`WorldContext::Resources()` 里的世界级服务(与 AssetRegistry 同层)。
	// ---------------------------------------------------------------------------
	class WLD_API AssetCatalog
	{
	public:
		// 识别"能带身份"的资产扩展名(小写,含点):.wmodel / .wmat / .wtex。
		static bool IsIdentifiedAssetPath(const std::string& logicalPath);

		// 扫描内容根(绝对路径),枚举已知扩展名并读各自的身份。
		// 返回登记成功的资产数;单个文件读失败只计入 Skipped 而不中断(坏资产不该让目录为空)。
		std::size_t Scan(const std::string& contentRootAbsolute);

		// 登记/更新一个资产(编辑器保存、导入产出后调用)。
		void Register(const std::string& logicalPath, AssetId identity);
		// 路径内容变化(改名/删除):清掉该路径的登记。
		void Unregister(const std::string& logicalPath);

		// 按身份找**当前**路径(未登记 ⇒ 无效 PathId)。
		PathId FindPath(AssetId identity) const;
		// 按路径找身份(未登记/无身份 ⇒ 无效)。
		AssetId FindIdentity(PathId path) const;

		void Clear();
		std::size_t Size() const { return m_ByIdentity.size(); }
		std::size_t Skipped() const { return m_Skipped; }
		// 诊断:全部登记项 "0x… -> path"。
		std::vector<std::string> Describe() const;

	private:
		std::unordered_map<AssetId, PathId> m_ByIdentity;
		std::unordered_map<PathId, AssetId> m_ByPath;
		std::size_t m_Skipped = 0;
	};
}