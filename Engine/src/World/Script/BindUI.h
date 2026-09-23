#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <string>

namespace World
{
	class ScriptBindingContext;
	struct ScriptServiceBinding;

	namespace Wui { class WuiContext; }

	// P2 W3c:UI 面的脚本绑定(只读全局表 `ui`)。
	//
	// 设计要点:
	//   - WUI 是即时模式:脚本在 UI 阶段(OnUI)调用 ui.*,控件立即画进当前 WuiContext;
	//   - 返回值式交互(button/checkbox/slider/list/grid),不做长期 onClick 回调
	//     (UI 阶段晚于 OnUpdate,且热重载会换掉回调引用);
	//   - 每帧由 ScriptEngine::DrawScriptUi 建立一次"当前 UI 上下文 + 脚本逻辑路径前缀",
	//     ui.* 只在这个作用域内可用;id 加脚本路径前缀后 HashId;
	//   - 参数类型/数量错误抛可读异常,由绑定层转成 Lua error;
	//   - 描述表是运行时注册与存根渲染的唯一来源(仿 BindServices)。
	WLD_API bool RegisterUiBindings(ScriptBindingContext& bindings, std::string* error = nullptr);
	WLD_API const ScriptServiceBinding* ScriptUiBindings(std::size_t* count);

	// ScriptEngine::DrawScriptUi 使用的 RAII 作用域:进入时设置当前 WuiContext 与
	// 脚本逻辑路径(用于给控件 id 加前缀),离开时恢复上一层。嵌套调用按栈恢复。
	WLD_API void PushScriptUiContext(Wui::WuiContext& context, const char* idPrefix);
	WLD_API void PopScriptUiContext();

	class ScriptUiScope
	{
	public:
		ScriptUiScope(Wui::WuiContext& context, const std::string& idPrefix)
		{
			PushScriptUiContext(context, idPrefix.c_str());
		}
		~ScriptUiScope() { PopScriptUiContext(); }

		ScriptUiScope(const ScriptUiScope&) = delete;
		ScriptUiScope& operator=(const ScriptUiScope&) = delete;
		ScriptUiScope(ScriptUiScope&&) = delete;
		ScriptUiScope& operator=(ScriptUiScope&&) = delete;
	};
}
