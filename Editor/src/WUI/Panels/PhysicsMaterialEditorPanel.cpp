#include <glm/vec2.hpp>
#include "wldpch.h"
#include "WUI/Panels/PhysicsMaterialEditorPanel.h"

#include "World/Core/Log.h"
#include "World/Utils/Paths.h"
#include "World/Core/KeyCodes.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/WuiWidgets.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace World
{
	std::string PhysicsMaterialEditorPanel::s_PendingOpenPath;

	void PhysicsMaterialEditorPanel::RequestOpenAsset(const std::string& path)
	{
		s_PendingOpenPath = path;
	}

	static void OpenInExplorer(const std::filesystem::path& path)
	{
#ifdef _WIN32
		std::error_code ec;
		std::filesystem::path target = std::filesystem::absolute(path);
		if (std::filesystem::exists(target, ec) && !std::filesystem::is_directory(target, ec))
		{
			const std::wstring parameters = L"/select,\"" + target.wstring() + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		}
		else
		{
			std::filesystem::path parent = target.parent_path();
			if (std::filesystem::exists(parent, ec))
			{
				ShellExecuteW(nullptr, L"open", parent.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		}
#else
		(void)path;
#endif
	}

	void PhysicsMaterialEditorPanel::RefreshAvailableAssets()
	{
		m_AvailableAssets.clear();
		std::error_code ec;
		const std::filesystem::path assetRoot = World::Paths::AssetRoot();
		if (!std::filesystem::exists(assetRoot, ec))
			return;

		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot, ec))
		{
			if (entry.is_regular_file(ec) && entry.path().extension() == ".wphysmat")
			{
				std::error_code relEc;
				std::filesystem::path rel = std::filesystem::relative(entry.path(), assetRoot, relEc);
				m_AvailableAssets.push_back(relEc ? entry.path().generic_string() : rel.generic_string());
			}
		}
	}

	void PhysicsMaterialEditorPanel::LoadAsset(const std::string& path)
	{
		std::string normalized = path;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');

		std::filesystem::path target(normalized);
		if (!target.is_absolute() && !std::filesystem::exists(target))
		{
			std::filesystem::path full = World::Paths::AssetRoot() / target;
			if (std::filesystem::exists(full))
				target = full;
			else if (normalized.rfind("assets/", 0) == 0 || normalized.rfind("assets\\", 0) == 0)
			{
				std::filesystem::path stripped = World::Paths::AssetRoot() / normalized.substr(7);
				if (std::filesystem::exists(stripped))
					target = stripped;
			}
		}

		PhysicsMaterial loaded;
		if (PhysicsMaterial::LoadFromFile(target.string(), loaded))
		{
			m_Material = loaded;
			m_LoadedPath = normalized;
			m_Dirty = false;
			m_StatusFeedback = "已载入物理材质: " + normalized;
			WLD_CORE_INFO("[physmat] Loaded physics material asset: {0}", normalized);
		}
		else
		{
			m_StatusFeedback = "载入物理材质失败: " + normalized;
			WLD_CORE_WARN("[physmat] Failed to load physics material asset: {0}", normalized);
		}
	}

	void PhysicsMaterialEditorPanel::SaveAsset()
	{
		if (m_LoadedPath.empty())
		{
			m_StatusFeedback = "未指定保存路径。";
			return;
		}

		std::filesystem::path target(m_LoadedPath);
		if (!target.is_absolute())
		{
			if (m_LoadedPath.rfind("assets/", 0) == 0 || m_LoadedPath.rfind("assets\\", 0) == 0)
				target = World::Paths::AssetRoot() / m_LoadedPath.substr(7);
			else
				target = World::Paths::AssetRoot() / target;
		}

		std::error_code ec;
		std::filesystem::create_directories(target.parent_path(), ec);

		if (m_Material.SaveToFile(target.string()))
		{
			m_Dirty = false;
			m_StatusFeedback = "已成功保存物理材质: " + m_LoadedPath;
			WLD_CORE_INFO("[physmat] Saved physics material: {0}", target.string());
		}
		else
		{
			m_StatusFeedback = "保存物理材质失败: " + target.string();
			WLD_CORE_ERROR("[physmat] Failed to save physics material: {0}", target.string());
		}
	}

	void PhysicsMaterialEditorPanel::ApplyPreset(const std::string& name)
	{
		if (name == "Default")
		{
			m_Material.Id = "PhysMat_Default";
			m_Material.Friction = 0.5f;
			m_Material.Restitution = 0.0f;
			m_Material.FrictionCombine = PhysMatCombineMode::Average;
			m_Material.RestitutionCombine = PhysMatCombineMode::Average;
		}
		else if (name == "Ice")
		{
			m_Material.Id = "PhysMat_Ice";
			m_Material.Friction = 0.05f;
			m_Material.Restitution = 0.05f;
			m_Material.FrictionCombine = PhysMatCombineMode::Minimum;
			m_Material.RestitutionCombine = PhysMatCombineMode::Minimum;
		}
		else if (name == "Rubber")
		{
			m_Material.Id = "PhysMat_BouncyRubber";
			m_Material.Friction = 0.85f;
			m_Material.Restitution = 0.90f;
			m_Material.FrictionCombine = PhysMatCombineMode::Average;
			m_Material.RestitutionCombine = PhysMatCombineMode::Maximum;
		}
		else if (name == "Metal")
		{
			m_Material.Id = "PhysMat_Metal";
			m_Material.Friction = 0.40f;
			m_Material.Restitution = 0.15f;
			m_Material.FrictionCombine = PhysMatCombineMode::Average;
			m_Material.RestitutionCombine = PhysMatCombineMode::Average;
		}
		else if (name == "Wood")
		{
			m_Material.Id = "PhysMat_Wood";
			m_Material.Friction = 0.60f;
			m_Material.Restitution = 0.10f;
			m_Material.FrictionCombine = PhysMatCombineMode::Average;
			m_Material.RestitutionCombine = PhysMatCombineMode::Average;
		}
		else if (name == "Mud")
		{
			m_Material.Id = "PhysMat_Mud";
			m_Material.Friction = 1.30f;
			m_Material.Restitution = 0.0f;
			m_Material.FrictionCombine = PhysMatCombineMode::Maximum;
			m_Material.RestitutionCombine = PhysMatCombineMode::Minimum;
		}
		m_Dirty = true;
		m_StatusFeedback = "已应用预设: " + name;
	}

	bool PhysicsMaterialEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)shift;
		(void)alt;
		if (ctrl && keyCode == KeyCodes::S)
		{
			SaveAsset();
			return true;
		}
		return false;
	}

	void PhysicsMaterialEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg);

		if (!s_PendingOpenPath.empty())
		{
			std::string p = s_PendingOpenPath;
			s_PendingOpenPath.clear();
			LoadAsset(p);
		}

		if (m_AvailableAssets.empty())
		{
			RefreshAvailableAssets();
			if (m_LoadedPath.empty() && !m_AvailableAssets.empty())
			{
				LoadAsset(m_AvailableAssets[0]);
			}
		}

		float curY = rect.Y + 8.0f;
		const float padX = 12.0f;
		const float contentW = rect.W - padX * 2.0f;

		// 1. Top Bar: Title & Save/Explorer Actions
		const Wui::WuiRect topBar { rect.X + padX, curY, contentW, 36.0f };
		Wui::BarSurface(ctx, topBar, theme.PanelHeader, theme.Border);

		Wui::Label(ctx, { topBar.X + 12.0f, topBar.Y + 9.0f }, "物理材质编辑器 (Physics Material Editor)", theme.Accent, 13.5f);

		std::string titleTag = m_LoadedPath.empty() ? "[未选择资产]" : (m_LoadedPath + (m_Dirty ? " *" : ""));
		Wui::Label(ctx, { topBar.X + 270.0f, topBar.Y + 10.0f }, "当前文件: " + titleTag, m_Dirty ? Wui::WuiColor { 1.0f, 0.7f, 0.2f, 1.0f } : theme.TextMuted, 11.5f);

		// Save Button
		const float btnW = 90.0f;
		const Wui::WuiRect saveBtnRect { topBar.X + topBar.W - btnW * 2.0f - 16.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, saveBtnRect, "保存当前物理材质资产修改 (Ctrl+S)");
		if (Wui::Button(ctx, Wui::HashId("pmat.btn.save"), saveBtnRect, m_Dirty ? "保存资产 *" : "保存资产", theme))
		{
			SaveAsset();
		}

		// Explorer Button
		const Wui::WuiRect expBtnRect { topBar.X + topBar.W - btnW - 8.0f, topBar.Y + 6.0f, btnW, 24.0f };
		Wui::Tooltip(ctx, expBtnRect, "在 Windows 资源管理器中打开此物理材质文件并高亮定位");
		if (Wui::Button(ctx, Wui::HashId("pmat.btn.explorer"), expBtnRect, "定位文件", theme))
		{
			std::filesystem::path full = m_LoadedPath;
			if (!full.is_absolute())
			{
				if (m_LoadedPath.rfind("assets/", 0) == 0 || m_LoadedPath.rfind("assets\\", 0) == 0)
					full = World::Paths::AssetRoot() / m_LoadedPath.substr(7);
				else
					full = World::Paths::AssetRoot() / full;
			}
			OpenInExplorer(full);
			m_StatusFeedback = "已在资源管理器中定位: " + full.string();
		}

		curY += 42.0f;

		// 2. Presets Quick Toolbar
		const Wui::WuiRect presetBar { rect.X + padX, curY, contentW, 32.0f };
		Wui::PanelBackground(ctx, presetBar, theme.ContentBg, 4.0f);
		Wui::Label(ctx, { presetBar.X + 8.0f, presetBar.Y + 8.0f }, "快速预设:", theme.TextMuted, 11.5f);

		float pX = presetBar.X + 70.0f;
		const struct { const char* Name; const char* Tag; } kPresets[] = {
			{ "默认 (Default)", "Default" },
			{ "冰面 (Ice)", "Ice" },
			{ "高弹橡胶 (Rubber)", "Rubber" },
			{ "金属 (Metal)", "Metal" },
			{ "木材 (Wood)", "Wood" },
			{ "泥泞 (Mud)", "Mud" }
		};

		for (size_t i = 0; i < 6; ++i)
		{
			const Wui::WuiRect pBtnRect { pX, presetBar.Y + 4.0f, 96.0f, 24.0f };
			if (Wui::Button(ctx, Wui::HashId(("pmat.preset." + std::to_string(i)).c_str()), pBtnRect, kPresets[i].Name, theme))
			{
				ApplyPreset(kPresets[i].Tag);
			}
			pX += 102.0f;
		}

		curY += 38.0f;

		// 3. Main Parameter Configuration Card
		const float cardW = contentW;
		const Wui::WuiRect cardRect { rect.X + padX, curY, cardW, 205.0f };
		Wui::PanelBackground(ctx, cardRect, theme.ContentBg, 6.0f);

		float rowY = cardRect.Y + 12.0f;
		const float labelX = cardRect.X + 16.0f;
		const float fieldX = cardRect.X + 180.0f;
		const float fieldW = cardW - 200.0f;

		// Row 1: Identifier Id
		Wui::Label(ctx, { labelX, rowY + 3.0f }, "材质标识 (Id):", theme.Text, 12.0f);
		const Wui::WuiRect idFieldRect { fieldX, rowY, fieldW, 22.0f };
		std::string oldId = m_Material.Id;
		if (Wui::TextField(ctx, Wui::HashId("pmat.field.id"), idFieldRect, m_Material.Id, theme) && m_Material.Id != oldId)
		{
			m_Dirty = true;
		}
		rowY += 32.0f;

		// Row 2: Friction
		std::ostringstream ssF;
		ssF << std::fixed << std::setprecision(2) << m_Material.Friction;
		std::string fText = "滑动摩擦力 (Friction): " + ssF.str();
		Wui::Label(ctx, { labelX, rowY + 3.0f }, fText, theme.Text, 12.0f);
		const Wui::WuiRect fSliderRect { fieldX, rowY, fieldW, 22.0f };
		Wui::Tooltip(ctx, fSliderRect, "表面摩擦系数 (0.0 为完全无阻力冰面, 0.5 为普通接触面, 1.0+ 为粗糙阻滞表面)");
		float beforeF = m_Material.Friction;
		Wui::SliderFloat(ctx, Wui::HashId("pmat.slider.f"), fSliderRect, m_Material.Friction, 0.0f, 2.0f, theme);
		if (std::abs(m_Material.Friction - beforeF) > 1e-4f)
		{
			m_Dirty = true;
		}
		rowY += 32.0f;

		// Row 3: Restitution
		std::ostringstream ssR;
		ssR << std::fixed << std::setprecision(2) << m_Material.Restitution;
		std::string rText = "弹性系数 (Restitution): " + ssR.str();
		Wui::Label(ctx, { labelX, rowY + 3.0f }, rText, theme.Text, 12.0f);
		const Wui::WuiRect rSliderRect { fieldX, rowY, fieldW, 22.0f };
		Wui::Tooltip(ctx, rSliderRect, "碰撞恢复弹性系数 (0.0 为非弹性碰撞完全不反弹, 1.0 为动能无损完全反弹)");
		float beforeR = m_Material.Restitution;
		Wui::SliderFloat(ctx, Wui::HashId("pmat.slider.r"), rSliderRect, m_Material.Restitution, 0.0f, 1.0f, theme);
		if (std::abs(m_Material.Restitution - beforeR) > 1e-4f)
		{
			m_Dirty = true;
		}
		rowY += 32.0f;

		// Row 4: Friction Combine Mode
		Wui::Label(ctx, { labelX, rowY + 3.0f }, "摩擦力合并规则:", theme.Text, 12.0f);
		const Wui::WuiRect fcComboRect { fieldX, rowY, fieldW, 22.0f };
		const std::vector<std::string> combineOptions = {
			"Average (算术平均值: (A + B) / 2)",
			"Minimum (最小值: 只要一方光滑就打滑)",
			"Multiply (乘积: A * B)",
			"Maximum (最大值: 只要一方粗糙就阻滞)"
		};
		int curFc = static_cast<int>(m_Material.FrictionCombine);
		int beforeFc = curFc;
		if (Wui::Combo(ctx, Wui::HashId("pmat.combo.fc"), fcComboRect, "FrictionCombine", combineOptions, curFc, theme) && curFc != beforeFc)
		{
			m_Material.FrictionCombine = static_cast<PhysMatCombineMode>(curFc);
			m_Dirty = true;
		}
		rowY += 32.0f;

		// Row 5: Restitution Combine Mode
		Wui::Label(ctx, { labelX, rowY + 3.0f }, "弹性恢复合并规则:", theme.Text, 12.0f);
		const Wui::WuiRect rcComboRect { fieldX, rowY, fieldW, 22.0f };
		int curRc = static_cast<int>(m_Material.RestitutionCombine);
		int beforeRc = curRc;
		if (Wui::Combo(ctx, Wui::HashId("pmat.combo.rc"), rcComboRect, "RestitutionCombine", combineOptions, curRc, theme) && curRc != beforeRc)
		{
			m_Material.RestitutionCombine = static_cast<PhysMatCombineMode>(curRc);
			m_Dirty = true;
		}

		curY += 215.0f;

		// 4. Live Collision Simulation & Audition Card
		const Wui::WuiRect simCard { rect.X + padX, curY, contentW, 115.0f };
		Wui::PanelBackground(ctx, simCard, theme.ContentBg, 6.0f);

		Wui::Label(ctx, { simCard.X + 16.0f, simCard.Y + 10.0f }, "碰撞仲裁与实时效果试算 (Collision Response Simulation):", theme.Accent, 12.5f);

		float simY = simCard.Y + 34.0f;
		Wui::Label(ctx, { simCard.X + 16.0f, simY + 3.0f }, "接触目标表面 B 预设:", theme.TextMuted, 11.5f);
		const Wui::WuiRect simComboRect { simCard.X + 180.0f, simY, 260.0f, 22.0f };
		const std::vector<std::string> otherOptions = {
			"标准地面 (Default: F=0.5, R=0.0)",
			"光滑冰面 (Ice: F=0.05, R=0.05)",
			"高弹蹦床 (Rubber: F=0.8, R=0.9)",
			"粗糙石地 (Rough: F=1.0, R=0.0)"
		};
		int beforeOther = m_OtherSurfacePreset;
		if (Wui::Combo(ctx, Wui::HashId("pmat.sim.other"), simComboRect, "SurfaceB", otherOptions, m_OtherSurfacePreset, theme) && m_OtherSurfacePreset != beforeOther)
		{
			if (m_OtherSurfacePreset == 0) { m_OtherFriction = 0.5f; m_OtherRestitution = 0.0f; }
			else if (m_OtherSurfacePreset == 1) { m_OtherFriction = 0.05f; m_OtherRestitution = 0.05f; }
			else if (m_OtherSurfacePreset == 2) { m_OtherFriction = 0.8f; m_OtherRestitution = 0.9f; }
			else if (m_OtherSurfacePreset == 3) { m_OtherFriction = 1.0f; m_OtherRestitution = 0.0f; }
		}

		simY += 32.0f;
		float effF = PhysicsMaterial::Combine(m_Material.FrictionCombine, m_Material.Friction, m_OtherFriction);
		float effR = PhysicsMaterial::Combine(m_Material.RestitutionCombine, m_Material.Restitution, m_OtherRestitution);

		std::ostringstream ssEff;
		ssEff << "▶ 合成有效摩擦力 = " << std::fixed << std::setprecision(3) << effF
		      << "    |    ▶ 合成有效反弹弹性 = " << std::fixed << std::setprecision(3) << effR;
		Wui::Label(ctx, { simCard.X + 16.0f, simY + 3.0f }, ssEff.str(), Wui::WuiColor { 0.3f, 0.85f, 0.45f, 1.0f }, 12.0f);

		curY += 125.0f;

		// 5. Project Materials Quick Switcher
		if (!m_AvailableAssets.empty())
		{
			const Wui::WuiRect switchBar { rect.X + padX, curY, contentW, 30.0f };
			Wui::Label(ctx, { switchBar.X + 4.0f, switchBar.Y + 7.0f }, "工程物理材质切换:", theme.TextMuted, 11.0f);
			float sX = switchBar.X + 120.0f;
			for (size_t i = 0; i < m_AvailableAssets.size() && i < 5; ++i)
			{
				std::filesystem::path p(m_AvailableAssets[i]);
				std::string stem = p.stem().string();
				bool isCurrent = (m_LoadedPath == m_AvailableAssets[i]);
				const Wui::WuiRect btnS { sX, switchBar.Y + 3.0f, 110.0f, 22.0f };
				std::string label = isCurrent ? ("• " + stem) : stem;
				if (Wui::Button(ctx, Wui::HashId(("pmat.sw." + std::to_string(i)).c_str()), btnS, label, theme))
				{
					LoadAsset(m_AvailableAssets[i]);
				}
				sX += 116.0f;
			}
			const Wui::WuiRect btnRescan { sX, switchBar.Y + 3.0f, 75.0f, 22.0f };
			if (Wui::Button(ctx, Wui::HashId("pmat.sw.rescan"), btnRescan, "↻ 扫描", theme))
			{
				RefreshAvailableAssets();
			}
			curY += 34.0f;
		}

		// 6. Bottom Status Feedback
		if (!m_StatusFeedback.empty())
		{
			const Wui::WuiRect statusRect { rect.X + padX, curY, contentW, 24.0f };
			Wui::Label(ctx, { statusRect.X + 4.0f, statusRect.Y + 4.0f }, m_StatusFeedback, theme.TextMuted, 11.0f);
		}
	}
}
