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
		const bool wasDispatching = m_Dispatching;
		m_Dispatching = true;
		// 按订阅顺序派发;派发期间新增的订阅不会收到本条事件(与常见引擎一致)。
		const size_t count = m_Entries.size();
		for (size_t i = 0; i < count && i < m_Entries.size(); ++i)
		{
			Entry& entry = m_Entries[i];
			if (entry.Removed || entry.TypeId != typeId || !entry.Callback)
				continue;
			entry.Callback(value);
		}
		m_Dispatching = wasDispatching;
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

		const bool wasDispatching = m_Dispatching;
		m_Dispatching = true;
		for (const EventValue& value : batch)
		{
			m_Emitted++;
			const size_t count = m_Entries.size();
			for (size_t i = 0; i < count && i < m_Entries.size(); ++i)
			{
				Entry& entry = m_Entries[i];
				if (entry.Removed || entry.TypeId != value.TypeId || !entry.Callback)
					continue;
				entry.Callback(value);
			}
		}
		m_Dispatching = wasDispatching;
		if (!m_Dispatching)
			m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(),
				[](const Entry& e) { return e.Removed; }), m_Entries.end());
		return static_cast<uint32_t>(batch.size());
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

		uint32_t fired = 0;
		for (Entry& entry : m_Entries)
		{
			if (entry.Cancelled)
				continue;
			entry.Remaining -= fixedStepSeconds;
			// 单次 Advance 内最多触发一次(步长小于间隔时行为稳定;间隔为 0 时每步触发一次)。
			if (entry.Interval > 0.0 && entry.Remaining > 0.0)
				continue;
			if (!entry.Callback)
				continue;

			entry.Callback();
			m_Fired++;
			fired++;
			if (entry.RemainingRepeats > 0)
			{
				entry.RemainingRepeats--;
				if (entry.RemainingRepeats == 0)
					entry.Cancelled = true;
			}
			entry.Remaining += entry.Interval;
			if (entry.Remaining <= -entry.Interval)
				entry.Remaining = entry.Interval;   // 防止长时间挂起后一次性补跑
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
