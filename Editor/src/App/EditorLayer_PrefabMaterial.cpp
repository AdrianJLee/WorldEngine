#include "EditorLayer_Internal.h"

namespace World
{

using namespace EditorLayerDetail;


	// ---- P4-U13:prefab 文档编辑会话 ----
	//
	// 交互设计(工业引擎同款):双击 .wprefab = 打开编辑(把它当文档),编辑期间顶部有一条
	// 常驻横幅说明"你在改的是资产、不是场景",保存写回资产、返回恢复原来的场景。
	// 文件格式与场景同构(同一个序列化器),因此这里直接复用文档的加载/保存通道。
void EditorLayer::OpenPrefab(const std::string& logicalPath){
		if (logicalPath.empty())
			return;
		RequestAction([this, logicalPath]() { DoOpenPrefab(logicalPath); });
	}


	// P4-U13c:prefab 资产窗口(看/管理)与编辑会话(OpenPrefab)分开:
	// 窗口只读展示资产;真正的编辑仍然进文档会话(窗口里的 Edit Prefab 调 OpenPrefab)。
void EditorLayer::OpenPrefabWindow(const std::string& logicalPath){
		if (logicalPath.empty())
			return;
		std::string message;
		// 读不了的资产也要开窗口(状态行写原因),但失败必须留一条可读日志,不能静默。
		if (!m_Shell.OpenPrefabWindowChecked(logicalPath, &message))
			WLD_CORE_WARN("[prefab] window opened for unreadable asset '{0}': {1}", logicalPath, message);
	}


void EditorLayer::DoOpenPrefab(const std::string& logicalPath){
		const std::filesystem::path absolute = World::Paths::AssetRoot() / logicalPath;
		// 记住"进来之前的场景":返回时原样重开(没有就在返回时给一个空场景)。
		if (!IsEditingPrefab())
		{
			m_SceneBeforePrefab = m_Document.GetPath();
			m_HadSceneBeforePrefab = m_Document.HasPath();
		}
		if (!m_Document.LoadFromFile(absolute))
		{
			ShowError(m_Document.GetLastError());
			return;
		}
		m_PrefabEditLogical = logicalPath;
		SetSceneState(SceneState::Edit);
		UpdateSceneContext(m_Document.GetScene());
		RebaselineExternalSceneWatch();
		WLD_CORE_INFO("[prefab] editing '{0}' (save writes back to the asset)", logicalPath);
	}


bool EditorLayer::SavePrefab(){
		if (!IsEditingPrefab())
			return false;
		const std::filesystem::path absolute = World::Paths::AssetRoot() / m_PrefabEditLogical;
		if (!m_Document.SaveTo(absolute))
		{
			ShowError(m_Document.GetLastError());
			WLD_CORE_WARN("[prefab] save failed: {0}", m_Document.GetLastError());
			return false;
		}
		WLD_CORE_INFO("[prefab] saved '{0}'", m_PrefabEditLogical);
		return true;
	}


void EditorLayer::ClosePrefab(){
		if (!IsEditingPrefab())
			return;
		const std::string logical = m_PrefabEditLogical;
		const std::filesystem::path previous = m_SceneBeforePrefab;
		const bool hadPrevious = m_HadSceneBeforePrefab;
		m_PrefabEditLogical.clear();
		m_SceneBeforePrefab.clear();
		m_HadSceneBeforePrefab = false;
		WLD_CORE_INFO("[prefab] leaving prefab edit session '{0}'", logical);
		if (hadPrevious && !previous.empty())
			DoOpenScene(previous);
		else
			DoNewScene();
	}


bool EditorLayer::InstantiatePrefabAsset(const std::string& logicalPath, std::string* message){
		if (!m_ActiveScene || logicalPath.empty())
		{
			if (message) *message = "no active scene";
			return false;
		}
		// 与 InstantiateModelFile 同一条只读口径:实例化会写活动场景结构,
		// Play/Simulate 下拒绝(否则 GetRegistry() 的"活动场景禁止结构写"断言会抛出来)。
		if (m_SceneState != SceneState::Edit)
		{
			if (message)
				*message = "预制体只能在编辑态实例化(Play/Simulate 下请先退出)";
			return false;
		}
		const std::filesystem::path absolute = World::Paths::AssetRoot() / logicalPath;
		const Gameplay::PrefabInstanceResult result =
			Gameplay::InstantiateFromFile(absolute, *m_ActiveScene, entt::null);
		if (!result.IsValid())
		{
			if (message) *message = "prefab instantiate failed: " + logicalPath;
			return false;
		}
		// P4-U13b:新入口产生的实例同样登记进场景注册表(层级徽标 / 属性面板实例条 /
		// 右键菜单 / 场景存档都读这一份记录)。路径存**逻辑路径**(与内容浏览器同一约定)。
		m_ActiveScene->AddPrefabInstance(logicalPath, static_cast<entt::entity>(result.Root));
		m_SelectedEntity = result.Root;
		MarkDocumentDirty();
		if (message)
			*message = "已实例化 " + std::to_string(result.EntityCount) + " 个实体: " + logicalPath;
		return true;
	}

namespace EditorLayerDetail
{
		// P4-U13d:子树实体数(写出的 prefab 里会有多少实体)。只走 const 注册表 —— Play/Simulate
		// 下活动场景的非 const GetRegistry() 会触发"结构写"断言;访问集防非法层级死循环。
uint32_t CountPrefabSubtreeEntities(const Scene& scene, entt::entity root){
			const entt::registry& registry = scene.GetRegistry();
			if (!registry.valid(root))
				return 0;
			std::vector<entt::entity> pending { root };
			std::unordered_set<uint32_t> seen;
			uint32_t count = 0;
			constexpr uint32_t kMaxEntities = 200000;
			while (!pending.empty() && count < kMaxEntities)
			{
				const entt::entity current = pending.back();
				pending.pop_back();
				if (!registry.valid(current) || !seen.insert(static_cast<uint32_t>(current)).second)
					continue;
				++count;
				for (const entt::entity child : Hierarchy::ChildrenOf(registry, current))
					pending.push_back(child);
			}
			return count;
		}


