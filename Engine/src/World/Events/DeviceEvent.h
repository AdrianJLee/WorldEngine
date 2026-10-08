#pragma once

#include "World/Events/Event.h"
#include "World/Gameplay/Framework/InputTypes.h"

namespace World
{
	class WLD_API DeviceConnectedEvent : public Event
	{
	public:
		explicit DeviceConnectedEvent(Gameplay::DeviceId device)
			: m_Device(device) {}

		Gameplay::DeviceId GetDevice() const { return m_Device; }

		std::string ToString() const override
		{
			return std::string("DeviceConnectedEvent: device=") +
				std::to_string(static_cast<int>(m_Device.Device)) +
				" index=" + std::to_string(m_Device.Index);
		}

		EVENT_CLASS_TYPE(DeviceConnected)
		EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryDevice)

	private:
		Gameplay::DeviceId m_Device;
	};

	class WLD_API DeviceDisconnectedEvent : public Event
	{
	public:
		explicit DeviceDisconnectedEvent(Gameplay::DeviceId device)
			: m_Device(device) {}

		Gameplay::DeviceId GetDevice() const { return m_Device; }

		std::string ToString() const override
		{
			return std::string("DeviceDisconnectedEvent: device=") +
				std::to_string(static_cast<int>(m_Device.Device)) +
				" index=" + std::to_string(m_Device.Index);
		}

		EVENT_CLASS_TYPE(DeviceDisconnected)
		EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryDevice)

	private:
		Gameplay::DeviceId m_Device;
	};
}
