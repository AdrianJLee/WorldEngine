#include "wldpch.h"
#include "World/UI/UiCommandRouter.h"

#include "World/Gameplay/Framework/EventBus.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace World::UI
{
	namespace
	{
		template <std::size_t Capacity>
		void CopyTruncated(char (&destination)[Capacity], const std::string& text)
		{
			const std::size_t count = std::min(text.size(), Capacity - 1);
			if (count > 0)
				std::memcpy(destination, text.data(), count);
			destination[count] = '\0';
		}

		// 去掉首尾 ASCII 空白(`.wui` 里可能写成 " ui.close ")。
		std::string TrimAscii(const std::string& text)
		{
			const std::size_t first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return {};
			const std::size_t last = text.find_last_not_of(" \t\r\n");
			return text.substr(first, last - first + 1);
		}

		bool IsNavigationCommand(const std::string& command)
		{
			return command == "ui.close" || command == "ui.back";
		}
	}

	UiCommandDispatchResult UiCommandRouter::Dispatch(const UiCommandQueue& queue,
		Gameplay::EventBus& events, UiNavigator* navigator)
	{
		UiCommandDispatchResult result;
		for (const UiInputCommand& command : queue.Commands())
		{
			// ① 每条命令都进总线(UI → 逻辑的出口必须无损;含内置导航命令 —— 项目也能观察到它)。
			UiCommandEvent event;
			CopyTruncated(event.NodeId, command.NodeId);
			CopyTruncated(event.Event, command.Event);
			CopyTruncated(event.Command, command.Command);
			events.EmitDeferred(event);
			++result.Dispatched;

			// ② 内置导航:只做"关模态 / 出栈",不发明玩法语义。宿主没给 navigator ⇒ 只发事件。
			if (navigator == nullptr)
				continue;
			const std::string commandText = TrimAscii(command.Command);
			if (!IsNavigationCommand(commandText))
				continue;
			if (commandText == "ui.close")
			{
				if (navigator->ModalCount() > 0)
					navigator->PopModal();
				else
					navigator->Pop();
			}
			else
			{
				navigator->Back();
			}
			++result.Navigations;
		}
		return result;
	}
}
