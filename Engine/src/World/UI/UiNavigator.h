#pragma once

// 游戏 UI 框架(GameUI)— 页面栈 / 模态层 / 层序 / 注册制转场(工作包 M3)。
//
// 契约:`contract.ui-runtime` §4(导航)与 §5(输入消费由 `UiInputRouter` 承担)。
// 边界(硬):
//   * 纯状态机 + 层序:不持有输入、不做布局、不碰渲染、不写 ECS、不改文档;
//   * 一页 = 调用方已加载/已布局的 `UiScreen`(本类只存指针,不复制、不拥有);
//   * 转场是注册制的**纯函数**(归一化进度 → 视觉权重),时间由调用方驱动。
//
// 层序固定(低 → 高):Page < Modal < Overlay < Debug;`Layers()` 按此顺序给绘制方。
//
// 与 `UiScreen` 的关系:`UiScreen` 回答"一页怎么画/怎么命中",`UiNavigator` 回答
// "现在该画哪一页、哪一层";两者都不持有输入。

#include "World/Core/Export.h"
#include "World/UI/UiScreen.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	// 固定层序(数值即绘制优先级:大的后画)。
	enum class UiLayer : uint8_t
	{
		Page = 0,
		Modal = 1,
		Overlay = 2,
		Debug = 3,
	};

	inline constexpr int kUiLayerCount = 4;

	// 内置转场名(注册表项,不是特殊分支)。
	inline constexpr const char* kUiTransitionNone = "None";
	inline constexpr const char* kUiTransitionFade = "Fade";

	WLD_API const char* UiLayerName(UiLayer layer);

	// 一页:名字 + 已加载屏幕。Modal/Overlay/Debug 层复用同一结构。
	struct UiPage
	{
		std::string Name;                 // 稳定页名(空 = 取 document.Screen)
		const UiScreen* Screen = nullptr; // 调用方持有,必须在本页出栈前保持有效
	};

	// 从屏幕推导一页(Name 默认取 `UiDocument::Screen`)。
	WLD_API UiPage MakeUiPage(const UiScreen& screen, std::string name = std::string());

	// 绘制顺序里的一项(层 + 层内下标 + 页)。
	struct UiDrawItem
	{
		UiLayer Layer = UiLayer::Page;
		std::size_t Index = 0;
		const UiPage* Page = nullptr;
	};

	// 页面生命周期回调(调用方注册;默认全空 = 无操作)。
	struct UiPageCallbacks
	{
		std::function<void(const UiPage&)> OnEnter;
		std::function<void(const UiPage&)> OnExit;
		std::function<void(const UiPage&)> OnPause;
		std::function<void(const UiPage&)> OnResume;
	};

	// 转场采样:归一化进度 → 出场/入场页的透明度权重(纯数据,绘制方消费)。
	struct UiTransitionSample
	{
		float FromAlpha = 0.0f;
		float ToAlpha = 1.0f;
	};

	using UiTransitionFn = void (*)(float progress, UiTransitionSample& out);

	class WLD_API UiNavigator
	{
	public:
		// ---- 页面栈(Page 层;进/出栈驱动生命周期)----
		bool Push(UiPage page);      // 旧顶 OnPause → 新页 OnEnter
		bool Pop();                  // 顶 OnExit → 新顶 OnResume;栈空 = false
		bool Replace(UiPage page);   // 顶 OnExit → 新页 OnEnter;栈空 = 等价 Push
		void Reset();                // 全部层逐层 OnExit(顶→底)后清空
		bool Reset(UiPage page);     // Reset + 推入唯一页(OnEnter);硬切换,不做转场

		std::size_t Count() const;   // 页面栈深度(不含模态)
		bool Empty() const;
		const UiPage* Top() const;
		const std::vector<UiPage>& Stack() const;

		// ---- 模态(独立层,不占栈;关掉回原页)----
		bool PushModal(UiPage page);
		bool PopModal();
		std::size_t ModalCount() const;
		const UiPage* TopModal() const;
		const std::vector<UiPage>& Modals() const;

		// ---- 覆盖层 / 调试层(同"独立层"口径;Page/Modal 走专用入口,本接口拒绝)----
		bool PushLayer(UiLayer layer, UiPage page);
		bool PopLayer(UiLayer layer);
		const std::vector<UiPage>& LayerPages(UiLayer layer) const;

		// ---- 层序 + 返回键路由 ----
		std::vector<UiDrawItem> Layers() const;   // Page(底→顶) < Modal < Overlay < Debug
		bool Back();                              // 有模态先关模态,否则出栈

		// ---- 输入路由辅助(给 UiInputRouter;本类不持有输入)----
		UiLayer TopLayer() const;                      // 最高非空层;全空 = Page
		const UiScreen* TopInteractiveScreen() const;  // 最高非空层的顶页屏幕

		// ---- 转场(注册制;时间由调用方驱动)----
		static bool RegisterTransition(std::string_view name, UiTransitionFn fn);
		static UiTransitionFn FindTransition(std::string_view name);
		bool SetTransition(std::string_view name);      // 未注册名 = false,保持当前
		const std::string& TransitionName() const;
		bool TransitionActive() const;
		void SetTransitionProgress(float normalized);   // [0,1];>= 1 结束过渡
		float TransitionProgress() const;
		UiTransitionSample SampleTransition() const;    // 非活动 = 0/1(稳态)
		const std::string& TransitionFromName() const;

		// ---- 页面生命周期回调 ----
		void SetPageCallbacks(UiPageCallbacks callbacks);
		const UiPageCallbacks& PageCallbacks() const;

	private:
		void BeginTransition(std::string fromName);
		void ExitAllLayers();

		UiPageCallbacks m_Callbacks;
		std::vector<UiPage> m_Layers[kUiLayerCount];   // 下标 = UiLayer
		std::string m_TransitionName = kUiTransitionNone;
		std::string m_TransitionFromName;
		float m_TransitionProgress = 0.0f;
		bool m_TransitionActive = false;
	};
}
