#include "wldpch.h"
#include "World/Script/BindEvents.h"

#include "World/Core/Log.h"
#include "World/Gameplay/EventBus.h"
#include "World/Gameplay/GameApp.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/BindServices.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace World
{
	namespace
	{
		// ---- 事件参数盒装的容量上限(脚本输入不可信,序列化前先设闸) ----
		constexpr std::size_t kMaxEventValues = 32;
		constexpr std::size_t kMaxEventPayloadBytes = 8 * 1024;
		constexpr std::size_t kMaxEventStringBytes = 1024;
		constexpr std::size_t kMaxSignatureArguments = 32;

		// ---- 事件名目录 ----

		struct EventNameEntry
		{
			std::string Name;
			std::string Signature;
			std::vector<ScriptEventValueKind> ArgumentKinds;
			std::uint32_t TypeId = 0;
		};

		std::vector<EventNameEntry>& EventNames()
		{
			static std::vector<EventNameEntry> entries;
			return entries;
		}

		std::uint32_t Fnv1a32(const std::string& text)
		{
			std::uint32_t hash = 2166136261u;
			for (const unsigned char character : text)
				hash = (hash ^ character) * 16777619u;
			return hash;
		}

		std::string TrimSignatureToken(const std::string& token)
		{
			const std::size_t start = token.find_first_not_of(" \t");
			if (start == std::string::npos)
				return {};
			const std::size_t end = token.find_last_not_of(" \t");
			return token.substr(start, end - start + 1);
		}

		bool ParseArgumentSignature(const std::string& signature, std::vector<ScriptEventValueKind>* out,
			std::string* error)
		{
			out->clear();
			std::size_t position = 0;
			while (position <= signature.size())
			{
				const std::size_t comma = signature.find(',', position);
				const std::string token = TrimSignatureToken(
					signature.substr(position, comma == std::string::npos ? std::string::npos : comma - position));
				if (!token.empty())
				{
					if (out->size() >= kMaxSignatureArguments)
					{
						if (error) *error = "event argument signature has more than " +
							std::to_string(kMaxSignatureArguments) + " arguments";
						return false;
					}
					if (token == "number" || token == "integer" || token == "float")
						out->push_back(ScriptEventValueKind::Number);
					else if (token == "boolean" || token == "bool")
						out->push_back(ScriptEventValueKind::Boolean);
					else if (token == "string")
						out->push_back(ScriptEventValueKind::String);
					else if (token == "Entity")
						out->push_back(ScriptEventValueKind::Entity);
					else
					{
						if (error) *error = "unknown event argument type '" + token +
							"' (expected number, integer, boolean, string or Entity)";
						return false;
					}
				}
				if (comma == std::string::npos)
					break;
				position = comma + 1;
				if (position == signature.size() && signature.back() == ',')
					break;
			}
			return true;
		}

		const EventNameEntry* FindEventName(const char* name)
		{
			if (!name || !name[0])
				return nullptr;
			for (const EventNameEntry& entry : EventNames())
				if (entry.Name == name)
					return &entry;
			return nullptr;
		}

		const EventNameEntry* FindEventNameById(std::uint32_t typeId)
		{
			for (const EventNameEntry& entry : EventNames())
				if (entry.TypeId == typeId)
					return &entry;
			return nullptr;
		}

		// ---- 载荷序列化(同一进程内往返;小端按本机字节序 memcpy) ----

		void AppendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size)
		{
			const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
			out.insert(out.end(), bytes, bytes + size);
		}

		template <typename T>
		void AppendValue(std::vector<std::uint8_t>& out, const T& value)
		{
			AppendBytes(out, &value, sizeof(T));
		}

		bool ReadBytes(const std::vector<std::uint8_t>& payload, std::size_t& cursor, void* out, std::size_t size)
		{
			if (cursor + size > payload.size())
				return false;
			std::memcpy(out, payload.data() + cursor, size);
			cursor += size;
			return true;
		}

		template <typename T>
		bool ReadValue(const std::vector<std::uint8_t>& payload, std::size_t& cursor, T* out)
		{
			return ReadBytes(payload, cursor, out, sizeof(T));
		}

		// ---- 桥状态 ----

		struct EventSubscription
		{
			std::uint64_t Id = 0;
			std::string Name;
			std::uint32_t TypeId = 0;
			// owner =(场景令牌 + 实体句柄含版本,组件 id,实例 generation)
			Entity OwnerEntity;
			Scene* OwnerScene = nullptr;
			std::uint64_t OwnerComponent = 0;
			std::uint64_t OwnerGeneration = 0;
			// 事件订阅与计时器订阅共用一张表:两者都要按 owner 批量退订。
			bool IsTimer = false;
			Gameplay::TimerService::Handle TimerHandle;
			// 订阅建立时的会话身份:会话重建后旧订阅全部作废(总线/计时器服务已销毁)。
			std::uint64_t SessionId = 0;
			ScriptFunctionRef Callback;
		};

		struct EventBridgeState
		{
			std::vector<EventSubscription> Subscriptions;
			std::vector<ScriptEventOwner> OwnerStack;
			std::uint64_t NextSubscriptionId = 1;
			Gameplay::EventBus* BoundBus = nullptr;
			std::uint64_t BoundSessionId = 0;
			// 每个事件名一条总线级分发订阅(惰性建立;桥层再按订阅表逐 handler 调用)。
			std::unordered_map<std::uint32_t, Gameplay::EventBus::Subscription> NameSubscriptions;
		};

		EventBridgeState& Bridge()
		{
			static EventBridgeState state;
			return state;
		}

		EventSubscription* FindSubscription(std::uint64_t id)
		{
			for (EventSubscription& subscription : Bridge().Subscriptions)
				if (subscription.Id == id)
					return &subscription;
			return nullptr;
		}

		bool SameOwner(const EventSubscription& subscription, const Entity& entity, std::uint64_t generation)
		{
			return subscription.OwnerGeneration == generation && subscription.OwnerEntity == entity;
		}

		void CancelTimerIfAlive(const EventSubscription& subscription)
		{
			if (!subscription.IsTimer || !subscription.TimerHandle.IsValid())
				return;
			Gameplay::GameApp* app = Gameplay::GameApp::TryGet();
			if (app && app->SessionId() == subscription.SessionId)
				app->Timers().Cancel(subscription.TimerHandle);
		}

		// 退订某个实例的全部订阅(销毁/热重载/回调失败共用);返回退订条数。
		std::size_t DropOwnerSubscriptions(const Entity& entity, std::uint64_t generation)
		{
			EventBridgeState& bridge = Bridge();
			std::size_t removed = 0;
			for (EventSubscription& subscription : bridge.Subscriptions)
			{
				if (!SameOwner(subscription, entity, generation))
					continue;
				// 计时器订阅先取消,避免 owner 已走后回调仍按步触发。
					CancelTimerIfAlive(subscription);
				++removed;
			}
			if (removed == 0)
				return 0;
			bridge.Subscriptions.erase(
				std::remove_if(bridge.Subscriptions.begin(), bridge.Subscriptions.end(),
					[&](const EventSubscription& subscription)
					{ return SameOwner(subscription, entity, generation); }),
				bridge.Subscriptions.end());
			return removed;
		}

		// 会话重建:旧总线/旧计时器服务已经销毁,直接丢弃全部订阅(引用会被安全释放)。
		void RebindSession(Gameplay::GameApp& app)
		{
			EventBridgeState& bridge = Bridge();
			bridge.NameSubscriptions.clear();
			bridge.Subscriptions.clear();
			bridge.BoundBus = &app.Events();
			bridge.BoundSessionId = app.SessionId();
		}

		Gameplay::EventBus& RequireEventBus()
		{
			Gameplay::GameApp* app = Gameplay::GameApp::TryGet();
			if (!app)
				throw std::logic_error(
					"events require an active GameApp session (Play/Runtime); there is no session yet");
			EventBridgeState& bridge = Bridge();
			if (bridge.BoundBus != &app->Events() || bridge.BoundSessionId != app->SessionId())
				RebindSession(*app);
			return *bridge.BoundBus;
		}

		Gameplay::TimerService& RequireTimerService()
		{
			Gameplay::GameApp* app = Gameplay::GameApp::TryGet();
			if (!app)
				throw std::logic_error(
					"timers require an active GameApp session (Play/Runtime); there is no session yet");
			EventBridgeState& bridge = Bridge();
			if (bridge.BoundBus != &app->Events() || bridge.BoundSessionId != app->SessionId())
				RebindSession(*app);
			return app->Timers();
		}

		void DispatchBusEvent(std::uint32_t typeId, const Gameplay::EventValue& value);

		void SubscribeNameOnBus(std::uint32_t typeId)
		{
			EventBridgeState& bridge = Bridge();
			if (!bridge.BoundBus || bridge.NameSubscriptions.count(typeId))
				return;
			bridge.NameSubscriptions.emplace(typeId, bridge.BoundBus->SubscribeRaw(typeId,
				[typeId](const Gameplay::EventValue& value) { DispatchBusEvent(typeId, value); }));
		}

		// ---- Lua 值转换 ----

		ScriptValue BoxEntityForScene(Scene& scene, std::uint32_t rawHandle)
		{
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			ScriptValue value = bindings.NewUserdata("Entity");
			Entity* target = nullptr;
			if (!bindings.Unwrap<Entity>("Entity", value, &target) || !target)
				throw std::logic_error("Entity user type is not registered");
			new (target) Entity(&scene, static_cast<entt::entity>(rawHandle));
			return value;
		}

		std::string RequireStringArgument(const ScriptValue& value, const char* method, const char* what)
		{
			std::string text;
			if (!value.AsString(&text))
				throw std::logic_error(std::string(method) + " expects " + what + " to be a string");
			return text;
		}

		double RequireNumberArgument(const ScriptValue& value, const char* method, const char* what)
		{
			double number = 0.0;
			if (!value.AsNumber(&number))
				throw std::logic_error(std::string(method) + " expects " + what + " to be a number");
			return number;
		}

		std::uint64_t RequireHandleArgument(const ScriptValue& value, const char* method)
		{
			const double number = RequireNumberArgument(value, method, "handle");
			constexpr double kMaxExactInteger = 9007199254740992.0;   // 2^53
			if (!std::isfinite(number) || number < 0.0 || number != std::floor(number) || number > kMaxExactInteger)
				throw std::logic_error(std::string(method) +
					" expects a handle returned by events.on / timers.after / timers.every");
			return static_cast<std::uint64_t>(number);
		}

		// 兼容 `events:on(...)`(冒号,args[0] 是表)与 `events.on(...)`(点号)两种调用;
		// 返回首个真实参数的下标,并在这里统一做参数个数校验。
		std::size_t ReceiverOffset(const ScriptValue* args, std::size_t count,
			std::size_t required, std::size_t maximumAllowed, const char* signature)
		{
			const bool hasReceiver = count > required && args[0].IsTable();
			const std::size_t base = hasReceiver ? 1 : 0;
			const std::size_t argumentCount = count > base ? count - base : 0;
			if (argumentCount < required || argumentCount > maximumAllowed)
			{
				const std::string expected = required == maximumAllowed
					? std::to_string(required)
					: (std::to_string(required) + " to " + std::to_string(maximumAllowed));
				throw std::logic_error(std::string(signature) + " expects " + expected +
					" argument(s); got " + std::to_string(argumentCount));
			}
			return base;
		}

		ScriptEventOwner RequireCurrentOwner(const char* method)
		{
			ScriptEventOwner owner;
			if (!TryGetCurrentScriptEventOwner(&owner) || !owner.ScenePtr || !owner.EntityRef.IsValid())
				throw std::logic_error(std::string(method) +
					" is only available inside a script instance callback (OnCreate/OnUpdate/OnUI/OnDestroy)");
			return owner;
		}

		// ---- 当前脚本实例(owner)栈 ----

		std::vector<ScriptEventOwner>& OwnerStack()
		{
			return Bridge().OwnerStack;
		}

		// ---- 订阅调用(事件派发与计时器触发共用) ----

		void InvokeSubscriptionById(std::uint64_t id, const std::vector<ScriptEventValue>& values,
			const char* phase)
		{
			EventSubscription snapshot;
			{
				EventSubscription* subscription = FindSubscription(id);
				if (!subscription)
					return;
				// 拷贝:回调里可能再 on/off/cancel,集合变化不影响本次调用持有的副本。
				snapshot = *subscription;
			}
			if (!snapshot.Callback.IsValid())
			{
				DropOwnerSubscriptions(snapshot.OwnerEntity, snapshot.OwnerGeneration);
				return;
			}

			Scene* scene = snapshot.OwnerScene;
			// owner 判活 + 进入"与生命周期回调同源"的派发作用域(白名单结构写在回调里可用)。
			if (!scene || !snapshot.OwnerEntity.IsValid())
			{
				DropOwnerSubscriptions(snapshot.OwnerEntity, snapshot.OwnerGeneration);
				return;
			}
			Scene::ScriptCallbackScope scope(*scene, snapshot.OwnerEntity,
				static_cast<entt::id_type>(snapshot.OwnerComponent), snapshot.OwnerGeneration);
			if (!scope.IsValid())
			{
				// 实例已销毁/已热重载/非 Running:订阅作废,不调用旧闭包。
				DropOwnerSubscriptions(snapshot.OwnerEntity, snapshot.OwnerGeneration);
				return;
			}

			std::vector<ScriptValue> arguments;
			arguments.reserve(values.size());
			try
			{
				for (const ScriptEventValue& value : values)
				{
					switch (value.Kind)
					{
						case ScriptEventValueKind::Number: arguments.push_back(ScriptValue::Number(value.Number)); break;
						case ScriptEventValueKind::Boolean: arguments.push_back(ScriptValue::Boolean(value.Boolean)); break;
						case ScriptEventValueKind::String: arguments.push_back(ScriptValue::String(value.String)); break;
						case ScriptEventValueKind::Entity:
							arguments.push_back(BoxEntityForScene(*scene, value.EntityHandle));
							break;
						default: break;
					}
				}
			}
			catch (const std::exception& error)
			{
				if (Log::GetCoreLogger())
					WLD_CORE_ERROR("[Lua] event argument boxing failed ({0}): {1}", snapshot.Name, error.what());
				return;
			}

			std::string error;
			if (!snapshot.Callback.Call(arguments.data(), arguments.size(), nullptr, &error))
			{
				ScriptEngine::FaultScriptInstance(snapshot.OwnerEntity, snapshot.OwnerGeneration, phase, error);
				DropOwnerSubscriptions(snapshot.OwnerEntity, snapshot.OwnerGeneration);
			}
		}

		void DispatchBusEvent(std::uint32_t typeId, const Gameplay::EventValue& value)
		{
			std::vector<ScriptEventValue> values;
			std::string error;
			if (!DeserializeScriptEventPayload(value.Payload, &values, &error))
			{
				const EventNameEntry* entry = FindEventNameById(typeId);
				if (Log::GetCoreLogger())
					WLD_CORE_ERROR("[Lua] dropped script event '{0}': {1}",
						entry ? entry->Name : std::string("<unknown>"), error);
				return;
			}

			// 先快照订阅 id:handler 内部再 on/off 不会破坏本次派发的遍历。
			std::vector<std::uint64_t> ids;
			for (const EventSubscription& subscription : Bridge().Subscriptions)
				if (!subscription.IsTimer && subscription.TypeId == typeId)
					ids.push_back(subscription.Id);
			for (const std::uint64_t id : ids)
				InvokeSubscriptionById(id, values, "Event");
		}

		// ---- Lua 方法实现 ----

		ScriptValue EventsOnImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 2, 2, "events:on(name, fn)");
			const std::string name = RequireStringArgument(args[base], "events:on", "the event name");
			const EventNameEntry* entry = FindEventName(name.c_str());
			if (!entry)
				throw std::logic_error("events:on: unknown event '" + name +
					"'; register it with World::RegisterScriptEventName before use");
			ScriptFunctionRef callback;
			if (!args[base + 1].AsFunction(&callback) || !callback.IsValid())
				throw std::logic_error("events:on: the second argument must be a function");
			const ScriptEventOwner owner = RequireCurrentOwner("events:on");
			(void)RequireEventBus();   // 没有 GameApp 会话 / 会话已重建 → 可读 Lua error
			SubscribeNameOnBus(entry->TypeId);

			EventSubscription subscription;
			subscription.Id = Bridge().NextSubscriptionId++;
			subscription.Name = entry->Name;
			subscription.TypeId = entry->TypeId;
			subscription.OwnerEntity = owner.EntityRef;
			subscription.OwnerScene = owner.ScenePtr;
			subscription.OwnerComponent = owner.Component;
			subscription.OwnerGeneration = owner.Generation;
			subscription.IsTimer = false;
			subscription.SessionId = Bridge().BoundSessionId;
			subscription.Callback = callback;
			const std::uint64_t id = subscription.Id;
			Bridge().Subscriptions.push_back(std::move(subscription));
			return ScriptValue::Number(static_cast<double>(id));
		}

		ScriptValue EventsOffImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 1, 1, "events:off(handle)");
			const std::uint64_t id = RequireHandleArgument(args[base], "events:off");
			EventSubscription* subscription = FindSubscription(id);
			if (!subscription || subscription->IsTimer)
				return ScriptValue::Boolean(false);
			Bridge().Subscriptions.erase(
				std::remove_if(Bridge().Subscriptions.begin(), Bridge().Subscriptions.end(),
					[&](const EventSubscription& entry) { return entry.Id == id; }),
				Bridge().Subscriptions.end());
			return ScriptValue::Boolean(true);
		}

		ScriptValue EventsEmitImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 1, kMaxSignatureArguments + 1,
				"events:emit(name, ...)");
			const std::string name = RequireStringArgument(args[base], "events:emit", "the event name");
			const EventNameEntry* entry = FindEventName(name.c_str());
			if (!entry)
				throw std::logic_error("events:emit: unknown event '" + name +
					"'; register it with World::RegisterScriptEventName before use");

			const std::size_t valueCount = count - base - 1;
			if (valueCount != entry->ArgumentKinds.size())
				throw std::logic_error("events:emit('" + name + "') expects " +
					std::to_string(entry->ArgumentKinds.size()) + " argument(s) matching its registered signature '" +
					entry->Signature + "'; got " + std::to_string(valueCount));

			std::vector<ScriptEventValue> values;
			values.reserve(valueCount);
			for (std::size_t index = 0; index < valueCount; ++index)
			{
				const ScriptValue& value = args[base + 1 + index];
				const std::string location = "events:emit('" + name + "') argument " + std::to_string(index + 1);
				switch (entry->ArgumentKinds[index])
				{
					case ScriptEventValueKind::Number:
					{
						double number = 0.0;
						if (!value.AsNumber(&number))
							throw std::logic_error(location + " must be a number (registered signature '" +
								entry->Signature + "')");
						values.push_back(ScriptEventValue::MakeNumber(number));
						break;
					}
					case ScriptEventValueKind::Boolean:
					{
						bool boolean = false;
						if (!value.AsBool(&boolean))
							throw std::logic_error(location + " must be a boolean (registered signature '" +
								entry->Signature + "')");
						values.push_back(ScriptEventValue::MakeBoolean(boolean));
						break;
					}
					case ScriptEventValueKind::String:
					{
						std::string text;
						if (!value.AsString(&text))
							throw std::logic_error(location + " must be a string (registered signature '" +
								entry->Signature + "')");
						values.push_back(ScriptEventValue::MakeString(std::move(text)));
						break;
					}
					case ScriptEventValueKind::Entity:
					{
						Entity* entity = nullptr;
						if (!ScriptEngine::GetBindingContext().Unwrap<Entity>("Entity", value, &entity) || !entity)
							throw std::logic_error(location + " must be an Entity (registered signature '" +
								entry->Signature + "')");
						if (!entity->IsValid())
							throw std::logic_error(location + " is an invalid/expired entity handle");
						values.push_back(ScriptEventValue::MakeEntity(static_cast<std::uint32_t>(*entity)));
						break;
					}
					default: break;
				}
			}

			std::vector<std::uint8_t> payload;
			std::string error;
			if (!SerializeScriptEventPayload(values, &payload, &error))
				throw std::logic_error("events:emit('" + name + "') payload rejected: " + error);

			ScriptEventOwner owner;
			const bool hasOwner = TryGetCurrentScriptEventOwner(&owner);
			const std::uint64_t source = hasOwner ? static_cast<std::uint32_t>(owner.EntityRef) : 0;
			Gameplay::EventBus& bus = RequireEventBus();
			bus.EmitDeferredRaw(entry->TypeId, payload.data(), payload.size(), source);
			return ScriptValue::Boolean(true);
		}

		ScriptValue TimersAfterImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 2, 2, "timers:after(seconds, fn)");
			const double seconds = RequireNumberArgument(args[base], "timers:after", "seconds");
			if (!std::isfinite(seconds) || seconds < 0.0)
				throw std::logic_error("timers:after expects seconds to be a finite number >= 0");
			ScriptFunctionRef callback;
			if (!args[base + 1].AsFunction(&callback) || !callback.IsValid())
				throw std::logic_error("timers:after: the second argument must be a function");
			const ScriptEventOwner owner = RequireCurrentOwner("timers:after");
			Gameplay::TimerService& timers = RequireTimerService();

			EventSubscription subscription;
			subscription.Id = Bridge().NextSubscriptionId++;
			subscription.Name = "timers:after";
			subscription.OwnerEntity = owner.EntityRef;
			subscription.OwnerScene = owner.ScenePtr;
			subscription.OwnerComponent = owner.Component;
			subscription.OwnerGeneration = owner.Generation;
			subscription.IsTimer = true;
			subscription.SessionId = Bridge().BoundSessionId;
			subscription.Callback = callback;
			EventSubscription& stored = Bridge().Subscriptions.emplace_back(std::move(subscription));
			const std::uint64_t id = stored.Id;
			stored.TimerHandle = timers.After(seconds, [id] { InvokeSubscriptionById(id, {}, "Timer"); });
			return ScriptValue::Number(static_cast<double>(id));
		}

		ScriptValue TimersEveryImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 2, 3, "timers:every(seconds, fn[, count])");
			const double seconds = RequireNumberArgument(args[base], "timers:every", "seconds");
			if (!std::isfinite(seconds) || seconds < 0.0)
				throw std::logic_error("timers:every expects seconds to be a finite number >= 0");
			ScriptFunctionRef callback;
			if (!args[base + 1].AsFunction(&callback) || !callback.IsValid())
				throw std::logic_error("timers:every: the second argument must be a function");
			std::uint32_t repeats = 0;   // 0 = 无限重复
			if (count - base == 3)
			{
				const double number = RequireNumberArgument(args[base + 2], "timers:every", "count");
				if (!std::isfinite(number) || number < 0.0 || number != std::floor(number) ||
					number > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
					throw std::logic_error("timers:every expects count to be a non-negative integer (0 = forever)");
				repeats = static_cast<std::uint32_t>(number);
			}
			const ScriptEventOwner owner = RequireCurrentOwner("timers:every");
			Gameplay::TimerService& timers = RequireTimerService();

			EventSubscription subscription;
			subscription.Id = Bridge().NextSubscriptionId++;
			subscription.Name = "timers:every";
			subscription.OwnerEntity = owner.EntityRef;
			subscription.OwnerScene = owner.ScenePtr;
			subscription.OwnerComponent = owner.Component;
			subscription.OwnerGeneration = owner.Generation;
			subscription.IsTimer = true;
			subscription.SessionId = Bridge().BoundSessionId;
			subscription.Callback = callback;
			EventSubscription& stored = Bridge().Subscriptions.emplace_back(std::move(subscription));
			const std::uint64_t id = stored.Id;
			stored.TimerHandle = timers.Every(seconds, [id] { InvokeSubscriptionById(id, {}, "Timer"); }, repeats);
			return ScriptValue::Number(static_cast<double>(id));
		}

		ScriptValue TimersCancelImpl(const ScriptValue* args, std::size_t count)
		{
			const std::size_t base = ReceiverOffset(args, count, 1, 1, "timers:cancel(handle)");
			const std::uint64_t id = RequireHandleArgument(args[base], "timers:cancel");
			EventSubscription* subscription = FindSubscription(id);
			if (!subscription || !subscription->IsTimer)
				return ScriptValue::Boolean(false);
			CancelTimerIfAlive(*subscription);
			Bridge().Subscriptions.erase(
				std::remove_if(Bridge().Subscriptions.begin(), Bridge().Subscriptions.end(),
					[&](const EventSubscription& entry) { return entry.Id == id; }),
				Bridge().Subscriptions.end());
			return ScriptValue::Boolean(true);
		}

		// ---- 描述表(运行时注册与存根渲染的唯一来源) ----

		const ScriptServiceBinding* EventBindingsDescriptor(std::size_t* count)
		{
			static const ScriptServiceParam onParameters[] = {
				{ "name", "string", ScriptServiceArgType::String, true, "Registered event name." },
				{ "fn", "function", ScriptServiceArgType::None, true, "Handler called at the frame-end dispatch." },
			};
			static const ScriptServiceParam offParameters[] = {
				{ "handle", "number", ScriptServiceArgType::Number, true, "Handle returned by events.on." },
			};
			static const ScriptServiceParam emitParameters[] = {
				{ "name", "string", ScriptServiceArgType::String, true, "Registered event name." },
			};
			static const ScriptServiceMethod eventMethods[] = {
				{ "on", &EventsOnImpl, onParameters, 2, 2, "number",
					"Subscribe to a registered event; returns an opaque handle owned by the calling script instance." },
				{ "off", &EventsOffImpl, offParameters, 1, 1, "boolean",
					"Unsubscribe a handle returned by events.on; false when the handle is unknown." },
				{ "emit", &EventsEmitImpl, emitParameters, 1, 1, "boolean",
					"Queue a registered event (delivered at the frame end by GameApp::Tick); extra arguments must match the signature registered with RegisterScriptEventName." },
			};
			static const ScriptServiceParam afterParameters[] = {
				{ "seconds", "number", ScriptServiceArgType::Number, true, "Delay in seconds; fires once on the fixed step that reaches it." },
				{ "fn", "function", ScriptServiceArgType::None, true, "Handler called on the fixed step that reaches the delay." },
			};
			static const ScriptServiceParam everyParameters[] = {
				{ "seconds", "number", ScriptServiceArgType::Number, true, "Interval in seconds on the fixed step." },
				{ "fn", "function", ScriptServiceArgType::None, true, "Repeated handler." },
				{ "count", "number", ScriptServiceArgType::Number, false, "Repeat count; 0 or omitted = forever." },
			};
			static const ScriptServiceParam cancelParameters[] = {
				{ "handle", "number", ScriptServiceArgType::Number, true, "Handle returned by timers.after/every." },
			};
			static const ScriptServiceMethod timerMethods[] = {
				{ "after", &TimersAfterImpl, afterParameters, 2, 2, "number",
					"Run a callback once after the given delay; returns an opaque handle owned by the calling script instance." },
				{ "every", &TimersEveryImpl, everyParameters, 3, 3, "number",
					"Run a callback every interval on the fixed step (optional repeat count; 0 = forever)." },
				{ "cancel", &TimersCancelImpl, cancelParameters, 1, 1, "boolean",
					"Cancel a handle returned by timers.after/every; false when the handle is unknown." },
			};
			static const ScriptServiceBinding tables[] = {
				{ "events", "Read-only event table; subscriptions belong to the calling script instance.", eventMethods, 3 },
				{ "timers", "Read-only timer table; timers advance on the fixed step and freeze while paused.", timerMethods, 3 },
			};
			if (count)
				*count = sizeof(tables) / sizeof(tables[0]);
			return tables;
		}
	}

	// ---- ScriptEventValue ----

	ScriptEventValue ScriptEventValue::MakeNumber(double value)
	{
		ScriptEventValue result;
		result.Kind = ScriptEventValueKind::Number;
		result.Number = value;
		return result;
	}

	ScriptEventValue ScriptEventValue::MakeBoolean(bool value)
	{
		ScriptEventValue result;
		result.Kind = ScriptEventValueKind::Boolean;
		result.Boolean = value;
		return result;
	}

	ScriptEventValue ScriptEventValue::MakeString(std::string value)
	{
		ScriptEventValue result;
		result.Kind = ScriptEventValueKind::String;
		result.String = std::move(value);
		return result;
	}

	ScriptEventValue ScriptEventValue::MakeEntity(std::uint32_t handle)
	{
		ScriptEventValue result;
		result.Kind = ScriptEventValueKind::Entity;
		result.EntityHandle = handle;
		return result;
	}

	// ---- 事件名目录 ----

	bool RegisterScriptEventName(const char* name, const char* argSignature, std::string* error)
	{
		if (error) error->clear();
		if (!name || !name[0])
		{
			if (error) *error = "event name must be a non-empty string";
			return false;
		}
		if (FindEventName(name))
		{
			if (error) *error = std::string("event name is already registered: ") + name;
			return false;
		}
		std::vector<ScriptEventValueKind> kinds;
		const std::string signature = argSignature ? argSignature : "";
		if (!ParseArgumentSignature(signature, &kinds, error))
			return false;

		EventNameEntry entry;
		entry.Name = name;
		entry.Signature = signature;
		entry.ArgumentKinds = std::move(kinds);
		entry.TypeId = Fnv1a32("script-event:" + entry.Name);
		if (entry.TypeId == 0)
		{
			if (error) *error = std::string("event name hashes to the reserved type id 0: ") + name;
			return false;
		}
		for (const EventNameEntry& existing : EventNames())
		{
			if (existing.TypeId == entry.TypeId)
			{
				if (error) *error = std::string("event name collides with '") + existing.Name +
					"' (same FNV-1a type id); rename one of them";
				return false;
			}
		}
		EventNames().push_back(std::move(entry));
		return true;
	}

	bool IsScriptEventNameRegistered(const char* name)
	{
		return FindEventName(name) != nullptr;
	}

	std::size_t GetScriptEventNameCount()
	{
		return EventNames().size();
	}

	std::uint32_t ScriptEventTypeId(const char* name)
	{
		const EventNameEntry* entry = FindEventName(name);
		return entry ? entry->TypeId : 0;
	}

	// ---- 载荷盒装 ----

	bool SerializeScriptEventPayload(const std::vector<ScriptEventValue>& values,
		std::vector<std::uint8_t>* out, std::string* error)
	{
		if (error) error->clear();
		if (!out)
		{
			if (error) *error = "payload output pointer is null";
			return false;
		}
		if (values.size() > kMaxEventValues)
		{
			if (error) *error = "too many event arguments: " + std::to_string(values.size()) +
				" (limit " + std::to_string(kMaxEventValues) + ")";
			return false;
		}

		std::vector<std::uint8_t> payload;
		payload.reserve(4 + values.size() * 12);
		AppendValue(payload, static_cast<std::uint32_t>(values.size()));
		for (const ScriptEventValue& value : values)
		{
			const std::uint8_t kind = static_cast<std::uint8_t>(value.Kind);
			AppendValue(payload, kind);
			switch (value.Kind)
			{
				case ScriptEventValueKind::Number:
					AppendValue(payload, value.Number);
					break;
				case ScriptEventValueKind::Boolean:
				{
					const std::uint8_t boolean = value.Boolean ? 1u : 0u;
					AppendValue(payload, boolean);
					break;
				}
				case ScriptEventValueKind::String:
				{
					if (value.String.size() > kMaxEventStringBytes)
					{
						if (error) *error = "event string argument is too long: " +
							std::to_string(value.String.size()) + " (limit " +
							std::to_string(kMaxEventStringBytes) + ")";
						return false;
					}
					AppendValue(payload, static_cast<std::uint32_t>(value.String.size()));
					AppendBytes(payload, value.String.data(), value.String.size());
					break;
				}
				case ScriptEventValueKind::Entity:
					AppendValue(payload, value.EntityHandle);
					break;
				default:
					if (error) *error = "unknown event value kind";
					return false;
			}
			if (payload.size() > kMaxEventPayloadBytes)
			{
				if (error) *error = "event payload exceeds " + std::to_string(kMaxEventPayloadBytes) + " bytes";
				return false;
			}
		}
		*out = std::move(payload);
		return true;
	}

	bool DeserializeScriptEventPayload(const std::vector<std::uint8_t>& payload,
		std::vector<ScriptEventValue>* out, std::string* error)
	{
		if (error) error->clear();
		if (!out)
		{
			if (error) *error = "payload output pointer is null";
			return false;
		}
		out->clear();
		if (payload.empty())
			return true;   // 无参数事件
		if (payload.size() > kMaxEventPayloadBytes)
		{
			if (error) *error = "event payload exceeds " + std::to_string(kMaxEventPayloadBytes) + " bytes";
			return false;
		}

		std::size_t cursor = 0;
		std::uint32_t count = 0;
		if (!ReadValue(payload, cursor, &count))
		{
			if (error) *error = "truncated event payload header";
			return false;
		}
		if (count > kMaxEventValues)
		{
			if (error) *error = "too many event arguments in payload: " + std::to_string(count);
			return false;
		}
		out->reserve(count);
		for (std::uint32_t index = 0; index < count; ++index)
		{
			std::uint8_t kind = 0;
			if (!ReadValue(payload, cursor, &kind))
			{
				if (error) *error = "truncated event payload value kind";
				return false;
			}
			ScriptEventValue value;
			switch (static_cast<ScriptEventValueKind>(kind))
			{
				case ScriptEventValueKind::Number:
				{
					double number = 0.0;
					if (!ReadValue(payload, cursor, &number))
					{
						if (error) *error = "truncated event number argument";
						return false;
					}
					value = ScriptEventValue::MakeNumber(number);
					break;
				}
				case ScriptEventValueKind::Boolean:
				{
					std::uint8_t boolean = 0;
					if (!ReadValue(payload, cursor, &boolean))
					{
						if (error) *error = "truncated event boolean argument";
						return false;
					}
					value = ScriptEventValue::MakeBoolean(boolean != 0);
					break;
				}
				case ScriptEventValueKind::String:
				{
					std::uint32_t size = 0;
					if (!ReadValue(payload, cursor, &size) || size > kMaxEventStringBytes)
					{
						if (error) *error = "invalid event string argument length";
						return false;
					}
					std::string text;
					if (size > 0)
					{
						if (cursor + size > payload.size())
						{
							if (error) *error = "truncated event string argument";
							return false;
						}
						text.assign(reinterpret_cast<const char*>(payload.data() + cursor), size);
						cursor += size;
					}
					value = ScriptEventValue::MakeString(std::move(text));
					break;
				}
				case ScriptEventValueKind::Entity:
				{
					std::uint32_t handle = 0;
					if (!ReadValue(payload, cursor, &handle))
					{
						if (error) *error = "truncated event entity argument";
						return false;
					}
					value = ScriptEventValue::MakeEntity(handle);
					break;
				}
				default:
					if (error) *error = "unknown event argument kind in payload";
					return false;
			}
			out->push_back(std::move(value));
		}
		return true;
	}

	// ---- 绑定注册 ----

	bool RegisterEventBindings(ScriptBindingContext& bindings, std::string* error)
	{
		if (error) error->clear();
		LuauVm* vm = bindings.Vm();
		if (!vm)
		{
			if (error) *error = "the binding context has no VM";
			return false;
		}
		std::size_t tableCount = 0;
		const ScriptServiceBinding* tables = ScriptEventBindings(&tableCount);
		for (std::size_t tableIndex = 0; tableIndex < tableCount; ++tableIndex)
		{
			const ScriptServiceBinding& table = tables[tableIndex];
			if (!table.Name || !table.Name[0] || !table.Methods || table.MethodCount == 0)
			{
				if (error) *error = "invalid event binding descriptor";
				return false;
			}
			if (vm->GetGlobal(table.Name).Type() != ScriptValueType::Nil)
			{
				if (error) *error = std::string("global name is already in use: ") + table.Name;
				return false;
			}
			ScriptTableRef luaTable = vm->CreateTable();
			if (!luaTable.IsValid())
			{
				if (error) *error = std::string("failed to create the event table: ") + table.Name;
				return false;
			}
			for (std::size_t methodIndex = 0; methodIndex < table.MethodCount; ++methodIndex)
			{
				const ScriptServiceMethod& method = table.Methods[methodIndex];
				if (!method.Name || !method.Name[0] || !method.Function)
				{
					if (error) *error = std::string("event table '") + table.Name + "' has an invalid method entry";
					return false;
				}
				if (!luaTable.SetField(method.Name, bindings.CreateFunction(method.Name, method.Function)))
				{
					if (error) *error = std::string("failed to bind ") + table.Name + "." + method.Name;
					return false;
				}
			}
			if (!luaTable.SetMetaField("__newindex", bindings.CreateFunction(table.Name,
				[](const ScriptValue*, std::size_t) -> ScriptValue
				{
					throw std::logic_error("event/timer tables are read-only; use the documented methods");
				})))
			{
				if (error) *error = std::string("failed to lock the event table: ") + table.Name;
				return false;
			}
			if (!vm->SetGlobal(table.Name, luaTable.ToValue()))
			{
				if (error) *error = std::string("failed to publish the event table: ") + table.Name;
				return false;
			}
		}
		return true;
	}

	const ScriptServiceBinding* ScriptEventBindings(std::size_t* count)
	{
		return EventBindingsDescriptor(count);
	}

	// ---- 当前脚本实例(owner)作用域 ----

	void PushScriptEventOwner(const ScriptEventOwner& owner)
	{
		OwnerStack().push_back(owner);
	}

	void PopScriptEventOwner()
	{
		std::vector<ScriptEventOwner>& stack = OwnerStack();
		if (!stack.empty())
			stack.pop_back();
	}

	bool TryGetCurrentScriptEventOwner(ScriptEventOwner* out)
	{
		const std::vector<ScriptEventOwner>& stack = OwnerStack();
		if (stack.empty())
			return false;
		if (out)
			*out = stack.back();
		return true;
	}

	// ---- 生命周期收口 ----

	std::size_t ReleaseScriptEventOwners(const Entity& owner, std::uint64_t generation)
	{
		if (!owner.IsValid())
			return 0;
		EventBridgeState& bridge = Bridge();
		std::size_t removed = 0;
		for (EventSubscription& subscription : bridge.Subscriptions)
		{
			if (!SameOwner(subscription, owner, generation))
				continue;
			CancelTimerIfAlive(subscription);
			++removed;
		}
		if (removed == 0)
			return 0;
		bridge.Subscriptions.erase(
			std::remove_if(bridge.Subscriptions.begin(), bridge.Subscriptions.end(),
				[&](const EventSubscription& subscription)
				{ return SameOwner(subscription, owner, generation); }),
			bridge.Subscriptions.end());
		return removed;
	}

	void ResetScriptEventSubscriptions()
	{
		EventBridgeState& bridge = Bridge();
		bridge.Subscriptions.clear();
		bridge.OwnerStack.clear();
		bridge.NameSubscriptions.clear();
		bridge.BoundBus = nullptr;
		bridge.BoundSessionId = 0;
	}
}