		// 实体属于哪个实例:自身是实例根,或沿父链找到实例根(深度上限防非法层级死循环)。
		// 只走 const 注册表:Play/Simulate 下活动场景的非 const GetRegistry() 会触发结构写断言,
		// 而实例条在 Play 期间仍然要显示(那时三个动作是禁用的)。
entt::entity OwningPrefabInstanceRoot(const Scene& scene, entt::entity entity){
			constexpr int kMaxAncestorDepth = 64;
			entt::entity current = entity;
			for (int depth = 0; depth < kMaxAncestorDepth && current != entt::null; ++depth)
			{
				if (scene.FindPrefabInstance(current))
					return current;
				const entt::registry& registry = scene.GetRegistry();
				if (!registry.valid(current))
					break;
				const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
				if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
					break;
				current = hierarchy->Parent;
			}
			return entt::null;
		}

}

	// ---- P4-U13d:创建预制体(实体子树 → .wprefab 资产)----
	//
	// 一条内核,两个入口:层级面板的"Create Prefab from Selection…"模态与 AI 通道
	// `asset.create_prefab`。口径:
	//  - 只写当前内容根(World::Paths::AssetRoot())内的 .wprefab;缺后缀自动补,
	//    越界/非法字符直接拒绝;
	//  - overwrite=false 且目标已存在 = 失败(绝不静默覆盖,把"覆盖"变成显式决定);
	//  - 成功 = 写盘 + `[prefab] created <逻辑路径> (N entities)` + 内容浏览器选中该资产
	//    + 打开它的 prefab 资产窗口(不进编辑会话,编辑仍要显式点 Edit Prefab)。
bool EditorLayer::CreatePrefabFromSelection(Entity root, const std::string& logicalPath, bool overwrite, std::string* message, PrefabCreateResult* result){
		if (message) message->clear();
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "预制体只能在编辑态创建(Play/Simulate 下请先退出)";
			return false;
		}
		if (!m_ActiveScene || !root.IsValid() || root.GetScene() != m_ActiveScene.get())
		{
			if (message) *message = "没有可导出的实体(先在层级面板里选中一个实体)";
			return false;
		}

		// 逻辑路径:统一分隔符、剥前导斜杠、补 .wprefab(大小写不敏感)。
		std::string logical = logicalPath;
		std::replace(logical.begin(), logical.end(), '\\', '/');
		while (!logical.empty() && logical.front() == '/')
			logical.erase(logical.begin());
		if (logical.empty())
		{
			if (message) *message = "缺少目标路径(如 prefabs/MyCube.wprefab)";
			return false;
		}
		auto lowered = [](std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		};
		constexpr size_t kPrefabSuffixLength = 8;   // ".wprefab"
		const std::string loweredLogical = lowered(logical);
		if (loweredLogical.size() < kPrefabSuffixLength
			|| loweredLogical.compare(loweredLogical.size() - kPrefabSuffixLength, kPrefabSuffixLength, ".wprefab") != 0)
			logical += ".wprefab";

