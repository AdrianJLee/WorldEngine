#include "App/EditorLayer.h"
#include "Core/EditorCooker.h"
#include "Core/EditorPreferences.h"
#include "Core/EditorResources.h"
#include "App/EditorStartup.h"
#include "Integrations/VisualStudioAutomation.h"
#include "Project/ProjectLauncher.h"
#include "World/Asset/BuiltinImporters.h"
#include "World/Asset/CookPipeline.h"
#include "World/Asset/GltfImporter.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Prefab/ModelInstance.h"
#include "World/Gameplay/Prefab/Prefab.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Modules/GameModuleReload.h"
#include "World/Plugins/PluginManager.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Runtime/HotReload.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/Renderer/Texture/TextureLibrary.h"   // M48:纹理解析钩子(逻辑路径 → 句柄)
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <shellapi.h>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include "World/Events/MouseEvent.h"
namespace World
{
namespace EditorLayerDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace EditorLayerDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace EditorLayerDetail
	{
		// WLD_FRAME_TIMING=1:把编辑器一帧拆成"场景更新 / AI+WUI 起帧 / 面板逻辑 / WUI 录制提交"
		// 四段(默认关;只做诊断,不改变行为)。配合 Renderer 的 [frame-timing] 使用:
		// Renderer 那条覆盖交换链/present,这条覆盖层栈内部。
		struct LayerTiming
		{
			bool Enabled = false;
			double Update = 0, UiFrame = 0, Begin = 0, Shell = 0, WuiRender = 0;
			uint32_t Frames = 0;
		};
LayerTiming& LayerTimingState();

double LayerTimingNowMs();


		struct LayerTimingScope
		{
			explicit LayerTimingScope(double& sink) : m_Sink(&sink), m_Start(0.0)
			{
				if (LayerTimingState().Enabled)
					m_Start = LayerTimingNowMs();
			}
			~LayerTimingScope()
			{
				if (m_Sink && LayerTimingState().Enabled)
					*m_Sink += LayerTimingNowMs() - m_Start;
			}
			double* m_Sink;
			double m_Start;
		};
void LayerTimingFlush();

bool IsModelPreviewPanelOpen(const EditorShell& shell, const std::string& assetLogical);

bool AssetHotReloadEnabled();

bool LoadGameModuleForEditor(WorldContext& context, std::string* error);


		// D7-1a/P4-U13h:3D 视口中键平移的灵敏度系数(单位:个"视口高度对应的世界距离")。
		//
		// 口径:鼠标拖过整个视口高度 ≈ 平移 kPanUnitsPerViewportHeight 个视口高度对应的世界
		// 距离(目标平面处的视锥高度 = 2 × 距离 × tan(FOV/2))。1.0 = 目标平面上的内容与
		// 光标 1:1 跟随(Blender/Unreal 一类编辑器的中键抓取手感)。
		//
		// 旧口径(2026-09-21 前)把像素位移**直接当世界单位**(1.0 世界单位/物理像素,与视口
		// 尺寸、相机距离、FOV 全部无关)。实测默认布局(1280×720 窗口、ui_scale 1.3、三栏
		// 停靠)视口高 281.25 设计单位 = 365.6 物理像素:旧口径拖过整个视口高度平移 366 个
		// 世界单位(场景里的立方体边长才 1~1.6),即用户反馈的"中键拖太灵敏";新口径同样
		// 的整屏拖拽只平移 13.86 个世界单位,每物理像素 0.0379(旧值的 3.79%,≈26× 更慢)。
		//
		// 只作用于平移;右键环绕(1 像素 = 1 度)与滚轮推拉(1 格 = 0.6 世界单位)保持原口径不变。
		constexpr float kPanUnitsPerViewportHeight = 1.0f;
bool PanTraceEnabled();

std::string JsonEscape(const std::string& text);

	}

	// ---- P4-U13b:prefab 实例(实例条 / 层级徽标共用同一条实现)----
	namespace EditorLayerDetail
	{
uint32_t CountPrefabSubtreeEntities(const Scene& scene, entt::entity root);

entt::entity OwningPrefabInstanceRoot(const Scene& scene, entt::entity entity);

	}

	namespace EditorLayerDetail
	{
const std::filesystem::path& VisualStudioDevenvPath();

	}



}
