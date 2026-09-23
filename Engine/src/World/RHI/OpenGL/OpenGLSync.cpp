#include "wldpch.h"
#include "OpenGLSync.h"

namespace World::Rhi::OpenGL
{
	OpenGLFence::~OpenGLFence()
	{
		if (m_Sync)
			glDeleteSync(m_Sync);
	}

	void OpenGLFence::Wait(uint64_t timeoutNs)
	{
		if (!m_Sync)
			return;
		const GLuint64 timeout = timeoutNs == UINT64_MAX ? UINT64_MAX : timeoutNs;
		glClientWaitSync(m_Sync, GL_SYNC_FLUSH_COMMANDS_BIT, timeout);
	}

	bool OpenGLFence::IsSignaled() const
	{
		if (!m_Sync)
			return true;
		return glClientWaitSync(m_Sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0) == GL_ALREADY_SIGNALED;
	}

	void OpenGLFence::Reset()
	{
		if (m_Sync)
		{
			glDeleteSync(m_Sync);
			m_Sync = nullptr;
		}
	}

	void OpenGLFence::Signal()
	{
		Reset();
		m_Sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}

	OpenGLSemaphore::~OpenGLSemaphore()
	{
		for (auto& [value, sync] : m_Pending)
			if (sync)
				glDeleteSync(sync);
	}

	void OpenGLSemaphore::Signal(uint64_t value)
	{
		if (!m_Timeline)
			return;   // 二值信号量在单队列 GL 下无需真实对象
		m_Value = std::max(m_Value, value);
		m_Pending.emplace_back(m_Value, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
	}

	void OpenGLSemaphore::Wait(uint64_t value)
	{
		if (!m_Timeline)
			return;
		if (m_Completed >= value)
			return;
		for (auto& [signalValue, sync] : m_Pending)
		{
			if (signalValue <= m_Completed)
				continue;
			glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
			m_Completed = std::max(m_Completed, signalValue);
			if (m_Completed >= value)
				return;
		}
	}
}
