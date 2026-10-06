#include "wldpch.h"
#include "World/UI/UiNodeRegistry.h"

#include <mutex>
#include <utility>

namespace World::UI
{
	namespace
	{
		// `.wui` Type 名的匹配口径:规范名 / 别名 / 组件 id / 组件结构体名 四选一。
		// 目的:文档可以写 "Button"(规范名)、"button"(组件 id)或 "WuiButton"(结构体名),
		// 三者指向同一件登记项 —— 但**不新增第五种命名**。
		bool MatchesName(const UiNodeTypeDesc& type, std::string_view name)
		{
			if (std::string_view(type.Type) == name || std::string_view(type.ComponentId) == name)
				return true;
			for (const std::string& alias : type.Aliases)
			{
				if (std::string_view(alias) == name)
					return true;
			}
			const Wui::WuiComponentDesc* component = Wui::WuiComponentRegistry::Find(type.ComponentId);
			return component != nullptr && std::string_view(component->TypeName) == name;
		}
	}

	std::vector<UiNodeTypeDesc>& UiNodeRegistry::Types()
	{
		static std::vector<UiNodeTypeDesc> types;
		return types;
	}

	namespace
	{
		std::once_flag& BuiltinsOnce()
		{
			static std::once_flag flag;
			return flag;
		}
	}

	void UiNodeRegistry::EnsureBuiltins()
	{
		// 与 WuiComponentRegistry 同口径:首次使用时一次性登记。
		// 注意:RegisterBuiltinUiNodeTypes 内部只调 RegisterType(不再回头调 EnsureBuiltins),
		// 所以不存在 std::call_once 重入死锁。
		std::call_once(BuiltinsOnce(), RegisterBuiltinUiNodeTypes);
	}

	bool UiNodeRegistry::RegisterType(UiNodeTypeDesc desc, std::string* error)
	{
		auto fail = [error](std::string message) {
			if (error)
				*error = std::move(message);
			return false;
		};

		if (desc.Type.empty())
			return fail("UI node type name must not be empty");
		if (desc.ComponentId.empty())
			return fail("UI node type '" + desc.Type + "' must reference an existing WuiComponentRegistry id");
		if (Wui::WuiComponentRegistry::Find(desc.ComponentId) == nullptr)
		{
			return fail("UI node type '" + desc.Type + "' references unregistered component id '" +
				desc.ComponentId + "': register the component in WuiComponentRegistry first "
				"(do not invent a parallel control naming)");
		}
		if (desc.Paint == nullptr)
			return fail("UI node type '" + desc.Type + "' must provide a Paint entry point");

		// 不做 EnsureBuiltins:内置类型由 Find/All 在首次查询时经 std::call_once 登记,
		// 从 RegisterType 里回调会与 call_once 重入。同 Type 重复登记 = 幂等覆盖(后登记者胜)。
		std::vector<UiNodeTypeDesc>& types = Types();
		for (UiNodeTypeDesc& existing : types)
		{
			if (existing.Type == desc.Type)
			{
				existing = std::move(desc);
				return true;
			}
		}
		types.push_back(std::move(desc));
		return true;
	}

	const UiNodeTypeDesc* UiNodeRegistry::Find(std::string_view type)
	{
		EnsureBuiltins();
		for (const UiNodeTypeDesc& candidate : Types())
		{
			if (MatchesName(candidate, type))
				return &candidate;
		}
		return nullptr;
	}

	const std::vector<UiNodeTypeDesc>& UiNodeRegistry::All()
	{
		EnsureBuiltins();
		return Types();
	}

	const Wui::WuiComponentDesc* UiNodeRegistry::Component(std::string_view type)
	{
		const UiNodeTypeDesc* nodeType = Find(type);
		return nodeType != nullptr ? Wui::WuiComponentRegistry::Find(nodeType->ComponentId) : nullptr;
	}

	bool UiNodeRegistry::DeclaresProperty(const UiNodeTypeDesc& type, std::string_view property)
	{
		for (const std::string& extra : type.ExtraProps)
		{
			if (std::string_view(extra) == property)
				return true;
		}
		const Wui::WuiComponentDesc* component = Wui::WuiComponentRegistry::Find(type.ComponentId);
		if (component == nullptr)
			return false;
		for (const Wui::WuiComponentProperty& declared : component->Properties)
		{
			if (std::string_view(declared.Name) == property)
				return true;
		}
		return false;
	}
}
