#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace World::Editor
{
// HOTR-P1-T3:引擎内建 shader(`Engine/assets/shaders/**/*.slang`)的开发态热重载监听。
// 生效矩阵与开关:`docs/dev/hot-reload.md`。
	//
	// 与 T1 的 `Editor::ShaderHotReload`(材质表面 `.slang`,走项目内容根 + 后台编译线程)分开:
	// 引擎 shader 不属于任何项目内容根,产物的编译/重建由渲染器负责,所以这里只做
	// "绝对路径 + 内容哈希"的轮询(150ms 消抖),稳定变化后在**主线程帧边界(渲染开始前)**
	// 调 `Renderer::ReloadShaders()` —— 与 AI 命令 `renderer.reload_shaders` 同一入口。
	//
	// 线程纪律:本类只被主线程使用(无工作线程、无 GPU 操作);编译发生在
	// Renderer::ReloadShaders() 内部(Renderer2D/3D/WUI 各自先建新、成功再替换)。
	class EngineShaderHotReload
	{
	public:
		static constexpr double kDebounceSeconds = 0.15;   // 与脚本/资产热重载同一节拍

		EngineShaderHotReload() = default;

		// 主线程帧边界(渲染开始前)调用:轮询落盘变化;true = 本帧触发了重建。
		// 首次调用只建立基线(不触发),避免"启动即重载"。
		bool Poll(double deltaSeconds);

		// 监听根目录(默认 = `<WLD_WORLD_DIR>/assets/shaders`;诊断/测试可覆盖)。
		void SetRoot(const std::filesystem::path& root) { m_Root = root; }
		const std::filesystem::path& Root() const { return m_Root; }

		// 是否可用(根目录存在)。根不存在时 Poll 是 no-op。
		bool Available() const;

		// 清空基线与未决状态(OnDetach 调用;下次 Poll 重新建立基线)。
		void Shutdown();

	private:
		void Scan(std::unordered_map<std::string, uint64_t>& out) const;
		static bool HashFile(const std::filesystem::path& path, uint64_t& out);
		void ReportChanges(const std::unordered_map<std::string, uint64_t>& before,
			const std::unordered_map<std::string, uint64_t>& after) const;

		std::filesystem::path m_Root;
		std::unordered_map<std::string, uint64_t> m_Stable;    // 已确认(已报告)的内容哈希
		std::unordered_map<std::string, uint64_t> m_Pending;   // 未决内容(等待消抖)
		double m_PendingElapsed = 0.0;
		bool m_HasBaseline = false;
		bool m_HasPending = false;
	};
}
