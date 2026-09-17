#include "wldpch.h"
#include "World/Gameplay/EventBus.h"

#include <algorithm>
#include <cstring>

namespace World::Gameplay
{
	EventBus::Subscription EventBus::SubscribeRaw(uint32_t typeId, Handler handler)
	{
		Entry entry;
		entry.Id = m_NextId++;
		entry.TypeId = typeId;
		entry.Callback = std::move(handler);
		m_Entries.push_back(std::move(entry));
		return { m_Entries.back().Id };
	}

	void EventBus::Unsubscribe(Subscription subscription)
	{
		if (!subscription.IsValid())
			return;
		for (Entry& entry : m_Entries)
		{
			if (entry.Id != subscription.Id)
				continue;
			// 派发中不能立即 erase(迭代器失效):标记删除,派发结束后统一清理。
			entry.Removed = true;
			if (!m_Dispatching)
				m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
					[](const Entry& e) { return e.Removed; }), m_Entries.end());
			return;
		}
	}

	void EventBus::EmitRaw(uint32_t typeId, const void* data, size_t size, uint64_t source)
	{
		EventValue value;
		value.TypeId = typeId;
		value.Source = source;
		if (data && size > 0)
			value.Payload.assign(static_cast<const uint8_t*>(data),
				static_cast<const uint8_t*>(data) + size);

		m_Emitted++;
		const DispatchGuard guard(*this);
		// 按订阅顺序派发;派发期间新增的订阅不会收到本条事件(与常见引擎一致)。
		const size_t count = m_Entries.size();
		for (size_t i = 0; i < count && i < m_Entries.size(); ++i)
		{
			Entry& entry = m_Entries[i];
			if (entry.Removed || entry.TypeId != typeId || !entry.Callback)
				continue;
			// 拷贝 handler:回调里 SubscribeRaw 会让 m_Entries 重新分配,
			// 正在执行的 std::function 不能被移动/析构。
			const Handler callback = entry.Callback;
			InvokeHandlerGuarded(callback, value);
		}
		if (!m_Dispatching)
			m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
				[](const Entry& e) { return e.Removed; }), m_Entries.end());
	}

	void EventBus::EmitDeferredRaw(uint32_t typeId, const void* data, size_t size, uint64_t source)
	{
		EventValue value;
		value.TypeId = typeId;
		value.Source = source;
		if (data && size > 0)
			value.Payload.assign(static_cast<const uint8_t*>(data),
				static_cast<const uint8_t*>(data) + size);
		m_Pending.push_back(std::move(value));
	}

	uint32_t EventBus::DispatchPending()
	{
		if (m_Pending.empty())
			return 0;

		// 取出本批再派发:派发过程中新入队的事件留到下一批(避免同帧无限增长)。
		std::vector<EventValue> batch;
		batch.swap(m_Pending);

		const DispatchGuard guard(*this);
		for (const EventValue& value : batch)
		{
			m_Emitted++;
			const size_t count = m_Entries.size();
			for (size_t i = 0; i < count && i < m_Entries.size(); ++i)
			{
				Entry& entry = m_Entries[i];
				if (entry.Removed || entry.TypeId != value.TypeId || !entry.Callback)
					continue;
				const Handler callback = entry.Callback;
				InvokeHandlerGuarded(callback, value);
			}
		}
		if (!m_Dispatching)
			m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
				[](const Entry& e) { return e.Removed; }), m_Entries.end());
		return static_cast<uint32_t>(batch.size());
	}

	void EventBus::InvokeHandlerGuarded(const Handler& handler, const EventValue& value)
	{
		try
		{
			handler(value);
		}
		catch (const std::exception& error)
		{
			++m_HandlerErrors;
			m_LastHandlerError = error.what();
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[EventBus] handler threw (typeId={0}): {1}", value.TypeId, error.what());
		}
		catch (...)
		{
			++m_HandlerErrors;
			m_LastHandlerError = "unknown exception";
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[EventBus] handler threw an unknown exception (typeId={0})", value.TypeId);
		}
	}

	size_t EventBus::GetSubscriberCount(uint32_t typeId) const
	{
		return static_cast<size_t>(std::count_if(m_Entries.begin(), m_Entries.end(),
			[typeId](const Entry& entry) { return !entry.Removed && entry.TypeId == typeId; }));
	}

	void EventBus::Clear()
	{
		m_Entries.clear();
		m_Pending.clear();
		m_Dispatching = false;
	}

	// ---- TimerService ----

	TimerService::Handle TimerService::After(double seconds, Callback callback)
	{
		Entry entry;
		entry.Id = m_NextId++;
		entry.Interval = std::max(0.0, seconds);
		entry.Remaining = entry.Interval;
		entry.RemainingRepeats = 1;
		entry.Callback = std::move(callback);
		m_Entries.push_back(std::move(entry));
		return { m_Entries.back().Id };
	}

	TimerService::Handle TimerService::Every(double seconds, Callback callback, uint32_t count)
	{
		Entry entry;
		entry.Id = m_NextId++;
		entry.Interval = std::max(0.0, seconds);
		entry.Remaining = entry.Interval;
		entry.RemainingRepeats = count;
		entry.Callback = std::move(callback);
		m_Entries.push_back(std::move(entry));
		return { m_Entries.back().Id };
	}

	void TimerService::Cancel(Handle handle)
	{
		if (!handle.IsValid())
			return;
		for (Entry& entry : m_Entries)
			if (entry.Id == handle.Id)
			{
				entry.Cancelled = true;
				return;
			}
	}

	uint32_t TimerService::Advance(double fixedStepSeconds)
	{
		if (fixedStepSeconds <= 0.0)
			return 0;
		// 浮点边界容差:0.5s @60Hz 的 30 次减法会留下 ~1e-16 的正残差,
		// 严格 <=0 会让触发整体延后一整步(实测 31 步)。容差远小于一个固定步。
		constexpr double kFireTolerance = 1e-9;
		// 回调内再调 Advance 属于宿主用法错误:直接忽略,避免嵌套推进破坏步进语义。
		if (m_Advancing)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_WARN("[TimerService] Advance ignored: callbacks must not advance timers recursively");
			return 0;
		}
		// 即使内部出现非回调异常(极少见的分配失败),也要复位重入标志。
		struct AdvanceGuard
		{
			bool& Flag;
			explicit AdvanceGuard(bool& flag) : Flag(flag) { Flag = true; }
			~AdvanceGuard() { Flag = false; }
			AdvanceGuard(const AdvanceGuard&) = delete;
			AdvanceGuard& operator=(const AdvanceGuard&) = delete;
		} advanceGuard(m_Advancing);

		uint32_t fired = 0;
		// 阶段 1:只推进计时并收集本步到期条目(不回调用户代码,可以安全地按引用遍历)。
		std::vector<uint64_t> due;
		due.reserve(m_Entries.size());
		for (Entry& entry : m_Entries)
		{
			if (entry.Cancelled)
				continue;
			entry.Remaining -= fixedStepSeconds;
			// 单次 Advance 内最多触发一次(步长小于间隔时行为稳定;间隔为 0 时每步触发一次)。
			if (!entry.Callback)
				continue;
			if (entry.Interval > 0.0 && entry.Remaining > kFireTolerance)
				continue;
			due.push_back(entry.Id);
		}

		// 阶段 2:逐条目触发。回调里 After/Every/Cancel/Clear 会改 m_Entries,
		// 所以每次触发前按 Id 重新查找,绝不持有跨回调的 Entry&。
		const auto findEntry = [this](uint64_t id) -> Entry*
		{
			for (Entry& entry : m_Entries)
				if (entry.Id == id)
					return &entry;
			return nullptr;
		};
		for (const uint64_t id : due)
		{
			Entry* entry = findEntry(id);
			if (!entry || entry->Cancelled || !entry->Callback)
				continue;
			// 拷贝回调:回调内 Clear/覆盖条目也不会让正在调用的对象失效。
			const Callback callback = entry->Callback;
			++m_Fired;
			++fired;
			try
			{
				callback();
			}
			catch (const std::exception& error)
			{
				++m_CallbackErrors;
				m_LastCallbackError = error.what();
				if (Log::GetCoreLogger())
					WLD_CORE_ERROR("[TimerService] timer callback threw: {0}", error.what());
			}
			catch (...)
			{
				++m_CallbackErrors;
				m_LastCallbackError = "unknown exception";
				if (Log::GetCoreLogger())
					WLD_CORE_ERROR("[TimerService] timer callback threw an unknown exception");
			}

			// 回调可能已经 Clear()/Cancel() 掉自己或整张表:按 Id 重新解析。
			entry = findEntry(id);
			if (!entry || entry->Cancelled)
				continue;
			if (entry->RemainingRepeats > 0)
			{
				entry->RemainingRepeats--;
				if (entry->RemainingRepeats == 0)
				{
					entry->Cancelled = true;
					continue;
				}
			}
			entry->Remaining += entry->Interval;
			if (entry->Remaining <= -entry->Interval)
				entry->Remaining = entry->Interval;   // 防止长时间挂起后一次性补跑
		}

		m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
			[](const Entry& entry) { return entry.Cancelled; }), m_Entries.end());
		return fired;
	}

	size_t TimerService::GetActiveCount() const
	{
		return static_cast<size_t>(std::count_if(m_Entries.begin(), m_Entries.end(),
			[](const Entry& entry) { return !entry.Cancelled; }));
	}

	void TimerService::Clear()
	{
		m_Entries.clear();
	}
}
