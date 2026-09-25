#pragma once

// M4-TEX-P11:纹理引用的**编辑器侧数据适配层**(运行期事实 → `Wui::WuiTexturePicker` 入参)。
//
// 口径(知识库 `contracts/texture-import.md` + `contracts/wui-component-library.md`):
//   * 一条纹理引用可以写**源图**(`textures/Icon.png`)或**资产**(`textures/Icon.wtex`);
//     资产 = 设置 + (容器时)内嵌源字节,源图只是可选导入源;同目录同主名有资产时只留资产条目;
//   * 赋纹理时选中**源图** = 先"确保资产存在"(当场导入成单文件容器),材质/注解写资产路径;
//   * 本层**只产数据**:不画控件、不登记 a11y、不持有 UI 状态。
//
// 为什么单独一层:概念归属表 `tools/agents/wui-concept-owners.json` 的 texture-ref 概念里
// 本文件是**数据侧唯一实现** —— 材质编辑器(槽位 / `.wmat` 参数行 / `.slang` 参数列)只调这里
// 和 `Wui::WuiTexturePicker`,不自己扫盘、不自己算状态与徽标。

#include "World/WUI/WuiTexturePicker.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	namespace Editor
	{
		// 一条候选纹理。Value = 写进材质/注解的逻辑路径;Label = 完整逻辑路径(资产条目把导入源名
		// 写进括号);Badges = 组件徽标 token 表(词表见 WuiTexturePicker.h 的 kBadgeSpecs)。
		struct TextureRefEntry
		{
			std::string Value;
			std::string Label;
			std::string Badges;
		};

		// 一条引用的**现场判定**(校验区 / 行内提示 / 工具提示 / 徽标共用这一份)。
		struct TextureRefFacts
		{
			bool Known = false;          // 非空引用才算 Known
			bool IsAsset = false;
			bool AssetExists = false;
			bool SourceExists = false;
			bool InContentRoot = true;   // 路径是内容根相对且不含 ".."
			bool Embedded = false;       // 容器:源字节在资产里(P9 起 = 有效)
			bool Container = false;      // `.wtex` 有 `---payload`
			bool Legacy = false;         // 旧式设置文件(无 payload,字节要靠外部源图)
			bool Readable = true;        // 资产能读出来(坏 YAML / 未知字段 = false)
			std::string Source;          // 资产 → 源图(源图引用 = 自己)
			std::string ImportSource;    // 盘上的可选导入源(容器没有也能用)
			std::string Error;           // 解析不了的原因(空 = 没有解析错误)
			std::string Badges;          // 当前值的徽标 token 表(逗号分隔;空 = 无)
			Wui::WuiTexturePickerState State = Wui::WuiTexturePickerState::Empty;
		};

		// 内容根(项目内容资产目录);与纹理设置面板 / 烘焙器同一口径(WLD_PROJECT_DIR/assets)。
		std::filesystem::path ContentRootPath();

		// 候选清单:先扫 `.wtex` 资产、再无同主名资产的源图,整表按**逻辑路径**排序;
		// 带 1.5s TTL 缓存(每帧要列表,不每帧扫盘)。
		const std::vector<TextureRefEntry>& TextureRefEntries();
		// 立即作废缓存(刚导入/赋了新资产时要马上能在下拉里看到)。
		void InvalidateTextureRefCatalog();

		// 逻辑路径 → **便宜**的现场判定(资产是否存在 / 资产 → 源图是否解析得到且存在 /
		// 是否在内容根内;不做产物哈希)。校验区、行内提示、工具提示每帧都用这一条。
		TextureRefFacts TextureRefFactsFor(const std::string& logical);

		// 逻辑路径 → 现场判定 + 徽标 token 表 + 组件状态 token(要读产物头/算源图哈希,
		// 只在真的画组件时用)。
		TextureRefFacts DescribeTextureRef(const std::string& logical);

		// 组件入参的**唯一构造点**。
		//
		// 为什么不让面板自己拼 options:门禁第 2 条按 `Wui::WuiXxx` 的**名字**统计"面板用到但
		// 登记表未覆盖的控件类型",而选项/条目/状态三个结构不是可展示部件、也不进登记表 ——
		// 面板侧出现这些类型名会被误判成未登记控件。数据侧统一在这里拼装,面板只传值/收结果。
		//
		// value = 当前引用(空 = 没有纹理);label = 控件标签(槽位标签 / 参数标签);
		// idPrefix = a11y id 前缀(`<prefix>.state` / `<prefix>.locate` 由组件登记);
		// droppedValue = 本帧从 `Editor::AssetDropBridge` 取到的一次跨窗口投放(空 = 没有)。
		Wui::WuiTexturePickerOptions TexturePickerOptions(const std::string& value, const std::string& label,
			const std::string& idPrefix, const std::string& droppedValue, bool readOnly = false);

		// 给材质"赋纹理"时的唯一入口(P9 口径):选中的若是**源图**而它还没有同主名 `.wtex`,
		// 就当场导入成**单文件容器**资产,并返回资产逻辑路径(已存在 = 原样返回资产,不碰 payload)。
		// outNote 非空时填一句给状态行的人话(导入成功 / 失败原因)。
		std::string NormalizeTextureChoice(const std::string& logical, std::string* outNote);

		// 引用的工具提示句:"资产 → 源图"关系 + 缺失原因(空引用返回空串)。
		std::string TextureRefDoc(const std::string& logical);
		// 行内校验的一句话(缺失 / 资产缺源图;正常返回空串)—— 槽位与参数行共用同一份口径。
		std::string TextureInlineWarning(const std::string& logical);
	}
}
