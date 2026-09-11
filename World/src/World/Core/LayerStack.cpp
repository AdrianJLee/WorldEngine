#include "wldpch.h"
#include "LayerStack.h"
#include <stdexcept>

namespace World
{
	LayerStack::LayerStack()
	{}

	LayerStack::~LayerStack()
	{
		DetachAll();
	}

	void LayerStack::PushLayer(Layer* layer)
	{
		if (!layer || m_Detaching || std::find(m_Layers.begin(), m_Layers.end(), layer) != m_Layers.end())
			throw std::logic_error("Cannot attach a null, duplicate or shutting-down layer");
		layer->OnAttach();
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, layer);
		m_LayerInsertIndex++;
		m_AttachOrder.push_back(layer);
	}

	void LayerStack::PushOverLay(Layer* overlay)
	{
		if (!overlay || m_Detaching || std::find(m_Layers.begin(), m_Layers.end(), overlay) != m_Layers.end())
			throw std::logic_error("Cannot attach a null, duplicate or shutting-down layer");
		overlay->OnAttach();
		m_Layers.emplace_back(overlay);
		m_AttachOrder.push_back(overlay);
	}
	void LayerStack::PopLayer(Layer* layer)
	{
		auto layersEnd = m_Layers.begin() + m_LayerInsertIndex;
		auto it = std::find(m_Layers.begin(), layersEnd, layer);
		if (it != layersEnd)
		{
			m_Layers.erase(it);
			m_LayerInsertIndex--;
			Detach(layer);
		}
	}
	void LayerStack::PopOverLay(Layer* overlay)
	{
		auto it = std::find(m_Layers.begin() + m_LayerInsertIndex, m_Layers.end(), overlay);
		if (it != m_Layers.end())
		{
			m_Layers.erase(it);
			Detach(overlay);
		}
	}

	void LayerStack::Detach(Layer* layer) noexcept
	{
		auto attached = std::find(m_AttachOrder.begin(), m_AttachOrder.end(), layer);
		if (attached == m_AttachOrder.end()) return;
		m_AttachOrder.erase(attached);
		try { layer->OnDetach(); }
		catch (const std::exception& error)
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("Layer detach failed: {}", error.what());
		}
		catch (...)
		{
			if (Log::GetCoreLogger()) WLD_CORE_ERROR("Layer detach failed with an unknown exception");
		}
	}

	void LayerStack::DetachAll() noexcept
	{
		if (m_Detaching) return;
		m_Detaching = true;
		while (!m_AttachOrder.empty()) Detach(m_AttachOrder.back());
		m_Layers.clear();
		m_LayerInsertIndex = 0;
		m_Detaching = false;
	}

}
