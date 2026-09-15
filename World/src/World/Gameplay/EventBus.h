#pragma once

#include "World/Core/Export.h"

#include <entt.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace World::Gameplay
{
	// 类型化事件(值盒装):载荷按字节保存,类型由 TypeId 区分。
	// 契约:事件只用于"事实通知",不用于逐帧数据流;派发点固定,顺序确定。
	struct EventValue
	{
		uint32_t TypeId = 0;
		std::vector<uint8_t> Payload;
		uint64_t Source = 0;          // 关联实体句柄(0 = 无)
	};

	class WLD_API EventBus
	{
	public:
		using Handler = std::function<void(const EventValue&)>;

		struct Subscription
		{
			uint64_t Id = 0;
			bool IsValid() const { return Id != 0; }
		};

		// 稳定类型 id:同一类型在同一构建内恒定(entt 的类型哈希)。
		template <typename T>
		static uint32_t TypeIdOf() { return static_cast<uint32_t>(entt::type_hash<T>::value()); }

		template <typename T>
		Subscription Subscribe(std::function<void(const T&)> handler)
		{
			const uint32_t typeId = TypeIdOf<T>();
			return SubscribeRaw(typeId, [fn = std::move(handler)](const EventValue& value)
			{
				if (value.Payload.size() != sizeof(T))
					return;   // 载荷尺寸不匹配:忽略而不是读越界
				T typed;
				std::memcpy(&typed, value.Payload.data(), sizeof(T));
				fn(typed);
			});
		}

		template <typename T>
		void Emit(const T& event, uint64_t source = 0)
		{
			EmitRaw(TypeIdOf<T>(), &event, sizeof(T), source);
		}

		template <typename T>
		void EmitDeferred(const T& event, uint64_t source = 0)
		{
			EmitDeferredRaw(TypeIdOf<T>(), &event, sizeof(T), source);
		}

		Subscription SubscribeRaw(uint32_t typeId, Handler handler);
		void Unsubscribe(Subscription subscription);

		void EmitRaw(uint32_t typeId, const void* data, size_t size, uint64_t source = 0);
		void EmitDeferredRaw(uint32_t typeId, const void* data, size_t size, uint64_t source = 0);
		// 在帧内固定点派发排队事件(由 GameApp::Tick 调用),返回派发条数。
		uint32_t DispatchPending();

		size_t GetSubscriberCount(uint32_t typeId) const;
		size_t GetPendingCount() const { return m_Pending.size(); }
		uint64_t GetEmittedCount() const { return m_Emitted; }
		void Clear();

	private:
		struct Entry
		{
			uint64_t Id = 0;
			uint32_t TypeId = 0;
			bool Removed = false;
			Handler Callback;
		};

		std::vector<Entry> m_Entries;
		std::vector<EventValue> m_Pending;
		uint64_t m_NextId = 1;
		uint64_t m_Emitted = 0;
		bool m_Dispatching = false;
	};

	// 计时器(P2a W6):After 一次性 / Every 重复(可限次数);在固定步长上推进,保证确定性。
	class WLD_API TimerService
	{
	public:
		struct Handle
		{
			uint64_t Id = 0;
			bool IsValid() const { return Id != 0; }
		};

		using Callback = std::function<void()>;

		Handle After(double seconds, Callback callback);
		// count = 0 表示无限重复。
		Handle Every(double seconds, Callback callback, uint32_t count = 0);
		void Cancel(Handle handle);
		// 由固定步长阶段调用;返回本次触发的回调次数。
		uint32_t Advance(double fixedStepSeconds);

		size_t GetActiveCount() const;
		uint64_t GetFiredCount() const { return m_Fired; }
		void Clear();

	private:
		struct Entry
		{
			uint64_t Id = 0;
			double Interval = 0.0;
			double Remaining = 0.0;
			uint32_t RemainingRepeats = 0;   // 0 = 无限
			bool Cancelled = false;
			Callback Callback;
		};

		std::vector<Entry> m_Entries;
		uint64_t m_NextId = 1;
		uint64_t m_Fired = 0;
	};
}
