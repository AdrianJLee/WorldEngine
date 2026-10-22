#include "wldpch.h"

#include "WUI/Common/AssetThumbnailRenderer.h"

#include "World/WUI/WuiWidgets.h"

#include "World/WUI/Widgets/WuiChrome.h"

#include "World/Utils/Paths.h"

#include "World/Math/Curve/CubicBezierCurve.h"



#include <cmath>

#include <fstream>

#include <filesystem>



namespace World::Editor

{

	namespace

	{

		uint32_t HashString(std::string_view str)

		{

			uint32_t hash = 2166136261u;

			for (char c : str)

			{

				hash ^= static_cast<uint8_t>(c);

				hash *= 16777619u;

			}

			return hash;

		}



		void DrawMaterialBall(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			// 背景底色

			Wui::PanelBackground(ctx, rect, { 0.08f, 0.10f, 0.13f, 1.0f }, 3.0f);



			// 尝试解析 .wmat 属性 (纯文本读取，零外部依赖)

			Wui::WuiColor baseColor { 0.85f, 0.35f, 0.35f, 1.0f };

			float roughness = 0.35f;

			std::string albedoTex;



			std::filesystem::path fullPath = Paths::AssetRoot() / logicalPath;

			std::ifstream file(fullPath);

			if (file.is_open())

			{

				std::string line;

				while (std::getline(file, line))

				{

					if (line.rfind("BaseColor:", 0) == 0)

					{

						size_t ob = line.find('[');

						size_t cb = line.find(']');

						if (ob != std::string::npos && cb != std::string::npos && cb > ob)

						{

							std::string nums = line.substr(ob + 1, cb - ob - 1);

							float r = 0, g = 0, b = 0, a = 1;

							if (std::sscanf(nums.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) >= 3 ||

							    std::sscanf(nums.c_str(), "%f %f %f %f", &r, &g, &b, &a) >= 3)

							{

								baseColor = { r, g, b, a };

							}

						}

					}

					else if (line.rfind("Roughness:", 0) == 0)

					{

						float rough = 0.5f;

						if (std::sscanf(line.c_str() + 10, "%f", &rough) == 1)

							roughness = rough;

					}

					else if (line.rfind("AlbedoTexture:", 0) == 0)

					{

						std::string val = line.substr(14);

						while (!val.empty() && (val.front() == ' ' || val.front() == '"')) val.erase(val.begin());

						while (!val.empty() && (val.back() == ' ' || val.back() == '"' || val.back() == '\r')) val.pop_back();

						albedoTex = val;

					}

				}

			}



			const float cx = rect.X + rect.W * 0.5f;

			const float cy = rect.Y + rect.H * 0.5f;

			const float r = std::min(rect.W, rect.H) * 0.36f;



			// 地面阴影 (柔和椭圆)

			Wui::PanelBackground(ctx, { cx - r * 0.75f, cy + r * 0.55f, r * 1.5f, r * 0.35f },

				{ 0.03f, 0.04f, 0.06f, 0.85f }, r * 0.35f);



			// 材质球基底圆

			const Wui::WuiRect sphereRect { cx - r, cy - r, r * 2.0f, r * 2.0f };

			Wui::PanelBackground(ctx, sphereRect, baseColor, r);



			// 3D 漫反射球面高光层 (向左上方偏移)

			const Wui::WuiColor highlightCol {

				std::min(1.0f, baseColor.R * 1.35f + 0.12f),

				std::min(1.0f, baseColor.G * 1.35f + 0.12f),

				std::min(1.0f, baseColor.B * 1.35f + 0.12f),

				0.75f

			};

			Wui::PanelBackground(ctx, { cx - r * 0.75f, cy - r * 0.75f, r * 1.30f, r * 1.30f },

				highlightCol, r * 0.65f);



			// 镜面高光亮点 (粗糙度越小越锐利)

			const float specR = r * 0.32f * (1.0f - roughness * 0.6f);

			Wui::PanelBackground(ctx, { cx - r * 0.42f, cy - r * 0.42f, specR, specR },

				{ 1.0f, 1.0f, 1.0f, 0.90f }, specR * 0.5f);



			// 边缘光轮廓

			Wui::HighlightOutline(ctx, sphereRect, { 1.0f, 1.0f, 1.0f, 0.30f }, r, 1.0f);



			// 右上角微型角标

			Wui::Badge(ctx, { rect.X + rect.W - 30.0f, rect.Y + 2.0f, 28.0f, 13.0f },

				"MAT", { 0.15f, 0.75f, 0.85f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawAudioWaveform(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			// 背景底色 (深冷色)

			Wui::PanelBackground(ctx, rect, { 0.08f, 0.11f, 0.16f, 1.0f }, 3.0f);



			const float cy = rect.Y + rect.H * 0.5f;



			// 中心基准线

			Wui::LineSegment(ctx, { rect.X + 6.0f, cy }, { rect.X + rect.W - 6.0f, cy },

				{ 0.25f, 0.35f, 0.45f, 0.35f }, 1.0f);



			// 16 根波形柱 (基于路径哈希确定性生成)

			const uint32_t hash = HashString(logicalPath);

			constexpr int kBarCount = 16;

			const float availW = rect.W - 16.0f;

			const float stepX = availW / static_cast<float>(kBarCount);



			for (int i = 0; i < kBarCount; ++i)

			{

				float normH = (std::sin(static_cast<float>(i) * 1.35f + static_cast<float>(hash % 29)) * 0.5f + 0.5f);

				normH = normH * normH * 0.8f + 0.15f; // 强调动态范围

				const float barH = std::max(3.0f, normH * (rect.H * 0.65f));

				const float barX = rect.X + 8.0f + static_cast<float>(i) * stepX + stepX * 0.5f;



				Wui::LineSegment(ctx, { barX, cy - barH * 0.5f }, { barX, cy + barH * 0.5f },

					{ 1.0f, 0.60f, 0.18f, 0.85f }, 2.0f);

			}



			Wui::Badge(ctx, { rect.X + rect.W - 34.0f, rect.Y + 2.0f, 32.0f, 13.0f },

				"WAV", { 0.95f, 0.55f, 0.20f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawCurveGraph(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.08f, 0.12f, 0.15f, 1.0f }, 3.0f);



			// 内部网格线

			const float midX = rect.X + rect.W * 0.5f;

			const float midY = rect.Y + rect.H * 0.5f;

			Wui::LineSegment(ctx, { rect.X + 4.0f, midY }, { rect.X + rect.W - 4.0f, midY },

				{ 0.20f, 0.28f, 0.35f, 0.30f }, 1.0f);

			Wui::LineSegment(ctx, { midX, rect.Y + 4.0f }, { midX, rect.Y + rect.H - 4.0f },

				{ 0.20f, 0.28f, 0.35f, 0.30f }, 1.0f);



			// 加载或采样曲线

			CubicBezierCurve curve;

			std::filesystem::path fullPath = Paths::AssetRoot() / logicalPath;

			if (!curve.LoadFromFile(fullPath.string()))

			{

				curve = CubicBezierCurve::EaseInOut();

			}



			constexpr int kSteps = 16;

			const float padX = 8.0f;

			const float padY = 8.0f;

			const float plotW = rect.W - padX * 2.0f;

			const float plotH = rect.H - padY * 2.0f;



			auto mapPoint = [&](float t, float val) -> glm::vec2

			{

				float px = rect.X + padX + t * plotW;

				float py = rect.Y + rect.H - padY - val * plotH;

				return { px, py };

			};



			glm::vec2 prev = mapPoint(0.0f, curve.Evaluate(0.0f));

			for (int i = 1; i <= kSteps; ++i)

			{

				float t = static_cast<float>(i) / static_cast<float>(kSteps);

				glm::vec2 curr = mapPoint(t, curve.Evaluate(t));

				Wui::LineSegment(ctx, prev, curr, { 0.0f, 0.88f, 0.82f, 1.0f }, 2.0f);

				prev = curr;

			}



			// 起止控制点

			glm::vec2 p0 = mapPoint(0.0f, curve.Evaluate(0.0f));

			glm::vec2 p1 = mapPoint(1.0f, curve.Evaluate(1.0f));

			Wui::PanelBackground(ctx, { p0.x - 2.5f, p0.y - 2.5f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, 2.5f);

			Wui::PanelBackground(ctx, { p1.x - 2.5f, p1.y - 2.5f, 5.0f, 5.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, 2.5f);



			Wui::Badge(ctx, { rect.X + rect.W - 36.0f, rect.Y + 2.0f, 34.0f, 13.0f },

				"CURV", { 0.20f, 0.80f, 0.75f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawPrefabBlueprint(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.12f, 0.09f, 0.18f, 1.0f }, 3.0f);



			const float cx = rect.X + rect.W * 0.5f;

			const float cy = rect.Y + rect.H * 0.5f + 2.0f;

			const float s = std::min(rect.W, rect.H) * 0.28f;

			const Wui::WuiColor wireCol { 0.80f, 0.45f, 0.90f, 0.85f };



			// 等轴测 3D 棱晶盒

			glm::vec2 top { cx, cy - s };

			glm::vec2 right { cx + s * 1.1f, cy - s * 0.4f };

			glm::vec2 left { cx - s * 1.1f, cy - s * 0.4f };

			glm::vec2 center { cx, cy + s * 0.2f };

			glm::vec2 bRight { cx + s * 1.1f, cy + s * 0.8f };

			glm::vec2 bLeft { cx - s * 1.1f, cy + s * 0.8f };

			glm::vec2 bottom { cx, cy + s * 1.4f };



			Wui::LineSegment(ctx, top, right, wireCol, 1.5f);

			Wui::LineSegment(ctx, right, center, wireCol, 1.5f);

			Wui::LineSegment(ctx, center, left, wireCol, 1.5f);

			Wui::LineSegment(ctx, left, top, wireCol, 1.5f);



			Wui::LineSegment(ctx, left, bLeft, wireCol, 1.5f);

			Wui::LineSegment(ctx, center, bottom, wireCol, 1.5f);

			Wui::LineSegment(ctx, right, bRight, wireCol, 1.5f);



			Wui::LineSegment(ctx, bLeft, bottom, wireCol, 1.5f);

			Wui::LineSegment(ctx, bottom, bRight, wireCol, 1.5f);



			// 中心菱形标记

			Wui::PanelBackground(ctx, { center.x - 2.5f, center.y - 2.5f, 5.0f, 5.0f },

				{ 1.0f, 1.0f, 1.0f, 0.95f }, 1.5f);



			Wui::Badge(ctx, { rect.X + rect.W - 36.0f, rect.Y + 2.0f, 34.0f, 13.0f },

				"PREFAB", { 0.75f, 0.40f, 0.85f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 9.0f);

		}



		void DrawMeshWireframe(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const std::string& logicalPath, const Wui::WuiTheme& theme)
		{
			// 背景深冷底色
			Wui::PanelBackground(ctx, rect, theme.ContentBg, 3.0f);

			const float cx = rect.X + rect.W * 0.5f;
			const float cy = rect.Y + rect.H * 0.5f + 1.0f;
			const float r = std::min(rect.W, rect.H) * 0.35f;

			// 底部接触投影 (柔和深黑椭圆)
			Wui::PanelBackground(ctx, { cx - r * 0.85f, cy + r * 0.55f, r * 1.70f, r * 0.35f },
				{ 0.03f, 0.04f, 0.06f, 0.85f }, r * 0.35f);

			// 等轴测 3D 棱台/立方体网格顶点定义
			const float dx = r * 0.82f;
			const float h = r * 0.72f;

			const glm::vec2 vCenter { cx, cy };
			const glm::vec2 vTop { cx, cy - h };
			const glm::vec2 vTopLeft { cx - dx, cy - h * 0.35f };
			const glm::vec2 vTopRight { cx + dx, cy - h * 0.35f };
			const glm::vec2 vBotLeft { cx - dx, cy + h * 0.45f };
			const glm::vec2 vBotRight { cx + dx, cy + h * 0.45f };
			const glm::vec2 vBottom { cx, cy + h * 0.80f };

			// 3D 几何线框主要轮廓 (鲜亮橙黄/琥珀色)
			Wui::LineSegment(ctx, vTop, vTopRight, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vTopRight, vCenter, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vCenter, vTopLeft, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vTopLeft, vTop, theme.Warning, 1.5f);

			Wui::LineSegment(ctx, vTopLeft, vBotLeft, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vBotLeft, vBottom, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vBottom, vCenter, theme.Warning, 1.5f);

			Wui::LineSegment(ctx, vTopRight, vBotRight, theme.Warning, 1.5f);
			Wui::LineSegment(ctx, vBotRight, vBottom, theme.Warning, 1.5f);

			// 内部拓扑细分骨架线 (传达 3D 多边形网格布线质感)
			const glm::vec2 vMidTopLeft = (vTop + vTopLeft) * 0.5f;
			const glm::vec2 vMidTopRight = (vTop + vTopRight) * 0.5f;
			const glm::vec2 vMidCenterLeft = (vTopLeft + vCenter) * 0.5f;
			const glm::vec2 vMidCenterRight = (vTopRight + vCenter) * 0.5f;
			const Wui::WuiColor wireGridCol = { theme.Warning.R, theme.Warning.G, theme.Warning.B, 0.40f };
			Wui::LineSegment(ctx, vMidTopLeft, vMidCenterRight, wireGridCol, 1.0f);
			Wui::LineSegment(ctx, vMidTopRight, vMidCenterLeft, wireGridCol, 1.0f);
			Wui::LineSegment(ctx, vTopLeft, vBottom, wireGridCol, 1.0f);
			Wui::LineSegment(ctx, vTopRight, vBottom, wireGridCol, 1.0f);

			// 关键几何顶点结点 (小圆点提示 3D 几何实体)
			const glm::vec2 vertices[] = { vTop, vTopLeft, vTopRight, vCenter, vBotLeft, vBotRight, vBottom };
			for (const auto& pt : vertices)
			{
				Wui::PanelBackground(ctx, { pt.x - 2.0f, pt.y - 2.0f, 4.0f, 4.0f }, theme.TextBright, 2.0f);
			}

			Wui::Badge(ctx, { rect.X + rect.W - 28.0f, rect.Y + 2.0f, 26.0f, 13.0f },
				"3D", theme.Warning, theme.TextBright, theme, 9.0f);
		}



		void DrawGenericThumbnail(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

			const std::string& logicalPath, AssetCategory category, const Wui::WuiTheme& theme)

		{

			Wui::PanelBackground(ctx, rect, { 0.12f, 0.13f, 0.16f, 1.0f }, 3.0f);



			const char* label = "FILE";

			Wui::WuiColor col { 0.6f, 0.6f, 0.6f, 1.0f };



			switch (category)

			{

			case AssetCategory::Script:

				label = "LUA";

				col = { 0.95f, 0.80f, 0.20f, 1.0f };

				break;

			case AssetCategory::Level:

				label = "LEVEL";

				col = { 0.90f, 0.30f, 0.30f, 1.0f };

				break;

			case AssetCategory::Shader:

				label = "SHADER";

				col = { 0.50f, 0.50f, 0.95f, 1.0f };

				break;

			case AssetCategory::UI:

				label = "UI";

				col = { 0.85f, 0.35f, 0.70f, 1.0f };

				break;

			default:

				break;

			}



			// 中心色块卡片

			const float bw = 50.0f;

			const float bh = 22.0f;

			Wui::Badge(ctx, { rect.X + (rect.W - bw) * 0.5f, rect.Y + (rect.H - bh) * 0.5f, bw, bh },

				label, col, { 1.0f, 1.0f, 1.0f, 1.0f }, theme, 11.0f);

		}

	}



	void AssetThumbnailRenderer::DrawThumbnail(Wui::WuiContext& ctx, const Wui::WuiRect& rect,

		const std::string& logicalPath, AssetCategory category, const Wui::WuiTheme& theme)

	{

		// 1. 纹理类资产 (Texture / Atlas) -> 优先使用 GPU 纹理直接呈现真实画面

		if (category == AssetCategory::Texture)

		{

			std::string targetTexture = logicalPath;



			// 如果是图集 .watlas，尝试解析其底层的贴图路径

			if (logicalPath.size() > 7 && logicalPath.rfind(".watlas") == logicalPath.size() - 7)

			{

				std::filesystem::path atlasFull = Paths::AssetRoot() / logicalPath;

				std::ifstream f(atlasFull);

				if (f.is_open())

				{

					std::string line;

					while (std::getline(f, line))

					{

						if (line.rfind("TexturePath:", 0) == 0)

						{

							std::string val = line.substr(12);

							while (!val.empty() && (val.front() == ' ' || val.front() == '"')) val.erase(val.begin());

							while (!val.empty() && (val.back() == ' ' || val.back() == '"' || val.back() == '\r')) val.pop_back();

							if (val.rfind("assets/", 0) == 0 || val.rfind("assets\\", 0) == 0)

								val = val.substr(7);

							targetTexture = val;

							break;

						}

					}

				}

			}



			uint64_t texId = AssetThumbnailCache::Get().GetOrCreateTextureId(targetTexture);

			if (texId != 0)

			{

				// 背景棋盘保护底

				Wui::PanelBackground(ctx, rect, { 0.12f, 0.14f, 0.18f, 1.0f }, 3.0f);



				// 绘制真实图像

				Wui::Image(ctx, rect, texId, { 0.0f, 0.0f, 1.0f, 1.0f }, theme);



				// 1px 细微边界保护

				Wui::HighlightOutline(ctx, rect, { 1.0f, 1.0f, 1.0f, 0.15f }, 3.0f, 1.0f);

				return;

			}

		}



		// 2. 材质类资产 -> 3D 材质球预览

		if (category == AssetCategory::Material)

		{

			DrawMaterialBall(ctx, rect, logicalPath, theme);

			return;

		}



		// 3. 音频类资产 -> 动态波形图预览

		if (category == AssetCategory::Audio)

		{

			DrawAudioWaveform(ctx, rect, logicalPath, theme);

			return;

		}



		// 4. 曲线类资产 -> 动态贝塞尔曲线图预览

		if (category == AssetCategory::Curve)

		{

			DrawCurveGraph(ctx, rect, logicalPath, theme);

			return;

		}



		// 5. 预制体类资产 -> 蓝图实体线框预览

		if (category == AssetCategory::Prefab)

		{

			DrawPrefabBlueprint(ctx, rect, logicalPath, theme);

			return;

		}



		// 6. 3D 模型类资产 -> 3D 网格几何线框预览

		if (category == AssetCategory::Mesh)

		{

			DrawMeshWireframe(ctx, rect, logicalPath, theme);

			return;

		}



		// 7. 其余资产 -> 结构化卡片与徽标

		DrawGenericThumbnail(ctx, rect, logicalPath, category, theme);

	}

}

