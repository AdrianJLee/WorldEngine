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

		auto snapshots = World::MemoryTracker::Get().GetFullSnapshot();

		// 1. 顶部摘要汇总
		size_t totalUsed = 0;
		size_t totalReserved = 0;
		for (const auto& s : snapshots)
		{
			totalUsed += s.UsedBytes;
			totalReserved += s.TotalReserved;
		}

		ImGui::Text("Total Engine Usage: %s / %s",
			FormatBytes(totalUsed).c_str(), FormatBytes(totalReserved).c_str());
		ImGui::Separator();

		static ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
			ImGuiTableFlags_Hideable | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;

		if (ImGui::BeginTable("AllocatorsTable", 5, flags))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Progress / Bar", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Allocs", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ImGui::TableHeadersRow();

			for (const auto& s : snapshots)
			{
				ImGui::TableNextRow();

				// --- 1. 名称 ---
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", s.Name);

				// --- 2. 类型 (适配 DualTrack) ---
				ImGui::TableSetColumnIndex(1);
				ImVec4 typeColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // 默认白色
				const char* typeStr = "Unknown";

				switch (s.Type)
				{
					case World::AllocatorType::Stack:
						typeColor = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // 黄色
						typeStr = "Stack";
						break;
					case World::AllocatorType::Pool:
						typeColor = ImVec4(0.0f, 0.8f, 1.0f, 1.0f); // 青色
						typeStr = "Pool";
						break;
					case World::AllocatorType::DualTrack:
						typeColor = ImVec4(0.7f, 0.4f, 1.0f, 1.0f); // 紫色 (DualTrack 专属)
						typeStr = "DualTrack";
						break;
					case World::AllocatorType::Linear:
						typeColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); // 灰色
						typeStr = "Linear";
						break;
				}
				ImGui::TextColored(typeColor, "%s", typeStr);

				// --- 3. 内存数值 ---
				ImGui::TableSetColumnIndex(2);
				ImGui::Text("%s / %s", FormatBytes(s.UsedBytes).c_str(), FormatBytes(s.TotalReserved).c_str());

				// --- 4. 进度条 (逻辑适配) ---
				ImGui::TableSetColumnIndex(3);
				float fraction = (s.TotalReserved > 0) ? (float)s.UsedBytes / (float)s.TotalReserved : 0.0f;

				// 根据类型决定进度条颜色
				ImVec4 barColor;
				if (s.Type == World::AllocatorType::DualTrack)
					barColor = ImVec4(0.5f, 0.2f, 0.8f, 0.8f); // 深紫色
				else if (s.Type == World::AllocatorType::Stack)
					barColor = ImVec4(0.9f, 0.5f, 0.1f, 0.8f); // 橙色
				else
					barColor = ImVec4(0.2f, 0.7f, 0.3f, 0.8f); // 绿色

				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);

				// 如果内存占用超过 90%，显示警告样式
				char buf[32];
				sprintf(buf, "%d%%", (int)(fraction * 100));
				ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), buf);

				ImGui::PopStyleColor();

				// --- 5. 分配次数 ---
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%llu", s.NumAllocations);
			}
			ImGui::EndTable();

		}

		ImGui::End();

		// Cleanup: 移除所有分配器的短命快照，保持界面简洁
		MemoryTracker::Get().ClearEphemeralStats();
	}
	void MemoryTraceLayer::OnEvent(Event& event)
	{}


}