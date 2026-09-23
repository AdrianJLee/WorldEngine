#pragma once
#include "wldpch.h"
#include "World/Core/Core.h"

namespace World
{
	// Events in World are currently blocking, meaning when an event occurs it
	// immediately gets dispatched and must be dealt with right then and there.
	// For the future, a better strategy might be to buffer events in an event
	// bus and process them during the "event" part of the update stage.
	enum class EventType
	{
		None = 0,
		WindowClose, WindowResize, WindowFocus, WindowLostFocus, WindowMoved,
		AppTick, AppUpdate, AppRender,
		KeyPressed, KeyReleased, KeyTyped,
		MouseButtonPressed, MouseButtonReleased, MouseMoved, MouseScrolled
	};

	// Bitfields for event categories
	enum EventCategory
	{
		None = 0,
		EventCategoryApplication = BIT(0),
		EventCategoryInput = BIT(1),
		EventCategoryKeyboard = BIT(2),
		EventCategoryMouse = BIT(3),
		EventCategoryMouseButton = BIT(4)
	};

	// Macros to help define event types and categories
	#define EVENT_CLASS_TYPE(type) static EventType GetStaticType() { return EventType::##type; }\
								virtual EventType GetEventType() const override { return GetStaticType(); }\
								virtual const char* GetName() const override { return #type; }

	// Macro to define event category
	#define EVENT_CLASS_CATEGORY(category) virtual int GetCategoryFlags() const override { return category; }


	class Event
	{
		friend class EventDispatcher;
	public:
		virtual ~Event() = default;
		virtual EventType GetEventType() const = 0;

		virtual const char* GetName() const = 0;

		virtual int GetCategoryFlags() const = 0;

		virtual std::string ToString() const { return GetName(); }

		// Check if the event is in a category
		inline bool IsInCategory(EventCategory category)
		{
			return GetCategoryFlags() & category;
		}
		// Has the event been handled?
		bool m_Handled = false;
	protected:

	};

	class EventDispatcher
	{

	public:

		EventDispatcher(Event& event)
			: m_Event(event)
		{}

		// Dispatch event if it matches the type T
		template<typename T, typename F>
		bool Dispatch(const F& func)
		{
			if (m_Event.GetEventType() == T::GetStaticType())
			{
				m_Event.m_Handled |= func(static_cast<T&>(m_Event));
				return true;
			}
			return false;
		}

	private:
		Event& m_Event;
	};

	inline std::ostream& operator<<(std::ostream& os, const Event& e)
	{
		return os << e.ToString();
	}
}