#pragma once

#include "World/RHI/RhiCommandQueue.h"

namespace World::Rhi::OpenGL
{
	class OpenGLCommandQueue : public CommandQueue
	{
	public:
		explicit OpenGLCommandQueue(const std::string& name) : m_Name(name) {}

		void Submit(const SubmitInfo& info) override;
		void WaitIdle() override;
		void ExecuteImmediate(const std::function<void(CommandBuffer&)>& record) override;

	private:
		std::string m_Name;
	};
}
