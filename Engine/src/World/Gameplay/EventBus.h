#pragma once

#include "World/Core/Export.h"

#include <entt.hpp>

#include <cstring>
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

		// 派发隔离(2026-09-17 冻结,供 Lua 事件桥依赖):
		//   - handler 抛出的异常不逃出 Emit*/DispatchPending,总线在异常后仍可用;
		//   - 一条 batch 里某个 handler 抛异常,不会丢掉本批剩余事件或后续订阅者;
		//   - 失败逐次记录在这里,便于宿主/测试断言(不做线程安全承诺)。
		uint64_t GetHandlerErrorCount() const { return m_HandlerErrors; }
		const std::string& GetLastHandlerError() const { return m_LastHandlerError; }

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

		// 派发标志的 RAII 守卫:handler 抛异常时也要复位 m_Dispatching,
		// 否则一次异常会让总线永久处于"派发中"(退订只标墓碑不清理)。
		class DispatchGuard
		{
		public:
			explicit DispatchGuard(EventBus& bus) : m_Bus(&bus), m_Previous(bus.m_Dispatching) { bus.m_Dispatching = true; }
			~DispatchGuard() { m_Bus->m_Dispatching = m_Previous; }
			DispatchGuard(const DispatchGuard&) = delete;
			DispatchGuard& operator=(const DispatchGuard&) = delete;
		private:
			EventBus* m_Bus = nullptr;
			bool m_Previous = false;
		};

		// 逐 handler 的受保护调用:异常转成诊断记录,不打断本批剩余派发。
		// handler 按值传入:派发中新增订阅会让 m_Entries 重新分配,不能移动正在执行的 std::function。
		void InvokeHandlerGuarded(const Handler& handler, const EventValue& value);

		std::vector<Entry> m_Entries;
		std::vector<EventValue> m_Pending;
		uint64_t m_NextId = 1;
		uint64_t m_Emitted = 0;
		uint64_t m_HandlerErrors = 0;
		std::string m_LastHandlerError;
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
		// 由固定步长阶段调用;返回本次触发的回调次数(抛异常的回调也算"已触发")。
		// 契约(2026-09-17 冻结,供 Lua 计时器桥依赖):
		//   - 回调内 After/Every/Cancel/Clear 安全:两阶段派发,回调期间不持有 vector 元素引用;
		//   - 单个回调抛异常只记录(GetCallbackErrorCount/GetLastCallbackError)并继续,
		//     异常不逃出 Advance;回调内新增的计时器从下一个固定步开始计时;
		//   - 回调内 Cancel/Clear 掉的计时器不再收到本步的后续触发。
		uint32_t Advance(double fixedStepSeconds);

		size_t GetActiveCount() const;
		uint64_t GetFiredCount() const { return m_Fired; }
		uint64_t GetCallbackErrorCount() const { return m_CallbackErrors; }
		const std::string& GetLastCallbackError() const { return m_LastCallbackError; }
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
		uint64_t m_CallbackErrors = 0;
		std::string m_LastCallbackError;
		bool m_Advancing = false;
	};
}
