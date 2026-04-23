#include "ContentBrowserPanel.h"

namespace World
{
	ContentBrowserPanel::ContentBrowserPanel()
		: m_CurrentDirectory(m_RootDirectory)
	{
		m_DirectoryIcon = Texture2D::Create("Resource/Icons/ContentBrowser/DirectoryIcon.png");
		m_FileIcon = Texture2D::Create("Resource/Icons/ContentBrowser/FileIcon.png");
	}

	void ContentBrowserPanel::OnImGuiRender()
	{
		ImGui::Begin("Content Browser");

		DrawNavigationBar();

		// 可滚动的图标区域
		if (ImGui::BeginChild("ContentGridArea", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar))
		{
			DrawContentsGrid();


			if (ImGui::BeginPopupContextWindow(0, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
			{
				DrawContextMenu(); // 调用刚才写的菜单绘制逻辑
				ImGui::EndPopup();
			}

			ImGui::EndChild();
		}

		// 删除确认弹窗
		DrawDeleteConfirmationModal();

		ImGui::End();

	}
	void ContentBrowserPanel::RegisterOpenAction(const std::string& extension, OpenActionCallback action)
	{
		// 统一转为小写扩展名以便后续精确匹配
		std::string ext = extension;
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		m_OpenActions[ext] = action;
	}
	void ContentBrowserPanel::OpenItem(const std::filesystem::path& path)
	{
		// =========================================================
		// 1. 文件夹处理：进入工作目录深层
		// =========================================================
		if (std::filesystem::is_directory(path))
		{
			m_CurrentDirectory = path;
			m_SelectedItems.clear();
			m_LastSelectedItem.clear();
			return;
		}

		// =========================================================
		// 2. 文件处理：检索并匹配已注册业务回调
		// =========================================================
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

		if (m_OpenActions.find(extension) != m_OpenActions.end())
		{
			m_OpenActions[extension](path);
		}
		else
		{
			// 系统默认后备：将未纳管类型的文档抛给 Windows 打开
			std::string filepath = std::filesystem::absolute(path).string();
			std::string command = "start \"\" \"" + filepath + "\"";
			system(command.c_str());
		}
	}

	void ContentBrowserPanel::DrawNavigationBar()
	{
		// 开启横向排列模式
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2 { 4.0f, 4.0f });

		// =========================================================
		// 模块A: 拖放接收器 (Drop Target) => 面包屑级别的灵活放置回调
		// =========================================================
		auto dragDropTarget = [this](const std::filesystem::path& dropPath)
			{
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
					{
						const wchar_t* payloadStr = (const wchar_t*)payload->Data;
						std::filesystem::path draggedPath = m_RootDirectory / payloadStr;

						// 不可将自身扔进自身内，也不响应平层位置移动
						if (draggedPath != dropPath && draggedPath.parent_path() != dropPath)
						{
							try
							{
								std::filesystem::rename(draggedPath, dropPath / draggedPath.filename());
								if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();
							}
							catch (const std::exception& e)
							{
								WLD_CORE_ERROR("Could not move item: {0}", e.what());
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			};

		// =========================================================
		// 模块B: 后退根按钮
		// =========================================================
		bool isAtRoot = (m_CurrentDirectory == m_RootDirectory);
		if (isAtRoot) ImGui::BeginDisabled();

		if (ImGui::Button("<"))
		{
			m_CurrentDirectory = m_CurrentDirectory.parent_path();
		}
		if (!isAtRoot) dragDropTarget(m_CurrentDirectory.parent_path());

		if (isAtRoot) ImGui::EndDisabled();

		ImGui::SameLine();

		// =========================================================
		// 模块C: 层级面包屑跟踪 (Breadcrumbs)
		// =========================================================
		std::filesystem::path relativePath = std::filesystem::relative(m_CurrentDirectory, m_RootDirectory);
		std::filesystem::path accumulatedPath = m_RootDirectory;

		if (ImGui::Button("Assets"))
		{
			m_CurrentDirectory = m_RootDirectory;
		}

		dragDropTarget(m_RootDirectory);

		for (const auto& part : relativePath)
		{
			if (part.empty() || part == ".") continue;

			accumulatedPath /= part;

			ImGui::SameLine();
			ImGui::TextDisabled(">");
			ImGui::SameLine();

			if (ImGui::Button(part.string().c_str()))
			{
				m_CurrentDirectory = accumulatedPath;
			}

			dragDropTarget(accumulatedPath);
		}

		// =========================================================
		// 模块D: 对齐末端的搜选输入器
		// =========================================================
		float searchBarWidth = 150.0f;
		float clearButtonWidth = 24.0f;
		float availableWidth = ImGui::GetContentRegionAvail().x;
		auto& style = ImGui::GetStyle();

		if (availableWidth > searchBarWidth + 50.0f)
		{
			// 计算并保持其尾部固定贴边
			float totalWidgetsWidth = searchBarWidth;
			if (m_SearchBuffer[0] != '\0')
				totalWidgetsWidth += style.ItemSpacing.x + clearButtonWidth;

			float rightEdge = ImGui::GetContentRegionMax().x;
			ImGui::SameLine(rightEdge - totalWidgetsWidth);
			ImGui::SetNextItemWidth(searchBarWidth);
			if (ImGui::InputTextWithHint("##Search", "Search...", m_SearchBuffer, sizeof(m_SearchBuffer)))
			{
				UpdateSearchCache();
			}

			if (m_SearchBuffer[0] != '\0')
			{
				ImGui::SameLine();
				if (ImGui::Button("X", ImVec2(clearButtonWidth, 0)))
				{
					m_SearchBuffer[0] = '\0';
					UpdateSearchCache();
				}
			}

		}

		ImGui::PopStyleVar();
		ImGui::Separator();
		ImGui::Spacing();
	}
	void ContentBrowserPanel::DrawContentsGrid()
	{
		static float padding = 16.0f;
		static float thumbnailSize = 128.0f;
		float cellSize = thumbnailSize + padding;

		float panelWidth = ImGui::GetContentRegionAvail().x;
		int columnCount = (int)(panelWidth / cellSize);
		if (columnCount < 1) columnCount = 1;

		// =========================================================
		// 加载显示资产单元格并处理搜索拦截过滤
		// =========================================================
		if (ImGui::BeginTable("ContentGrid", columnCount))
		{
			bool isSearching = strlen(m_SearchBuffer) > 0;

			std::vector<std::filesystem::path> currentDisplayPaths;

			if (isSearching)
			{
				currentDisplayPaths = m_SearchResults;
			}
			else
			{
				for (auto& directoryEntry : std::filesystem::directory_iterator(m_CurrentDirectory))
				{
					currentDisplayPaths.push_back(directoryEntry.path());
				}
				// 为了 Shift 连选结果直观预判，统一使用原级字母序列
				std::sort(currentDisplayPaths.begin(), currentDisplayPaths.end());
			}

			for (const auto& path : currentDisplayPaths)
			{
				std::filesystem::directory_entry entry(path);
				RenderItem(entry, isSearching, currentDisplayPaths);
			}

			ImGui::EndTable();
		}

		// =========================================================
		// 网格背景点击侦听：实现空白解除选择焦点
		// =========================================================
		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			m_SelectedItems.clear();
		}
	}
	void ContentBrowserPanel::UpdateSearchCache()
	{
		m_SearchResults.clear();
		std::string query = m_SearchBuffer;
		std::transform(query.begin(), query.end(), query.begin(), ::tolower);

		// =========================================================
		// 递归向下遍历搜索（将名称打平为全小写配对）
		// =========================================================
		for (auto& entry : std::filesystem::recursive_directory_iterator(m_CurrentDirectory))
		{
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);

			if (name.find(query) != std::string::npos)
				m_SearchResults.push_back(entry.path());
		}
	}
	void ContentBrowserPanel::RenderItem(const std::filesystem::directory_entry& entry, bool showPath, const std::vector<std::filesystem::path>& currentDisplayPaths)
	{
		static float thumbnailSize = 128.0f;
		const auto& path = entry.path();
		std::string filename = path.filename().string();

		ImGui::TableNextColumn();
		ImGui::PushID(path.string().c_str());

		Ref<Texture2D> icon = entry.is_directory() ? m_DirectoryIcon : m_FileIcon;

		// =========================================================
		// 1. 图标样式覆盖与状态呈现 
		// =========================================================
		bool isSelected = m_SelectedItems.find(path) != m_SelectedItems.end();

		// 被选中资产着色深灰高亮
		if (isSelected)
			ImGui::PushStyleColor(ImGuiCol_Button, { 0.3f, 0.3f, 0.3f, 0.5f });
		else
			ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });


		bool isCut = (m_ClipboardAction == ClipboardAction::Cut) &&
			(std::find(m_Clipboard.begin(), m_Clipboard.end(), path) != m_Clipboard.end());

		// 剪切状态产生虚化半透明效果以区分行为
		ImVec4 tintColor = isCut ? ImVec4(1.0f, 1.0f, 1.0f, 0.5f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

		ImGui::ImageButton((ImTextureID)icon->GetRendererID(), { thumbnailSize, thumbnailSize }, { 0, 1 }, { 1, 0 }, -1, ImVec4(0, 0, 0, 0), tintColor);

		// =========================================================
		// 2. 发起跨模块拖出处理 (Drag Source)
		// =========================================================
		if (ImGui::BeginDragDropSource())
		{
			// 计算资产相对环境结构的位地址用于数据包分发
			std::filesystem::path relativePath = std::filesystem::relative(path, m_RootDirectory);
			const wchar_t* itemPath = relativePath.c_str();

			ImGui::SetDragDropPayload("CONTENT_BROWSER_ITEM", itemPath, (wcslen(itemPath) + 1) * sizeof(wchar_t), ImGuiCond_Once);

			// 持续渲染跟随鼠标的半透明缩略图及标识符
			ImGui::Image((ImTextureID)icon->GetRendererID(), { 32.0f, 32.0f }, { 0, 1 }, { 1, 0 });
			ImGui::SameLine();
			ImGui::TextUnformatted(filename.c_str());

			ImGui::EndDragDropSource();
		}

		// =========================================================
		// 3. 承接拖入重组处理 (Drop Target - 仅限合法目录)
		// =========================================================
		if (entry.is_directory() && ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CONTENT_BROWSER_ITEM"))
			{
				const wchar_t* payloadStr = (const wchar_t*)payload->Data;
				std::filesystem::path draggedPath = m_RootDirectory / payloadStr;

				// 严禁文件夹吞噬自身
				if (draggedPath != path)
				{
					try
					{
						std::filesystem::rename(draggedPath, path / draggedPath.filename());

						if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();
					}
					catch (const std::exception& e)
					{
						WLD_CORE_ERROR("Could not move item: {0}", e.what());
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		// =========================================================
		// 4. 输入设备事件分流处理（单双击 / Ctrl&Shift组合）
		// =========================================================
		if (ImGui::IsItemHovered())
		{
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				OpenItem(path);
			}
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				bool ctrl = ImGui::GetIO().KeyCtrl;
				bool shift = ImGui::GetIO().KeyShift;

				if (ctrl)
				{
					// Ctrl：游离单点取反
					if (isSelected) m_SelectedItems.erase(path);
					else m_SelectedItems.insert(path);
					m_LastSelectedItem = path;
				}
				else if (shift && !m_LastSelectedItem.empty())
				{
					// Shift：计算数组序列区间全量选中
					auto startIt = std::find(currentDisplayPaths.begin(), currentDisplayPaths.end(), m_LastSelectedItem);
					auto endIt = std::find(currentDisplayPaths.begin(), currentDisplayPaths.end(), path);

					if (startIt != currentDisplayPaths.end() && endIt != currentDisplayPaths.end())
					{
						// 若无Ctrl陪同则冲刷清空旧有状态
						if (!ctrl) m_SelectedItems.clear();

						if (std::distance(startIt, endIt) < 0) std::swap(startIt, endIt);
						for (auto it = startIt; it <= endIt; ++it)
						{
							m_SelectedItems.insert(*it);
						}
					}
				}
				else
				{
					// 普通左键单击：唯一标记排他
					m_SelectedItems.clear();
					m_SelectedItems.insert(path);
					m_LastSelectedItem = path;
				}
			}
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
			{
				// 右键提前强选，确保目标始终在弹出操作名单内
				if (!isSelected)
				{
					m_SelectedItems.clear();
					m_SelectedItems.insert(path);
					m_LastSelectedItem = path;
				}
			}
		}

		// =========================================================
		// 5. 资产专属：右键上下文操作菜单
		// =========================================================
		if (ImGui::BeginPopupContextItem("ItemContextMenu"))
		{
			ImGui::TextDisabled("Selected: %Iu item(s)", m_SelectedItems.size());
			ImGui::Separator();

			if (ImGui::MenuItem("Open"))
			{
				// 转 dump 为临时拷贝队列以防 OpenItem 的迭代器崩溃
				std::vector<std::filesystem::path> itemsToOpen(m_SelectedItems.begin(), m_SelectedItems.end());

				for (const auto& selectedPath : itemsToOpen)
				{
					OpenItem(selectedPath);

					if (std::filesystem::is_directory(selectedPath)) break;
				}
			}

			if (ImGui::MenuItem("Cut"))
			{
				m_Clipboard.assign(m_SelectedItems.begin(), m_SelectedItems.end());
				m_ClipboardAction = ClipboardAction::Cut;
			}
			if (ImGui::MenuItem("Copy"))
			{
				m_Clipboard.assign(m_SelectedItems.begin(), m_SelectedItems.end());
				m_ClipboardAction = ClipboardAction::Copy;
			}

			bool hasClipboardData = !m_Clipboard.empty() && m_ClipboardAction != ClipboardAction::None;

			// 提示内容增强展示当前持有批量的体积
			std::string pasteIntoText = "Paste Into";
			if (hasClipboardData)
				pasteIntoText += " (" + std::to_string(m_Clipboard.size()) + " items)";

			if (entry.is_directory() && ImGui::MenuItem(pasteIntoText.c_str(), nullptr, false, hasClipboardData))
			{
				PasteCopiedItems(path);
			}

			// 重命名互斥锁：必须且仅能同时操作单一项
			if (ImGui::MenuItem("Rename", nullptr, false, m_SelectedItems.size() == 1))
			{
				m_ItemToRename = *m_SelectedItems.begin();
				strncpy(m_RenameBuffer, m_ItemToRename.filename().string().c_str(), sizeof(m_RenameBuffer));

			}
			if (ImGui::MenuItem("Open in Explorer"))
			{
				for (const auto& selectedPath : m_SelectedItems)
				{
					std::string filepath = std::filesystem::absolute(selectedPath).string();

					if (std::filesystem::is_directory(selectedPath))
					{
						std::string command = "explorer \"" + filepath + "\"";
						system(command.c_str());
					}
					else
					{
						std::string command = "explorer /select,\"" + filepath + "\"";
						system(command.c_str());
					}
				}
			}
			if (ImGui::MenuItem("Delete"))
			{
				m_ShowDeleteModal = true;
			}
			ImGui::EndPopup();
		}

		if (showPath)
		{
			std::string subPath = std::filesystem::relative(path, m_CurrentDirectory).parent_path().string();
			if (!subPath.empty())
			{
				ImGui::TextDisabled("(in %s)", subPath.c_str());
			}
		}

		// =========================================================
		// 6. 重命名活动焦框或文本标签收尾渲染
		// =========================================================
		if (m_ItemToRename == path)
		{
			// 对齐视觉宽度确保不会偏移出图标
			ImGui::PushItemWidth(thumbnailSize);

			ImGui::SetKeyboardFocusHere();

			int inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
				ImGuiInputTextFlags_AutoSelectAll |
				ImGuiInputTextFlags_CallbackCharFilter;

			// 拦截底层系统非法语义字符注入
			auto nameFilterCallback = [](ImGuiInputTextCallbackData* data) -> int
				{
					if (data->EventChar == ' ' ||
						data->EventChar == '\\' || data->EventChar == '/' ||
						data->EventChar == ':' || data->EventChar == '*' ||
						data->EventChar == '?' || data->EventChar == '"' ||
						data->EventChar == '<' || data->EventChar == '>' ||
						data->EventChar == '|')
					{
						return 1;
					}
					return 0;
				};

			if (ImGui::InputText("##Rename", m_RenameBuffer, sizeof(m_RenameBuffer), inputFlags, nameFilterCallback))
			{
				try
				{
					std::filesystem::path newPath = path.parent_path() / m_RenameBuffer;
					if (path != newPath)
					{
						std::filesystem::rename(path, newPath);
						if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();
					}
				}
				catch (const std::exception& e)
				{
					WLD_CORE_ERROR("Could not rename file: {0}", e.what());
				}

				m_ItemToRename.clear();
				m_SelectedItems.clear();
			}
			// 焦点撤销代表放弃保存结果
			else if (ImGui::IsItemDeactivated() && ImGui::IsWindowFocused())
			{
				m_ItemToRename.clear();
			}

			ImGui::PopItemWidth();
		}
		else
		{
			// 常规标签呈现剪切发灰状态或原状
			if (isCut) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
			ImGui::TextWrapped(filename.c_str());
			if (isCut) ImGui::PopStyleColor();
		}

		ImGui::PopStyleColor();
		ImGui::PopID();
	}

	void ContentBrowserPanel::DrawContextMenu()
	{
		// =========================================================
		// 右键空白区域：基础菜单栏入口
		// =========================================================
		if (ImGui::BeginMenu("New"))
		{
			// 发起常规生成功能
			if (ImGui::MenuItem("Folder"))
			{
				CreateNewDirectory("NewFolder");
			}

			ImGui::EndMenu();
		}

		ImGui::Separator();

		// =========================================================
		// 动态显示通用粘贴项及剪贴板情况
		// =========================================================
		bool hasClipboardData = !m_Clipboard.empty() && m_ClipboardAction != ClipboardAction::None;
		std::string pasteText = "Paste";
		if (hasClipboardData)
			pasteText += " (" + std::to_string(m_Clipboard.size()) + " items)";

		if (ImGui::MenuItem(pasteText.c_str(), nullptr, false, hasClipboardData))
		{
			PasteCopiedItems(m_CurrentDirectory);
		}

		// =========================================================
		// 更新视图索引与本地原生互操作
		// =========================================================
		if (ImGui::MenuItem("Refresh"))
		{
			if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();
		}

		if (ImGui::MenuItem("Open in Explorer"))
		{
			std::string folderpath = std::filesystem::absolute(m_CurrentDirectory).string();
			std::string command = "explorer \"" + folderpath + "\"";
			system(command.c_str());
		}
	}
	void ContentBrowserPanel::CreateNewDirectory(const std::string& name)
	{
		std::filesystem::path newPath = m_CurrentDirectory / name;

		// =========================================================
		// 递增索引推导防干涉算法 ("Name (1)...(N)")
		// =========================================================
		int counter = 1;
		while (std::filesystem::exists(newPath))
		{
			newPath = m_CurrentDirectory / (name + " (" + std::to_string(counter) + ")");
			counter++;
		}

		// 触发系统 IO 及缓存状态响应映射
		try
		{
			std::filesystem::create_directory(newPath);

			if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();

			// 使新生目录自然处于待办高亮编辑态
			m_ItemToRename = newPath;
			strncpy(m_RenameBuffer, newPath.filename().string().c_str(), sizeof(m_RenameBuffer));

			m_SelectedItems.clear();
			m_SelectedItems.insert(newPath);
			m_LastSelectedItem = newPath;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_ERROR("Could not create directory: {0}", e.what());
		}
	}
	void ContentBrowserPanel::DrawDeleteConfirmationModal()
	{
		if (m_ShowDeleteModal)
		{
			ImGui::OpenPopup("Delete Confirmation");
			m_ShowDeleteModal = false; // 取消高频轮询保证弹出稳定
		}

		// =========================================================
		// 居中不可逆损毁警告主界面 (Modal Popup)
		// =========================================================
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		if (ImGui::BeginPopupModal("Delete Confirmation", NULL, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("Are you sure you want to delete the following %Iu item(s)?", m_SelectedItems.size());
			ImGui::Spacing();

			// 提供带边缘约束和过载上限滚动的路径详细反馈
			ImGui::BeginChild("ItemsToDelete", ImVec2(350, 150), true);
			for (const auto& selectedPath : m_SelectedItems)
			{
				if (std::filesystem::is_directory(selectedPath))
				{
					// 父级目录结构说明
					ImGui::TextDisabled("[Folder]");
					ImGui::SameLine();
					ImGui::TextUnformatted(selectedPath.filename().string().c_str());

					// 提供相对缩进以强调包含级别属性
					ImGui::Indent(20.0f);
					int displayCount = 0;
					try
					{
						// 解包子层级内容并拦截深海无限加载保护UI主线程
						for (const auto& entry : std::filesystem::recursive_directory_iterator(selectedPath))
						{
							if (displayCount >= 50)
							{
								// 限制同类信息刷新阈值
								ImGui::TextDisabled("... and more hidden items");
								break;
							}

							if (entry.is_directory()) ImGui::TextDisabled("[Folder]");
							else ImGui::TextDisabled("[File]");

							ImGui::SameLine();

							// 展现简化内构相对树层
							std::string relPath = std::filesystem::relative(entry.path(), selectedPath).string();
							ImGui::TextUnformatted(relPath.c_str());

							displayCount++;
						}
					}
					catch (const std::exception& e)
					{
						ImGui::TextColored(ImVec4(1, 0, 0, 1), "(Error reading contents)");
					}
					// 恢复排版定位防止全局结构偏转
					ImGui::Unindent(20.0f);
				}
				else
				{
					// 文件单体输出说明
					ImGui::TextDisabled("[File]");
					ImGui::SameLine();
					ImGui::TextUnformatted(selectedPath.filename().string().c_str());
				}
			}
			ImGui::EndChild();

			ImGui::Spacing();
			// 发出鲜红警戒反馈告知危险级别
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "This action cannot be undone!");
			ImGui::Separator();
			ImGui::Spacing();

			// --- 执行抹除交互按钮 ---
			if (ImGui::Button("Yes", ImVec2(120, 0)))
			{
				for (const auto& selectedPath : m_SelectedItems)
				{
					try
					{
						std::filesystem::remove_all(selectedPath);
					}
					catch (const std::exception& e)
					{
						WLD_CORE_ERROR("Could not delete {0}: {1}", selectedPath.string(), e.what());
					}
				}
				m_SelectedItems.clear();
				m_LastSelectedItem.clear();

				if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();

				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine(0, 60);

			// --- 中断并关停交互按钮 ---
			if (ImGui::Button("No", ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
			}

			// 防止回车失误自动强聚焦点指向安全区
			ImGui::SetItemDefaultFocus();

			ImGui::EndPopup();
		}
	}
	void ContentBrowserPanel::PasteCopiedItems(const std::filesystem::path& destination)
	{
		// 校验剪切存在合理状态
		if (m_Clipboard.empty() || m_ClipboardAction == ClipboardAction::None)
			return;

		// =========================================================
		// 基于物理系统接口完成迁移或原版复印组装
		// =========================================================
		for (const auto& path : m_Clipboard)
		{
			// 取消外部修改产生的废弃项绑定
			if (!std::filesystem::exists(path)) continue;

			std::filesystem::path destPath = destination / path.filename();

			try
			{
				if (m_ClipboardAction == ClipboardAction::Cut)
				{
					// 跳过已存在同位置重复消耗的操作
					if (path == destPath) continue;

					// 快速转移原生体
					std::filesystem::rename(path, destPath);
				}
				else if (m_ClipboardAction == ClipboardAction::Copy)
				{
					// 解构元数据名称及后缀以便组建派生名称规避冲突
					std::string filename = path.stem().string();
					std::string extension = path.extension().string();
					int counter = 1;
					while (std::filesystem::exists(destPath))
					{
						destPath = destination / (filename + "-Copy(" + std::to_string(counter) + ")" + extension);
						counter++;
					}

					// 推送包含内部结构的所有节点复制业务
					std::filesystem::copy(path, destPath, std::filesystem::copy_options::recursive);
				}
			}
			catch (const std::exception& e)
			{
				// 执行抛弃或拒绝权限时阻断记录
				WLD_CORE_ERROR("Clipboard operation failed: {0}", e.what());
			}
		}

		// =========================================================
		// Windows 标准机制：释放已经应用成功搬迁的源内存印记
		// =========================================================
		if (m_ClipboardAction == ClipboardAction::Cut)
		{
			m_Clipboard.clear();
			m_ClipboardAction = ClipboardAction::None;
		}

		if (strlen(m_SearchBuffer) > 0) UpdateSearchCache();
	}
}