#pragma once

#include "World/RHI/RhiSync.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	class OpenGLFence : public Fence
	{
	public:
		OpenGLFence() = default;
		~OpenGLFence() override;

		void Wait(uint64_t timeoutNs = UINT64_MAX) override;
		bool IsSignaled() const override;
		void Reset() override;

		void Signal();   // 队列提交后由后端调用

	private:
		GLsync m_Sync = nullptr;
	};

	class OpenGLSemaphore : public Semaphore
	{
	public:
		explicit OpenGLSemaphore(const SemaphoreCreateDesc& desc)
			: m_Timeline(desc.Timeline), m_Value(desc.InitialValue) {}
		~OpenGLSemaphore() override;

		void Signal(uint64_t value = 0) override;
		void Wait(uint64_t value = 0) override;
		bool IsTimeline() const override { return m_Timeline; }

	private:
		bool m_Timeline = false;
		uint64_t m_Value = 0;
		uint64_t m_Completed = 0;
		std::vector<std::pair<uint64_t, GLsync>> m_Pending;
	};
}
