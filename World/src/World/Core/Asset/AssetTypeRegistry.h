#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace World
{
	// P4-UX16:编辑器"新建资产"的类型注册表。
	//
	// 起因(用户 2026-09-20):「新建材质这类操作需要重新设计 —— 不能每加一个类似的资产就新增一个按钮」。
	// 目标形态:**新增一种资产类型 = 一次注册(数据)**,而不是"再加一个按钮 / 再加一条 if 分支"。
	// 菜单项、扩展名提示、排序、图标全部从这张表派生,内容浏览器的 UI 代码零改动。
	//
	// 生命周期约定(重要):`Create` 回调由注册方持有(通常是编辑器面板,捕获 this)。
	// 注册方必须在析构时 `Unregister` 自己注册过的**全部** id,否则回调会在面板销毁后悬空。
	struct AssetTypeDesc
	{
		// 稳定 id("folder" / "material" / "scene" / "script" / …)。
		// 同时是本地化键 `asset.type.<Id>` 与无障碍节点 id(`browser.…new.<Id>`)的组成部分 ——
		// 改名等于改对外契约,新增类型不要复用别人的 id。
		std::string Id;
		// 内联英文默认值(本地化键 asset.type.<Id>);中文名进 Editor/assets/localization/zh-CN.json。
		std::string Label;
		// 术语/别名(默认与 Label 相同):语言切换或歧义对照时用。
		std::string Term;
		// 默认扩展名(".wmat" / ".wd" / ".luau");文件夹为空。
		// 菜单右侧的扩展名提示、重命名冲突检测、另存对话框共用同一份。
		std::string Extension;
		// 图标纹理 id(与内容区切片同一套);0 = 无图标(菜单只用文字 + 扩展名)。
		uint64_t Icon = 0;
		// 菜单排序:文件夹恒排第一,其余按 SortOrder 升序,同序按 Id 字典序(确定性)。
		int SortOrder = 100;
		// 文件夹语义:菜单恒排第一,且不显示扩展名。
		bool IsFolder = false;
		// 真正创建资产。dir = 目标目录(绝对路径,调用前保证已存在);
		// 成功 = 至少落一份磁盘产物(失败必须不留半成品)。失败时把可读原因写进 error。
		std::function<bool(const std::filesystem::path& dir, std::string* error)> Create;
	};

	// 进程内唯一的一张表。实现放在 .cpp 里,符号随 WorldRuntime.dll 导出 ——
	// 保证 Editor 与 World 侧读到的是**同一个实例**(头文件里放 inline 单例会各自生成副本)。
	class AssetTypeRegistry
	{
	public:
		static AssetTypeRegistry& Get();

		// 同 Id 二次注册 = 覆盖(测试与热更新友好);Id 为空 = 直接忽略。
		void Register(AssetTypeDesc desc);
		bool Unregister(const std::string& id);
		void Clear();
		size_t Size() const { return m_Types.size(); }
		// 注册顺序的表(调用方几乎总是想用 Sorted())。
		const std::vector<AssetTypeDesc>& All() const { return m_Types; }
		// 菜单顺序:文件夹恒第一 → SortOrder 升序 → Id 字典序。返回副本,调用方可自由持有。
		std::vector<AssetTypeDesc> Sorted() const;
		const AssetTypeDesc* Find(const std::string& id) const;

	private:
		std::vector<AssetTypeDesc> m_Types;   // 注册顺序;菜单顺序见 Sorted()
	};
}
