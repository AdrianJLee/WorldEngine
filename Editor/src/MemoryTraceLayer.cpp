#include "MemoryTraceLayer.h"
namespace World
{
	static std::string FormatBytes(size_t bytes)
	{
		const char* units[] = { "B", "KB", "MB", "GB", "TB" };
		double d_bytes = static_cast<double>(bytes);
		int i = 0;
		while (d_bytes >= 1024 && i < 4)
		{
			d_bytes /= 1024;
			i++;
		}
		char buf[64];
		snprintf(buf, sizeof(buf), "%.2f %s", d_bytes, units[i]);
		return std::string(buf);
	}

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
		ImGui::Begin("Memory Analyzer");

		// 获取所有分配器的快照
		auto snapshots = World::MemoryTracker::Get().GetFullSnapshot();

		// 顶部摘要
		size_t totalUsed = 0;
		size_t totalReserved = 0;
		for (const auto& s : snapshots)
		{
			totalUsed += s.UsedBytes;
			totalReserved += s.TotalReserved;
		}

		ImGui::Text("Total Engine Usage: %s / %s",
			FormatBytes(totalUsed).c_str(),
			FormatBytes(totalReserved).c_str());

		ImGui::Separator();

		// 使用 Table 展示详细列表
		static ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;

		if (ImGui::BeginTable("AllocatorsTable", 5, flags))
		{
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Usage");
			ImGui::TableSetupColumn("Progress / Bar");
			ImGui::TableSetupColumn("Allocs");
			ImGui::TableHeadersRow();

			for (const auto& s : snapshots)
			{
				ImGui::TableNextRow();

				// 1. 名称
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", s.Name);

				// 2. 类型 (带颜色区分)
				ImGui::TableSetColumnIndex(1);
				if (s.Type == World::AllocatorType::Stack)
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Stack"); // 黄色
				else if (s.Type == World::AllocatorType::Pool)
					ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Pool");  // 青色
				else
					ImGui::Text("Linear");

				// 3. 内存数值
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%s / %s", FormatBytes(s.UsedBytes).c_str(), FormatBytes(s.TotalReserved).c_str());

				// 4. 进度条 (直观显示水位)
				ImGui::TableSetColumnIndex(3);
				float fraction = (s.TotalReserved > 0) ? (float)s.UsedBytes / (float)s.TotalReserved : 0.0f;

				// 如果是 Stack，水位波动很大，用橙色提醒
				ImVec4 barColor = (s.Type == World::AllocatorType::Stack) ? ImVec4(0.9f, 0.5f, 0.1f, 0.8f) : ImVec4(0.2f, 0.7f, 0.3f, 0.8f);
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
				ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0));
				ImGui::PopStyleColor();

				// 5. 分配次数
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu", s.NumAllocations);
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
	void MemoryTraceLayer::OnEvent(Event& event)
	{}


}