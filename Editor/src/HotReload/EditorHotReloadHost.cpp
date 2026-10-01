#include "EditorHotReloadHost.h"

#include <cstdlib>
#include <string>
#include <utility>

namespace World::Editor
{
	namespace
	{
		// 子开关与收敛前同口径(PollAssetHotReload 里的 `static const bool`):进程内首次
		// 判定后固定 —— 环境变量在启动前设置,运行期不变化。
		bool TextureHotReloadEnabled()
		{
			static const bool enabled = []
			{
				const char* value = std::getenv("WLD_TEXTURE_HOTRELOAD");
				return !(value != nullptr && *value != '\0' && std::string(value) == "0");
			}();
			return enabled;
		}

		bool ModelHotReloadEnabled()
		{
			static const bool enabled = []
			{
				const char* value = std::getenv("WLD_MODEL_HOTRELOAD");
				return !(value != nullptr && *value != '\0' && std::string(value) == "0");
			}();
			return enabled;
		}
	}

	EditorHotReloadHost::~EditorHotReloadHost()
	{
		Shutdown();
	}

	void EditorHotReloadHost::Poll(bool assetHotReloadEnabled, double deltaSeconds)
	{
		// 引擎内建 shader:自己的 WLD_SHADER_HOTRELOAD 在 EngineShaderHotReload::Poll 内部
		// 把关;与资产热重载总开关/活动场景无关(启动器/无项目形态同样生效)。
		m_EngineShaderHotReload.Poll(deltaSeconds);
		if (!assetHotReloadEnabled)
			return;
		if (TextureHotReloadEnabled())
			m_TextureImportWatch.Poll(deltaSeconds);
		if (ModelHotReloadEnabled())
			m_ModelImportWatch.Poll(deltaSeconds);
	}

	void EditorHotReloadHost::Pump(bool assetHotReloadEnabled)
	{
		if (!assetHotReloadEnabled)
			return;
		// 顺序与收敛前一致:材质 `.slang` 的 Install 在材质库轮询之前;纹理重烘落盘与
		// 模型重导入提交各自保持"主线程帧边界 + 子开关"口径。
		m_ShaderHotReload.Pump();
		if (TextureHotReloadEnabled())
			m_TextureImportWatch.Pump();
		if (ModelHotReloadEnabled())
			m_ModelImportWatch.Pump();
	}

	void EditorHotReloadHost::Shutdown()
	{
		// 与收敛前 OnDetach 的调用顺序一致。
		m_ShaderHotReload.Shutdown();
		m_EngineShaderHotReload.Shutdown();
		m_TextureImportWatch.Shutdown();
		m_ModelImportWatch.Shutdown();
	}

	void EditorHotReloadHost::EnqueueShader(const std::string& logicalPath)
	{
		m_ShaderHotReload.Enqueue(logicalPath);
	}

	void EditorHotReloadHost::SetModelSkipProbe(ModelImportWatch::SkipProbe probe)
	{
		m_ModelImportWatch.SetSkipProbe(std::move(probe));
	}

	std::size_t EditorHotReloadHost::WatchedTextureAssets() const
	{
		return m_TextureImportWatch.WatchedAssetCount();
	}

	std::size_t EditorHotReloadHost::WatchedModelAssets() const
	{
		return m_ModelImportWatch.WatchedModelCount();
	}
}