		// 逐段校验:不许空段 / "." / "..",文件名不许含 Windows 非法字符 —— 落点必须留在内容根内。
		for (size_t start = 0; start <= logical.size();)
		{
			const size_t slash = logical.find('/', start);
			const std::string part = logical.substr(start,
				slash == std::string::npos ? std::string::npos : slash - start);
			if (part.empty() || part == "." || part == "..")
			{
				if (message) *message = "非法路径: " + logicalPath + "(不许空目录段 / \"..\")";
				return false;
			}
			if (slash == std::string::npos)
				break;
			start = slash + 1;
		}
		for (const char ch : logical)
		{
			if (ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
			{
				if (message) *message = "非法路径: 不能包含 : * ? \" < > | — " + logicalPath;
				return false;
			}
		}

		const std::filesystem::path absolute = World::Paths::AssetRoot() / std::filesystem::path(logical);
		std::error_code existsError;
		const bool exists = std::filesystem::exists(absolute, existsError);
		if (exists && !overwrite)
		{
			if (message) *message = "目标已存在: " + logical + "(未覆盖;需要覆盖请显式确认)";
			return false;
		}
		if (!absolute.parent_path().empty())
		{
			std::error_code dirError;
			std::filesystem::create_directories(absolute.parent_path(), dirError);
			if (!std::filesystem::is_directory(absolute.parent_path()))
			{
				if (message) *message = "目录创建失败: " + absolute.parent_path().generic_string()
					+ (dirError ? (" (" + dirError.message() + ")") : std::string());
				return false;
			}
		}

		std::string saveError;
		if (!Gameplay::SaveFromScene(*m_ActiveScene, root, absolute, &saveError))
		{
			if (message) *message = saveError.empty() ? ("写盘失败: " + logical) : saveError;
			WLD_CORE_WARN("[prefab] create failed: {0} ({1})", logical, saveError);
			return false;
		}
		const uint32_t entityCount = CountPrefabSubtreeEntities(*m_ActiveScene, static_cast<entt::entity>(root));
		WLD_CORE_INFO("[prefab] created {0} ({1} entities)", logical, entityCount);
		m_WuiContext.RecordOp("prefab", exists ? "overwrite" : "create", logical,
			std::to_string(entityCount));

		// 成功口径:内容浏览器选中该资产 + 打开它的 prefab 资产窗口(编辑仍要显式进会话)。
		m_Shell.SelectContentAsset(logical, "create-prefab");
		std::string openMessage;
		if (!m_Shell.OpenPrefabWindowChecked(logical, &openMessage))
			WLD_CORE_WARN("[prefab] created '{0}' but its asset window could not read it back: {1}",
				logical, openMessage);
		if (result)
		{
			result->LogicalPath = logical;
			result->EntityCount = entityCount;
			result->Overwrote = exists;
		}
		if (message)
			*message = "已创建 " + logical + " (" + std::to_string(entityCount) + " entities)";
		return true;
	}


	// ---- U25-M2:材质工作流:把材质接回场景(编辑器侧唯一写入口)----
	//
	// 与 AI 通道 `scene.set ... Material` / 属性面板用的是**同一个字段**
	// (MeshRendererComponent.MaterialPath,相对内容根):场景存档、渲染器与"撤销本次赋值"
	// 因此天然一致。面板不自己改组件,只通过 PanelHost 调这一条。
bool EditorLayer::AssignMaterialToEntity(Entity entity, const std::string& logicalPath, std::string* message, std::string* outPreviousPath){
		if (message)
			message->clear();
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "材质只能在编辑态赋值(Play/Simulate 下场景只读)";
			return false;
		}
		if (!m_ActiveScene || !entity.IsValid() || entity.GetScene() != m_ActiveScene.get())
		{
			if (message) *message = "没有选中实体(先在层级面板里选中一个实体)";
			return false;
		}
		const entt::entity handle = static_cast<entt::entity>(entity);
		auto& registry = m_ActiveScene->GetRegistry();
		auto* mesh = registry.try_get<MeshRendererComponent>(handle);
		if (!mesh)
		{
			if (message) *message = "选中实体没有 MeshRenderer(材质只能赋给会渲染的实体)";
			return false;
		}
		const std::string normalized = MaterialLibrary::NormalizePath(logicalPath);
		const std::string previous = mesh->MaterialPath;
		const bool changed = previous != normalized;
		mesh->MaterialPath = normalized;
		if (changed)
			MarkDocumentDirty();
		const auto* tag = registry.try_get<TagComponent>(handle);
		const std::string name = tag ? tag->Tag : std::string("(unnamed)");
		WLD_CORE_INFO("[material-ui] assign '{0}' -> entity {1} ('{2}'){3}", normalized,
			static_cast<uint32_t>(handle), name, changed ? "" : " (unchanged)");
		m_WuiContext.RecordOp("material", "assign", normalized, "entity=" + std::to_string(static_cast<uint32_t>(handle)));
		if (outPreviousPath)
			*outPreviousPath = previous;
		if (message)
			*message = (changed ? "已把 " : "已是 ")
				+ (normalized.empty() ? std::string("(none)") : normalized) + " → " + name;
		return true;
	}


