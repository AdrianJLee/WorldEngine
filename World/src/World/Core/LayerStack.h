#pragma once
#include "World/Core/Core.h"
#include "World/Core/Layer.h"

#include <vector>

namespace World
{
	class LayerStack
	{
	public:
		LayerStack();
		~LayerStack();

		void PushLayer(Layer* layer);
		void PushOverLay(Layer* overlay);

		void PopLayer(Layer* layer);
		void PopOverLay(Layer* overlay);
		// The allocator owns layers; this stack only pairs attachment and detachment.
		void DetachAll() noexcept;

		std::vector<Layer*>::iterator begin() { return m_Layers.begin(); }
		std::vector<Layer*>::iterator end() { return m_Layers.end(); }
	private:
		void Detach(Layer* layer) noexcept;
		std::vector<Layer*> m_Layers;
		std::vector<Layer*> m_AttachOrder;
		unsigned int m_LayerInsertIndex = 0;
		bool m_Detaching = false;


	};
}
