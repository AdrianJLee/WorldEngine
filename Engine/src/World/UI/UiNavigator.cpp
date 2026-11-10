#include "wldpch.h"
#include "World/UI/UiNavigator.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace World::UI
{
	namespace
	{
		const std::vector<UiPage>& EmptyPages()
		{
			static const std::vector<UiPage> empty;
			return empty;
		}

		// 内置转场:
		//   None = 硬切换(出场立即不可见、入场全亮);
		//   Fade = 交叉淡入淡出(进度由调用方按时间驱动)。
		void NoneTransition(float, UiTransitionSample& out)
		{
			out.FromAlpha = 0.0f;
			out.ToAlpha = 1.0f;
		}

		void FadeTransition(float progress, UiTransitionSample& out)
		{
			const float t = std::clamp(progress, 0.0f, 1.0f);
			out.FromAlpha = 1.0f - t;
			out.ToAlpha = t;
		}

		using TransitionEntry = std::pair<std::string, UiTransitionFn>;

		std::vector<TransitionEntry>& TransitionTable()
		{
			static std::vector<TransitionEntry> table;
			return table;
		}

		std::once_flag& TransitionOnce()
		{
			static std::once_flag flag;
			return flag;
		}

		void EnsureBuiltinTransitions()
		{
			std::call_once(TransitionOnce(), [] {
				TransitionTable().push_back(TransitionEntry { kUiTransitionNone, &NoneTransition });
				TransitionTable().push_back(TransitionEntry { kUiTransitionFade, &FadeTransition });
			});
		}

		void InvokeCallback(const std::function<void(const UiPage&)>& callback, const UiPage& page)
		{
			if (callback)
				callback(page);
		}

		// 页面必须有屏幕;Name 缺失时取 `UiDocument::Screen`。
		bool FillPageName(UiPage& page)
		{
			if (page.Screen == nullptr)
				return false;
			if (page.Name.empty())
				page.Name = page.Screen->Document().Screen;
			return true;
		}
	}

	const char* UiLayerName(UiLayer layer)
	{
		switch (layer)
		{
		case UiLayer::Page: return "Page";
		case UiLayer::Modal: return "Modal";
		case UiLayer::Overlay: return "Overlay";
		case UiLayer::Debug: return "Debug";
		}
		return "Unknown";
	}

	UiPage MakeUiPage(const UiScreen& screen, std::string name)
	{
		UiPage page;
		if (name.empty())
			name = screen.Document().Screen;
		page.Name = std::move(name);
		page.Screen = &screen;
		return page;
	}

	// ---- 页面栈(Page 层)----

	bool UiNavigator::Push(UiPage page)
	{
		if (!FillPageName(page))
			return false;
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(UiLayer::Page)];
		std::string from;
		if (!pages.empty())
		{
			from = pages.back().Name;
			InvokeCallback(m_Callbacks.OnPause, pages.back());
		}
		pages.push_back(std::move(page));
		InvokeCallback(m_Callbacks.OnEnter, pages.back());
		BeginTransition(std::move(from));
		return true;
	}

	bool UiNavigator::Pop()
	{
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(UiLayer::Page)];
		if (pages.empty())
			return false;
		const std::string from = pages.back().Name;
		InvokeCallback(m_Callbacks.OnExit, pages.back());
		pages.pop_back();
		if (!pages.empty())
			InvokeCallback(m_Callbacks.OnResume, pages.back());
		BeginTransition(from);
		return true;
	}

	bool UiNavigator::Replace(UiPage page)
	{
		if (!FillPageName(page))
			return false;
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(UiLayer::Page)];
		if (pages.empty())
		{
			pages.push_back(std::move(page));
			InvokeCallback(m_Callbacks.OnEnter, pages.back());
			BeginTransition(std::string());
			return true;
		}
		const std::string from = pages.back().Name;
		InvokeCallback(m_Callbacks.OnExit, pages.back());
		pages.back() = std::move(page);
		InvokeCallback(m_Callbacks.OnEnter, pages.back());
		BeginTransition(from);
		return true;
	}

	void UiNavigator::Reset()
	{
		ExitAllLayers();
		BeginTransition(std::string());
	}

	bool UiNavigator::Reset(UiPage page)
	{
		if (!FillPageName(page))
			return false;
		ExitAllLayers();
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(UiLayer::Page)];
		pages.push_back(std::move(page));
		InvokeCallback(m_Callbacks.OnEnter, pages.back());
		BeginTransition(std::string());   // Reset = 硬切换,不做转场
		return true;
	}

	std::size_t UiNavigator::Count() const
	{
		return m_Layers[static_cast<std::size_t>(UiLayer::Page)].size();
	}

	bool UiNavigator::Empty() const
	{
		return m_Layers[static_cast<std::size_t>(UiLayer::Page)].empty();
	}

	const UiPage* UiNavigator::Top() const
	{
		const std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(UiLayer::Page)];
		return pages.empty() ? nullptr : &pages.back();
	}

	const std::vector<UiPage>& UiNavigator::Stack() const
	{
		return m_Layers[static_cast<std::size_t>(UiLayer::Page)];
	}

	// ---- 模态(独立层)----

	bool UiNavigator::PushModal(UiPage page)
	{
		if (!FillPageName(page))
			return false;
		{
			// 转场"出场页" = 压入前的最高层顶页(页面或旧模态)。
			const UiLayer top = TopLayer();
			const std::vector<UiPage>& topPages = LayerPages(top);
			BeginTransition(topPages.empty() ? std::string() : topPages.back().Name);
		}
		std::vector<UiPage>& modals = m_Layers[static_cast<std::size_t>(UiLayer::Modal)];
		modals.push_back(std::move(page));
		InvokeCallback(m_Callbacks.OnEnter, modals.back());
		return true;
	}

	bool UiNavigator::PopModal()
	{
		std::vector<UiPage>& modals = m_Layers[static_cast<std::size_t>(UiLayer::Modal)];
		if (modals.empty())
			return false;
		const std::string from = modals.back().Name;
		InvokeCallback(m_Callbacks.OnExit, modals.back());
		modals.pop_back();
		BeginTransition(from);
		return true;
	}

	std::size_t UiNavigator::ModalCount() const
	{
		return m_Layers[static_cast<std::size_t>(UiLayer::Modal)].size();
	}

	const UiPage* UiNavigator::TopModal() const
	{
		const std::vector<UiPage>& modals = m_Layers[static_cast<std::size_t>(UiLayer::Modal)];
		return modals.empty() ? nullptr : &modals.back();
	}

	const std::vector<UiPage>& UiNavigator::Modals() const
	{
		return m_Layers[static_cast<std::size_t>(UiLayer::Modal)];
	}

	// ---- 覆盖层 / 调试层 ----

	bool UiNavigator::PushLayer(UiLayer layer, UiPage page)
	{
		if (layer != UiLayer::Overlay && layer != UiLayer::Debug)
			return false;   // Page/Modal 有专用入口(生命周期语义不同)
		if (!FillPageName(page))
			return false;
		{
			const UiLayer top = TopLayer();
			const std::vector<UiPage>& topPages = LayerPages(top);
			BeginTransition(topPages.empty() ? std::string() : topPages.back().Name);
		}
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(layer)];
		pages.push_back(std::move(page));
		InvokeCallback(m_Callbacks.OnEnter, pages.back());
		return true;
	}

	bool UiNavigator::PopLayer(UiLayer layer)
	{
		if (layer != UiLayer::Overlay && layer != UiLayer::Debug)
			return false;
		std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(layer)];
		if (pages.empty())
			return false;
		const std::string from = pages.back().Name;
		InvokeCallback(m_Callbacks.OnExit, pages.back());
		pages.pop_back();
		BeginTransition(from);
		return true;
	}

	const std::vector<UiPage>& UiNavigator::LayerPages(UiLayer layer) const
	{
		const std::size_t index = static_cast<std::size_t>(layer);
		if (index >= kUiLayerCount)
			return EmptyPages();
		return m_Layers[index];
	}

	// ---- 层序 + 返回键 ----

	std::vector<UiDrawItem> UiNavigator::Layers() const
	{
		std::vector<UiDrawItem> items;
		for (int layer = 0; layer < kUiLayerCount; ++layer)
		{
			const std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(layer)];
			for (std::size_t i = 0; i < pages.size(); ++i)
				items.push_back(UiDrawItem { static_cast<UiLayer>(layer), i, &pages[i] });
		}
		return items;
	}

	bool UiNavigator::Back()
	{
		if (!m_Layers[static_cast<std::size_t>(UiLayer::Modal)].empty())
			return PopModal();
		if (!m_Layers[static_cast<std::size_t>(UiLayer::Page)].empty())
			return Pop();
		return false;
	}

	UiLayer UiNavigator::TopLayer() const
	{
		for (int layer = kUiLayerCount - 1; layer >= 0; --layer)
		{
			if (!m_Layers[static_cast<std::size_t>(layer)].empty())
				return static_cast<UiLayer>(layer);
		}
		return UiLayer::Page;
	}

	const UiScreen* UiNavigator::TopInteractiveScreen() const
	{
		for (int layer = kUiLayerCount - 1; layer >= 0; --layer)
		{
			const std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(layer)];
			if (!pages.empty())
				return pages.back().Screen;
		}
		return nullptr;
	}

	// ---- 转场 ----

	bool UiNavigator::RegisterTransition(std::string_view name, UiTransitionFn fn)
	{
		if (name.empty() || fn == nullptr)
			return false;
		EnsureBuiltinTransitions();
		std::vector<TransitionEntry>& table = TransitionTable();
		for (TransitionEntry& entry : table)
		{
			if (entry.first == name)
			{
				entry.second = fn;
				return true;
			}
		}
		table.push_back(TransitionEntry { std::string(name), fn });
		return true;
	}

	UiTransitionFn UiNavigator::FindTransition(std::string_view name)
	{
		EnsureBuiltinTransitions();
		for (const TransitionEntry& entry : TransitionTable())
		{
			if (entry.first == name)
				return entry.second;
		}
		return nullptr;
	}

	bool UiNavigator::SetTransition(std::string_view name)
	{
		if (FindTransition(name) == nullptr)
			return false;
		m_TransitionName = std::string(name);
		return true;
	}

	const std::string& UiNavigator::TransitionName() const
	{
		return m_TransitionName;
	}

	bool UiNavigator::TransitionActive() const
	{
		return m_TransitionActive;
	}

	void UiNavigator::SetTransitionProgress(float normalized)
	{
		m_TransitionProgress = std::clamp(normalized, 0.0f, 1.0f);
		if (m_TransitionProgress >= 1.0f)
			m_TransitionActive = false;
	}

	float UiNavigator::TransitionProgress() const
	{
		return m_TransitionProgress;
	}

	UiTransitionSample UiNavigator::SampleTransition() const
	{
		UiTransitionSample sample;
		if (!m_TransitionActive)
			return sample;   // 稳态:出场不可见、入场全亮
		const UiTransitionFn fn = FindTransition(m_TransitionName);
		if (fn == nullptr)
			return sample;
		fn(m_TransitionProgress, sample);
		return sample;
	}

	const std::string& UiNavigator::TransitionFromName() const
	{
		return m_TransitionFromName;
	}

	// ---- 生命周期回调 ----

	void UiNavigator::SetPageCallbacks(UiPageCallbacks callbacks)
	{
		m_Callbacks = std::move(callbacks);
	}

	const UiPageCallbacks& UiNavigator::PageCallbacks() const
	{
		return m_Callbacks;
	}

	// ---- 内部 ----

	void UiNavigator::BeginTransition(std::string fromName)
	{
		m_TransitionFromName = std::move(fromName);
		m_TransitionProgress = 0.0f;
		const UiTransitionFn fn = FindTransition(m_TransitionName);
		// None 是硬切换:不进入活动态(即使注册表被换成自定义 "None" 也按同一口径)。
		m_TransitionActive = fn != nullptr && fn != &NoneTransition && !m_TransitionFromName.empty();
	}

	void UiNavigator::ExitAllLayers()
	{
		for (int layer = kUiLayerCount - 1; layer >= 0; --layer)
		{
			std::vector<UiPage>& pages = m_Layers[static_cast<std::size_t>(layer)];
			for (auto it = pages.rbegin(); it != pages.rend(); ++it)
				InvokeCallback(m_Callbacks.OnExit, *it);
			pages.clear();
		}
	}
}