bool EditorLayer::AssignMaterialToSelection(const std::string& logicalPath, Entity* outEntity, std::string* outPreviousPath, std::string* message){
		if (outEntity)
			*outEntity = Entity {};
		// 选择模型是**单选**(EditorLayer::m_SelectedEntity):面板的 "Assign to Selection"
		// 交给这里的永远是"第一个(也是唯一一个)选中实体";多选落地后按同一入口逐个调用即可。
		const Entity target = m_SelectedEntity;
		std::string previous;
		if (!AssignMaterialToEntity(target, logicalPath, message, &previous))
			return false;
		if (outEntity)
			*outEntity = target;
		if (outPreviousPath)
			*outPreviousPath = previous;
		return true;
	}


bool EditorLayer::PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount, Entity* root){
		if (!m_ActiveScene || !entity.IsValid() || entity.GetScene() != m_ActiveScene.get())
			return false;
		const entt::entity rootHandle = OwningPrefabInstanceRoot(
			*m_ActiveScene, static_cast<entt::entity>(entity));
		if (rootHandle == entt::null)
			return false;
		const Gameplay::PrefabInstanceRecord* record = m_ActiveScene->FindPrefabInstance(rootHandle);
		if (!record)
			return false;
		if (sourcePath)
			*sourcePath = record->PrefabPath;
		if (overrideCount)
			*overrideCount = Gameplay::GetOverrideCount(*record);
		if (root)
			*root = Entity(m_ActiveScene.get(), rootHandle);
		return true;
	}


bool EditorLayer::PrefabInstanceRevert(Entity root, std::string* message){
		// Play/Simulate 下活动场景是播放副本,改它没有意义(与属性面板的只读规则一致)。
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && root.IsValid()
			? m_ActiveScene->FindPrefabInstance(static_cast<entt::entity>(root)) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		if (!Gameplay::RevertInstance(*record, *m_ActiveScene))
		{
			if (message) *message = "回滚失败:来源资产读不到或结构已不匹配";
			return false;
		}
		MarkDocumentDirty();
		if (message) *message = "已回滚到资产";
		return true;
	}


bool EditorLayer::PrefabInstanceApply(Entity root, std::string* message){
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && root.IsValid()
			? m_ActiveScene->FindPrefabInstance(static_cast<entt::entity>(root)) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		if (record->PrefabPath.empty())
		{
			if (message) *message = "来源资产路径为空,无法写回";
			return false;
		}
		const std::string sourcePath = record->PrefabPath;
		std::string error;
		if (!Gameplay::SaveFromScene(*m_ActiveScene, root, sourcePath, &error))
		{
			if (message) *message = "写回资产失败: " + error;
			return false;
		}
		// 资产已跟上实例 → 覆盖不再是"偏离资产"的记录(与右键菜单同一条口径)。
		Gameplay::ClearOverrides(*record);
		MarkDocumentDirty();
		if (message) *message = "已写回资产: " + sourcePath;
		return true;
	}


bool EditorLayer::PrefabInstanceUnpack(Entity root, std::string* message){
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		const entt::entity handle = root.IsValid()
			? static_cast<entt::entity>(root) : entt::null;
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && handle != entt::null
			? m_ActiveScene->FindPrefabInstance(handle) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		const std::string source = record->PrefabPath;
		if (!Gameplay::UnpackInstance(*record))
		{
			if (message) *message = "断开链接失败";
			return false;
		}
		// 记录本身也要出注册表:之后它就是普通实体(否则实例条会以"空来源"的形态留着)。
		m_ActiveScene->RemovePrefabInstance(handle);
		MarkDocumentDirty();
		if (message) *message = "已断开链接: " + source;
		return true;
	}

void EditorLayer::StartCookingAction(){
		const std::string target =
			World::FileDialogs::SelectFolder("Select the output folder for the game package");
		if (!target.empty())
			StartCooking(target);
	}


void EditorLayer::GenerateLuaStubsAction(){
		// CPPT-3(T5b 硬要求):模块未加载窗口(启动失败 / `unloaded` / `reloading`)**禁止**跑
		// Lua 存根生成 —— Game 组件 schema 不在注册表里,写出来的存根缺 Game 组件块,而目标
		// 是入库文件(Layout-S6 实测:World.ScriptWorkflow 漂移门禁因此变红)。这里从"只警告"
		// 升级为"拒绝执行 + 显式提示";加载或回滚成功后(rolled-back 仍加载着旧模块)恢复可用。
		if (!IsCppModuleLoaded())
		{
			const std::string reason = Wui::Tr("notice.cppmodule.stub_blocked",
				"Lua stub generation is disabled while the C++ module (Game.dll) is not loaded; "
				"reload the module first, then generate the stubs");
			WLD_CORE_WARN("[Lua] {0}", reason);
			m_Shell.Notify(reason);
			return;
		}
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Lua API stub generation failed; keeping the last valid declarations.");
	}

}
