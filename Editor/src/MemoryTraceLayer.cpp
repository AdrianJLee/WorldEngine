#include "MemoryTraceLayer.h"
namespace World
{
	MemoryTraceLayer::MemoryTraceLayer()
		:Layer("MemoryTraceLayer")
	{}
	void MemoryTraceLayer::OnAttach()
	{}
	void MemoryTraceLayer::OnDetach()
	{}
	void MemoryTraceLayer::OnUpdate(Timestep ts)
	{}
	void MemoryTraceLayer::OnImGuiRender()
	{
		DrawPoolStats();
	}
	void MemoryTraceLayer::OnEvent(Event& event)
	{}
	void MemoryTraceLayer::DrawPoolStats()
	{
		ImGui::Begin("Memory Analyzer");

		auto stats = MemoryTracker::Get().GetSnapshot();

		if (ImGui::CollapsingHeader("Pool Allocators", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("MemTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Type (Tag)");
				ImGui::TableSetupColumn("Usage");
				ImGui::TableSetupColumn("Reserved");
				ImGui::TableSetupColumn("Count");
				ImGui::TableHeadersRow();

				for (const auto& s : stats)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%s (%d)", s.TypeName, (int)s.Tag);
					ImGui::TableNextColumn();
					ImGui::Text("%.2f KB", s.UsedBytes / 1024.0f);
					ImGui::TableNextColumn();
					ImGui::Text("%.2f KB", s.TotalReserved / 1024.0f);
					ImGui::TableNextColumn();
					ImGui::Text("%llu", s.NumAllocations);
				}
				ImGui::EndTable();
			}
		}

		ImGui::End();
	}
}