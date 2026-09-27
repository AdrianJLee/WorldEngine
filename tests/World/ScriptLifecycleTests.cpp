#include "wldpch.h"
#include "World/Core/Memory/PoolAllocator.h"
#include "World/Core/WorldContext.h"
#include "World/Core/LayerStack.h"
#include "World/Scene/Components.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/SceneSerializer.h"
#include "World/Scene/LuaStubGenerator.h"
#include "World/Script/LuauVm.h"
#include "World/Script/Sandbox.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptProperties.h"
#include "World/Script/ScriptValue.h"
#include "World/Modules/GameModuleReload.h"
#include "World/Script/BehaviorRegistry.h"

#include <box2d/box2d.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace World;
    namespace fs = std::filesystem;

    World::WorldContext& TestContext()
    {
        static World::WorldContext context;
        return context;
    }

    void Check(bool condition, const char* expression, int line)
    {
        if (!condition)
            throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
    }
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

    template<typename F>
    bool RejectsLogic(F&& action)
    {
        try { action(); }
        catch (const std::logic_error&) { return true; }
        return false;
    }

    void RunLua(const std::string& source)
    {
        std::string error;
        if (!ScriptEngine::GetState().RunString(source, "T02Test", &error))
            throw std::runtime_error(error);
    }

    // W1b:测试宿主的注入门面 —— 把 C++ 值装箱成 Luau 值(与真实的 Entity 绑定同一路径)。
    World::ScriptValue MakeEntityValue(Entity entity)
    {
        auto& bindings = ScriptEngine::GetBindingContext();
        World::ScriptValue value = bindings.NewUserdata("Entity");
        Entity* target = nullptr;
        CHECK(bindings.Unwrap("Entity", value, &target) && target != nullptr);
        new (target) Entity(entity);
        return value;
    }

    Entity ReadEntityValue(const World::ScriptValue& value)
    {
        Entity* entity = nullptr;
        ScriptEngine::GetBindingContext().Unwrap("Entity", value, &entity);
        if (!entity)
            throw std::runtime_error("expected an Entity userdata");
        return *entity;
    }

    struct Counts
    {
        int Creates = 0;
        int Updates = 0;
        int Destroys = 0;
        int Deletes = 0;
        int Returns = 0;
        int RunUpdates = 0;
        int LastUpdateFrame = -1;
    };

    struct ProbeContext
    {
        std::thread::id Owner = std::this_thread::get_id();
        std::map<uint32_t, Counts> Native;
        std::map<uint32_t, Counts> Lua;
        std::map<std::string, int> Observed;
        std::function<void(Entity, const std::string&)> NativeAction;
        std::function<void(Entity, const std::string&)> LuaAction;
        int Frame = 0;
        bool CorrectThread = true;
        bool OncePerFrame = true;
        bool SeparateTables = true;

        void Record(bool lua, uint32_t id, const std::string& phase, int localUpdates = 0)
        {
            CorrectThread &= std::this_thread::get_id() == Owner;
            auto& count = (lua ? Lua : Native)[id];
            if (phase == "create") { ++count.Creates; count.RunUpdates = 0; }
            else if (phase == "update")
            {
                OncePerFrame &= count.LastUpdateFrame != Frame;
                count.LastUpdateFrame = Frame;
                ++count.Updates;
                ++count.RunUpdates;
                if (lua && localUpdates != 0) SeparateTables &= localUpdates == count.RunUpdates;
            }
            else if (phase == "destroy") ++count.Destroys;
            else if (phase == "returned") ++count.Returns;
        }
    };

    ProbeContext* s_ProbeContext = nullptr;

    class NativeProbe final : public ScriptableEntity
    {
    public:
        explicit NativeProbe(ProbeContext& context) : m_Context(context) {}
        ~NativeProbe() override { ++m_Context.Native[m_Id].Deletes; }
        float Value = 7.5f;
        // CPPT-2:Enum / Asset / 只读摘要(IVec3)字段 —— 与生成物里真实脚本字段走同一条 schema 通道。
        int64_t Mode = 0;
        std::string Icon;
        glm::ivec3 Cell { 0 };

    private:
        void OnCreate() override
        {
            m_Id = static_cast<uint32_t>(GetEntity());
            Invoke("create");
        }
        void OnUpdate(Timestep) override
        {
            Invoke("update");
            m_Context.Record(false, m_Id, "returned");
        }
        void OnDestroy() override
        {
            CHECK(GetEntity().IsValid());
            CHECK(GetEntity().HasComponent<TagComponent>());
            Invoke("destroy");
        }
        void Invoke(const std::string& phase)
        {
            m_Context.Record(false, m_Id, phase);
            if (m_Context.NativeAction) m_Context.NativeAction(GetEntity(), phase);
        }
        ProbeContext& m_Context;
        uint32_t m_Id = 0;
    };

    // 2026-09-26 重写:组件只存脚本引用(ScriptName);实例化走 schema 的脚本工厂
    // —— 工厂在本文件 main() 里随 probeSchema 注册(factory 需要当前 ProbeContext)。
    void BindProbe(CppScriptComponent& script)
    {
        script.ScriptName = "T02NativeProbe";
    }

    struct Fixture
    {
        ProbeContext Context;
        Ref<Scene> World = CreateRef<Scene>(TestContext());

        Fixture()
        {
            CHECK(s_ProbeContext == nullptr);
            s_ProbeContext = &Context;
            auto& lua = ScriptEngine::GetState();
            auto& bindings = ScriptEngine::GetBindingContext();
            lua.SetGlobal("T02Record", bindings.CreateFunction("T02Record",
                [](const World::ScriptValue* args, std::size_t count) -> World::ScriptValue {
                    if (!s_ProbeContext || count < 3)
                        throw std::runtime_error("T02Record expects (id, phase, localUpdates)");
                    double id = 0.0;
                    double localUpdates = 0.0;
                    std::string phase;
                    if (!args[0].AsNumber(&id) || !args[1].AsString(&phase) || !args[2].AsNumber(&localUpdates))
                        throw std::runtime_error("T02Record argument types");
                    s_ProbeContext->Record(true, static_cast<uint32_t>(id), phase, static_cast<int>(localUpdates));
                    return World::ScriptValue::Nil();
                }));
            lua.SetGlobal("T02Action", bindings.CreateFunction("T02Action",
                [](const World::ScriptValue* args, std::size_t count) -> World::ScriptValue {
                    if (!s_ProbeContext || count < 2)
                        throw std::runtime_error("T02Action expects (entity, phase)");
                    const Entity entity = ReadEntityValue(args[0]);
                    std::string phase;
                    if (!args[1].AsString(&phase))
                        throw std::runtime_error("T02Action phase must be a string");
                    if (s_ProbeContext->LuaAction) s_ProbeContext->LuaAction(entity, phase);
                    return World::ScriptValue::Nil();
                }));
            lua.ClearGlobal("T02LoadMode");
            lua.ClearGlobal("T02Inheritance");
        }
        ~Fixture()
        {
            // Callback captures and counters outlive all scene cleanup, also on a failed CHECK.
            World.reset();
            auto& lua = ScriptEngine::GetState();
            lua.ClearGlobal("T02Record");
            lua.ClearGlobal("T02Action");
            lua.ClearGlobal("T02LoadMode");
            lua.ClearGlobal("T02Inheritance");
            lua.ClearGlobal("T02Entity");
            s_ProbeContext = nullptr;
        }
        Entity AddNative()
        {
            auto entity = Entity::CreateEntity(World.get(), "Native probe");
            BindProbe(entity.AddComponent<CppScriptComponent>());
            return entity;
        }
        Entity AddLua(const std::string& path = "scripts/tests/LifecycleProbe.lua")
        {
            auto entity = Entity::CreateEntity(World.get(), "Lua probe");
            entity.AddComponent<LuauScriptComponent>(path);
            return entity;
        }
        void Step()
        {
            ++Context.Frame;
            World->OnScriptUpdate(Timestep(1.0f / 60.0f));
        }
        void Stop() { World->OnRuntimeStop(); }
    };

    void SetLuaString(Entity entity, const std::string& name, const std::string& value)
    {
        std::vector<ScriptProperty>& properties = entity.GetComponent<LuauScriptComponent>().Properties;
        ScriptProperty* found = ScriptProperties::Find(properties, name);
        if (!found)
        {
            properties.push_back(ScriptProperty{ name, Schema::Kind::String, Schema::Value(value) });
            return;
        }
        found->Type = Schema::Kind::String;
        found->Value = value;
    }

    void CheckLuaReleased(Entity entity)
    {
        const auto& script = entity.GetComponent<LuauScriptComponent>();
        CHECK(script.Runtime.State != ScriptInstanceState::Running);
        CHECK(!script.Runtime.CreateEntered);
        CHECK(!script.LuaEnv.IsValid());
        CHECK(!script.ScriptTable.IsValid());
        CHECK(!script.OnCreateFunc.IsValid());
        CHECK(!script.OnUpdateFunc.IsValid());
        CHECK(!script.OnDestroyFunc.IsValid());
        CHECK(!script.RuntimeEntity);
    }

    void ManyInstancesAndRestart()
    {
        Fixture fixture;
        std::vector<Entity> luaEntities, nativeEntities;
        for (int i = 0; i < 130; ++i) luaEntities.push_back(fixture.AddLua());
        for (int i = 0; i < 4; ++i) nativeEntities.push_back(fixture.AddNative());
        fixture.World->OnScriptStart();
        fixture.World->OnScriptStart();
        for (int frame = 0; frame < 3; ++frame) fixture.Step();
        fixture.Stop();
        fixture.Stop();
        for (auto entity : luaEntities)
        {
            const auto& count = fixture.Context.Lua.at(static_cast<uint32_t>(entity));
            CHECK(count.Creates == 1 && count.Updates == 3 && count.Destroys == 1);
            CheckLuaReleased(entity);
        }
        for (auto entity : nativeEntities)
        {
            const auto& count = fixture.Context.Native.at(static_cast<uint32_t>(entity));
            CHECK(count.Creates == 1 && count.Updates == 3 && count.Destroys == 1 && count.Deletes == 1);
        }
        fixture.World->OnScriptStart();
        fixture.Step();
        fixture.Stop();
        for (auto entity : luaEntities)
        {
            const auto& count = fixture.Context.Lua.at(static_cast<uint32_t>(entity));
            CHECK(count.Creates == 2 && count.Updates == 4 && count.Destroys == 2);
        }
        CHECK(fixture.Context.CorrectThread && fixture.Context.OncePerFrame && fixture.Context.SeparateTables);
    }

    void OwnerThreadGuards()
    {
        Fixture fixture;
        fixture.AddLua();
        std::atomic<int> rejected { 0 };
        std::thread worker([&] {
            if (RejectsLogic([&] { fixture.World->OnScriptStart(); })) ++rejected;
            if (RejectsLogic([&] { static_cast<const Scene&>(*fixture.World).GetRegistry(); })) ++rejected;
            if (RejectsLogic([] { ScriptEngine::GetState(); })) ++rejected;
        });
        worker.join();
        CHECK(rejected == 3);
        CHECK(!fixture.World->IsActive());
        fixture.World->OnScriptStart();
        fixture.Step();
        CHECK(fixture.Context.CorrectThread);
    }

    void SelfDestroyAndRemove()
    {
        for (const std::string mode : { "destroy", "remove" })
        {
            Fixture fixture;
            auto native = fixture.AddNative();
            auto lua = fixture.AddLua();
            SetLuaString(lua, "Mode", mode);
            fixture.Context.NativeAction = [mode](Entity entity, const std::string& phase) {
                if (phase != "update") return;
                if (mode == "destroy")
                {
                    Entity::DestroyEntity(entity.GetScene(), entity);
                    Entity::DestroyEntity(entity.GetScene(), entity);
                }
                else
                {
                    entity.RemoveComponent<CppScriptComponent>();
                    entity.RemoveComponent<CppScriptComponent>();
                }
            };
            fixture.World->OnScriptStart();
            fixture.Step();
            const auto& n = fixture.Context.Native.at(static_cast<uint32_t>(native));
            const auto& l = fixture.Context.Lua.at(static_cast<uint32_t>(lua));
            CHECK(n.Updates == 1 && n.Returns == 1 && n.Destroys == 1 && n.Deletes == 1);
            CHECK(l.Updates == 1 && l.Returns == 1 && l.Destroys == 1);
            if (mode == "destroy") CHECK(!native && !lua);
            else CHECK(native && lua && !native.HasComponent<CppScriptComponent>() && !lua.HasComponent<LuauScriptComponent>());
            fixture.Step();
            fixture.Stop();
            CHECK(n.Updates == 1 && n.Destroys == 1 && l.Updates == 1 && l.Destroys == 1);
        }
    }

    void DestroyBeforeVictimUpdate()
    {
        Fixture fixture;
        fixture.AddNative();
        fixture.AddNative();
        auto view = static_cast<const Scene&>(*fixture.World).GetRegistry().view<CppScriptComponent>();
        auto iterator = view.begin();
        Entity first(fixture.World.get(), *iterator++);
        Entity second(fixture.World.get(), *iterator);
        auto luaVictim = fixture.AddLua();
        fixture.Context.NativeAction = [first, second, luaVictim](Entity entity, const std::string& phase) {
            if (entity == first && phase == "update")
            {
                Entity::DestroyEntity(entity.GetScene(), second);
                Entity::DestroyEntity(entity.GetScene(), luaVictim);
            }
        };
        fixture.World->OnScriptStart();
        fixture.Step();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(first)).Updates == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(second)).Updates == 0);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(second)).Destroys == 1);
        CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(luaVictim)).Updates == 0);
        CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(luaVictim)).Destroys == 1);
        CHECK(!second && !luaVictim);
    }

    void DeferredCreationAndPausedFlush()
    {
        Fixture fixture;
        auto source = fixture.AddNative();
        fixture.Context.NativeAction = [&fixture, source](Entity entity, const std::string& phase) {
            if (entity != source || phase != "create") return;
            CHECK(entity.GetScene()->DeferStructuralChange([&fixture](Scene& scene) {
                auto child = Entity::CreateEntity(&scene, "Deferred child");
                child.AddComponent<TransformComponent>();
                BindProbe(child.AddComponent<CppScriptComponent>());
                ++fixture.Context.Observed["first batch"];
                CHECK(scene.DeferStructuralChange([&fixture](Scene&) { ++fixture.Context.Observed["next batch"]; }));
            }));
        };
        fixture.World->OnScriptStart();
        CHECK(fixture.Context.Observed["first batch"] == 0);
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Observed["first batch"] == 1 && fixture.Context.Observed["next batch"] == 0);
        Entity child;
        const auto& registry = static_cast<const Scene&>(*fixture.World).GetRegistry();
        for (auto handle : registry.view<CppScriptComponent>())
            if (handle != static_cast<entt::entity>(source)) child = Entity(fixture.World.get(), handle);
        CHECK(child && child.HasComponent<TransformComponent>());
        CHECK(child.GetComponent<CppScriptComponent>().Runtime.State == ScriptInstanceState::Pending);
        CHECK(fixture.Context.Native.size() == 1); // Paused flush did not call OnCreate/OnUpdate.
        fixture.Step();
        CHECK(fixture.Context.Observed["next batch"] == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(child)).Creates == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(child)).Updates == 0);
        fixture.Step();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(child)).Updates == 1);
    }

    void CancelCommandsFromEndedSources()
    {
        for (bool lua : { false, true })
        for (const std::string cause : { "destroy", "remove", "fault" })
        {
            Fixture fixture;
            auto source = lua ? fixture.AddLua() : fixture.AddNative();
            auto action = [&fixture, cause, lua](Entity entity, const std::string& phase) {
                if (phase != "update") return;
                CHECK(entity.GetScene()->DeferStructuralChange([&fixture](Scene& scene) {
                    ++fixture.Context.Observed["invalid work"];
                    Entity::CreateEntity(&scene, "Must not be created");
                }));
                if (cause == "fault") throw std::runtime_error("intentional source failure");
                if (cause == "remove")
                {
                    if (lua) entity.RemoveComponent<LuauScriptComponent>();
                    else entity.RemoveComponent<CppScriptComponent>();
                }
                else Entity::DestroyEntity(entity.GetScene(), entity);
            };
            if (lua) fixture.Context.LuaAction = action;
            else fixture.Context.NativeAction = action;
            fixture.World->OnScriptStart();
            fixture.Step();
            fixture.World->FlushStructuralChanges();
            CHECK(fixture.Context.Observed["invalid work"] == 0);
            CHECK((lua ? fixture.Context.Lua : fixture.Context.Native).at(static_cast<uint32_t>(source)).Destroys == 1);
            if (cause == "fault")
            {
                if (lua) CHECK(source.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
                else CHECK(source.GetComponent<CppScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
            }
        }
    }

    void SynchronousWhitelistAndDeferredCleanup()
    {
        // C++ 类型化结构写模板仍不在白名单:回调内继续拒绝;
        // 只有 Lua 动态绑定与 Scene::CreateEntityShell 走 W3d 的同步白名单。
        {
            Fixture fixture;
            auto source = fixture.AddNative();
            fixture.Context.NativeAction = [&fixture](Entity entity, const std::string& phase) {
                if (phase != "update") return;
                auto* scene = entity.GetScene();
                fixture.Context.Observed["rejections"] += RejectsLogic([&] { Entity::CreateEntity(scene); });
                fixture.Context.Observed["rejections"] += RejectsLogic([&] { entity.AddComponent<TransformComponent>(); });
                fixture.Context.Observed["rejections"] += RejectsLogic([&] { scene->GetRegistry(); });
                fixture.Context.Observed["rejections"] += RejectsLogic([&] { entity.AddOrReplaceComponent<CppScriptComponent>(); });
            };
            fixture.World->OnScriptStart();
            fixture.Step();
            CHECK(fixture.Context.Observed["rejections"] == 4);
            CHECK(!source.HasComponent<TransformComponent>());
            CHECK(source.GetComponent<CppScriptComponent>().Runtime.State == ScriptInstanceState::Running);
        }

        // Lua 白名单:OnUpdate 内 CreateChild/AddComponent 当帧同步生效,
        // 同帧第二个脚本按快照语义看不到新实体(它自己的 assert 失败会置 Faulted)。
        {
            Fixture fixture;
            auto spawner = fixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
            auto observer = fixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
            SetLuaString(spawner, "Mode", "spawn");
            SetLuaString(observer, "Mode", "query");
            fixture.World->OnScriptStart();
            fixture.Step();
            CHECK(spawner.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
            CHECK(spawner.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
            CHECK(observer.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
            CHECK(observer.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());

            // OnScriptUpdate 结束后、渲染前实体与组件已经在场景里(当帧可见)。
            const auto& registry = static_cast<const Scene&>(*fixture.World).GetRegistry();
            Entity child;
            for (const entt::entity handle : registry.view<TagComponent>())
                if (registry.get<TagComponent>(handle).Tag == "A")
                {
                    child = Entity(fixture.World.get(), handle);
                    break;
                }
            CHECK(child && child.IsValid());
            CHECK(child.HasComponent<TransformComponent>());
            CHECK(child.HasComponent<SpriteComponent>());
            const auto& transform = child.GetComponent<TransformComponent>();
            CHECK(transform.Location.x == 1.0f && transform.Location.y == 2.0f && transform.Location.z == 3.0f);
            CHECK(child.GetComponent<HierarchyComponent>().Parent == static_cast<entt::entity>(spawner));
        }

        // Lua 动态 AddComponent 在同一个 OnUpdate 内提交:一次 Step 后 C++ 侧可见。
        {
            Fixture fixture;
            auto lua = fixture.AddLua();
            SetLuaString(lua, "Mode", "add");
            fixture.World->OnScriptStart();
            fixture.Step();
            CHECK(lua.HasComponent<TransformComponent>());
            CHECK(lua.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
            CHECK(lua.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
            fixture.Stop();
        }

        // Destroy/RemoveComponent 仍延迟:回调内句柄/组件保持可见,帧末提交后才消失。
        {
            Fixture destroyFixture;
            auto destroyer = destroyFixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
            SetLuaString(destroyer, "Mode", "destroy");
            destroyFixture.World->OnScriptStart();
            destroyFixture.Step();
            CHECK(destroyer.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
            const auto& registry = static_cast<const Scene&>(*destroyFixture.World).GetRegistry();
            bool doomedAlive = false;
            for (const entt::entity handle : registry.view<TagComponent>())
                doomedAlive |= registry.get<TagComponent>(handle).Tag == "Doomed";
            CHECK(!doomedAlive);
        }

        {
            Fixture removeFixture;
            auto remover = removeFixture.AddLua("scripts/tests/EntitySpawnProbe.lua");
            SetLuaString(remover, "Mode", "remove");
            removeFixture.World->OnScriptStart();
            removeFixture.Step();
            CHECK(remover.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
            CHECK(!remover.HasComponent<TransformComponent>());
        }
    }

    void NestedCommandsRetainScriptSource()
    {
        for (const std::string cause : { "alive", "destroy", "remove", "fault" })
        {
            Fixture fixture;
            auto source = fixture.AddNative();
            fixture.Context.NativeAction = [&fixture, source, cause](Entity entity, const std::string& phase) {
                if (phase != "update") return;
                CHECK(entity.GetScene()->DeferStructuralChange([&fixture, source, cause](Scene& scene) mutable {
                    ++fixture.Context.Observed["C1 executed"];
                    CHECK(scene.DeferStructuralChange([&fixture](Scene& nextScene) {
                        Entity::CreateEntity(&nextScene, "C2 child");
                        ++fixture.Context.Observed["C2 executed"];
                    }));
                    if (cause == "destroy") Entity::DestroyEntity(&scene, source);
                    else if (cause == "remove") source.RemoveComponent<CppScriptComponent>();
                    else if (cause == "fault") throw std::runtime_error("intentional C1 failure");
                    ++fixture.Context.Observed["C1 returned"];
                }));
            };
            fixture.World->OnScriptStart();
            fixture.Step(); // OnUpdate queues C1; end-of-frame commit executes C1 only.
            CHECK(fixture.Context.Observed["C1 executed"] == 1);
            CHECK(fixture.Context.Observed["C2 executed"] == 0);
            CHECK(fixture.Context.Observed["C1 returned"] == (cause == "fault" ? 0 : 1));
            CHECK(fixture.Context.Native.at(static_cast<uint32_t>(source)).Returns == 1);
            fixture.World->FlushStructuralChanges();
            CHECK(fixture.Context.Observed["C2 executed"] == (cause == "alive" ? 1 : 0));
            const auto& counts = fixture.Context.Native.at(static_cast<uint32_t>(source));
            CHECK(counts.Updates == 1);
            CHECK(counts.Destroys == (cause == "alive" ? 0 : 1));
            CHECK(counts.Deletes == counts.Destroys);
            if (cause == "destroy") CHECK(!source);
            else if (cause == "remove") CHECK(source && !source.HasComponent<CppScriptComponent>());
            else if (cause == "fault")
            {
                const auto& script = source.GetComponent<CppScriptComponent>();
                CHECK(script.Runtime.State == ScriptInstanceState::Faulted && script.Instance == nullptr);
                CHECK(script.Runtime.LastError.find("phase=StructuralChange") != std::string::npos);
                CHECK(script.Runtime.LastError.find("intentional C1 failure") != std::string::npos);
            }
        }
    }

    void ReplacementRequiresCleanup()
    {
        Fixture fixture;
        auto runningNative = fixture.AddNative();
        auto runningLua = fixture.AddLua();
        auto faultNative = fixture.AddNative();
        // 2026-09-26 重写:创建失败的判定不再是"工厂返回 null",而是 ScriptName 没有注册的类型。
        faultNative.GetComponent<CppScriptComponent>().ScriptName = "T02MissingProbe";
        auto faultLua = fixture.AddLua("scripts/tests/does-not-exist.lua");
        fixture.World->OnScriptStart();
        Entity pendingNative, pendingLua;
        CHECK(fixture.World->DeferStructuralChange([&](Scene&) {
            pendingNative = fixture.AddNative(); pendingLua = fixture.AddLua();
        }));
        fixture.World->FlushStructuralChanges();
        auto* oldInstance = runningNative.GetComponent<CppScriptComponent>().Instance;
        CHECK(fixture.World->DeferStructuralChange([&](Scene&) {
            for (auto entity : { runningNative, pendingNative, faultNative })
                fixture.Context.Observed["replace rejected"] += RejectsLogic([&] { entity.AddOrReplaceComponent<CppScriptComponent>(); });
            for (auto entity : { runningLua, pendingLua, faultLua })
                fixture.Context.Observed["replace rejected"] += RejectsLogic([&] { entity.AddOrReplaceComponent<LuauScriptComponent>(); });
        }));
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Observed["replace rejected"] == 6);
        CHECK(runningNative.GetComponent<CppScriptComponent>().Instance == oldInstance);
        CHECK(pendingNative.GetComponent<CppScriptComponent>().Runtime.State == ScriptInstanceState::Pending);
        CHECK(faultNative.GetComponent<CppScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
        CHECK(faultLua.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
        CHECK(faultNative.GetComponent<CppScriptComponent>().Instance == nullptr);
        CHECK(fixture.Context.Native.find(static_cast<uint32_t>(faultNative)) == fixture.Context.Native.end());
        CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(faultLua)) == fixture.Context.Lua.end());
        runningNative.RemoveComponent<CppScriptComponent>();
        runningLua.RemoveComponent<LuauScriptComponent>();
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(runningNative)).Deletes == 1);
        CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(runningLua)).Destroys == 1);
        CHECK(fixture.World->DeferStructuralChange([=](Scene&) mutable {
            BindProbe(runningNative.AddComponent<CppScriptComponent>());
            runningLua.AddComponent<LuauScriptComponent>("scripts/tests/LifecycleProbe.lua");
        }));
        fixture.World->FlushStructuralChanges();
        fixture.Step();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(runningNative)).Creates == 2);
        CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(runningLua)).Creates == 2);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(runningNative)).Updates == 0);
    }

    void StopFromCallbackAndPendingCancellation()
    {
        Fixture fixture;
        auto source = fixture.AddNative();
        auto pendingLua = fixture.AddLua();
        fixture.Context.NativeAction = [&fixture](Entity entity, const std::string& phase) {
            if (phase == "create")
            {
                CHECK(entity.GetScene()->DeferStructuralChange([&fixture](Scene&) { ++fixture.Context.Observed["late work"]; }));
                entity.GetScene()->OnRuntimeStop();
                ++fixture.Context.Observed["stop returned"];
            }
            else if (phase == "destroy")
            {
                fixture.Context.Observed["destroy refuses create"] = !entity.GetScene()->DeferStructuralChange([](Scene&) {});
                entity.RemoveComponent<CppScriptComponent>();
                Entity::DestroyEntity(entity.GetScene(), entity);
            }
        };
        fixture.World->OnScriptStart();
        CHECK(!fixture.World->IsActive());
        CHECK(fixture.Context.Observed["stop returned"] == 1);
        CHECK(fixture.Context.Observed["late work"] == 0);
        CHECK(fixture.Context.Observed["destroy refuses create"] == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(source)).Destroys == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(source)).Deletes == 1);
        CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(pendingLua)) == fixture.Context.Lua.end());
        CheckLuaReleased(pendingLua);
        fixture.World->FlushStructuralChanges();
        CHECK(!source);
    }

    void NativeErrorsReleaseExactlyOnce()
    {
        for (const std::string stage : { "create", "update", "destroy" })
        {
            Fixture fixture;
            auto bad = fixture.AddNative();
            auto good = fixture.AddNative();
            fixture.Context.NativeAction = [bad, stage](Entity entity, const std::string& phase) {
                if (entity == bad && phase == stage) throw std::runtime_error("intentional native callback failure");
            };
            fixture.World->OnScriptStart();
            fixture.Step();
            if (stage == "destroy") fixture.Stop();
            auto& script = bad.GetComponent<CppScriptComponent>();
            CHECK(script.Runtime.State == ScriptInstanceState::Faulted && script.Instance == nullptr && !script.Runtime.CreateEntered);
            CHECK(script.Runtime.LastError.find("T02NativeProbe") != std::string::npos);
            CHECK(script.Runtime.LastError.find("entity=") != std::string::npos);
            const auto error = script.Runtime.LastError;
            fixture.Step();
            fixture.Stop();
            fixture.Stop();
            CHECK(script.Runtime.LastError == error);
            const auto& count = fixture.Context.Native.at(static_cast<uint32_t>(bad));
            CHECK(count.Creates == 1 && count.Destroys == 1 && count.Deletes == 1);
            CHECK(count.Updates == (stage == "create" ? 0 : 1));
            CHECK(fixture.Context.Native.at(static_cast<uint32_t>(good)).Updates >= 1);
        }
    }

    void LuaErrorsReleaseExactlyOnce()
    {
        for (const std::string stage : { "OnCreate", "OnUpdate", "OnDestroy" })
        {
            Fixture fixture;
            auto bad = fixture.AddLua("scripts/tests/CallbackErrors.lua");
            auto good = fixture.AddLua();
            SetLuaString(bad, "FailStage", stage);
            fixture.World->OnScriptStart();
            fixture.Step();
            if (stage == "OnDestroy") fixture.Stop();
            auto& script = bad.GetComponent<LuauScriptComponent>();
            CHECK(script.Runtime.State == ScriptInstanceState::Faulted);
            CHECK(script.Runtime.LastError.find("CallbackErrors.lua") != std::string::npos);
            CHECK(script.Runtime.LastError.find("entity=") != std::string::npos);
            CHECK(script.Runtime.LastError.find("phase=" + stage) != std::string::npos);
            CHECK(script.Runtime.LastError.find("stack traceback") != std::string::npos);
            CheckLuaReleased(bad);
            const auto error = script.Runtime.LastError;
            fixture.Step();
            fixture.Stop();
            fixture.Stop();
            CHECK(script.Runtime.LastError == error);
            const auto& count = fixture.Context.Lua.at(static_cast<uint32_t>(bad));
            CHECK(count.Creates == 1 && count.Destroys == 1);
            CHECK(count.Updates == (stage == "OnCreate" ? 0 : 1));
            CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(good)).Updates >= 1);
        }
        for (const std::string mode : { "load", "return", "callback" })
        {
            Fixture fixture;
            ScriptEngine::GetState().SetGlobal("T02LoadMode", World::ScriptValue::String(mode));
            auto bad = fixture.AddLua("scripts/tests/CallbackErrors.lua");
            auto good = fixture.AddLua();
            fixture.World->OnScriptStart();
            fixture.Step();
            CHECK(bad.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
            CheckLuaReleased(bad);
            CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(bad)) == fixture.Context.Lua.end());
            CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(good)).Updates == 1);
        }
    }

    // SCRIPT-V7 P2-②:属性值的 variant 与声明类型不符(手改场景 / 坏存档)时不能抛 bad_variant_access ——
    // 跳过该字段 + 保留脚本自己的构造默认值,脚本照常 Running(不是 Faulted)。
    void MismatchedPropertyVariantIsSkipped()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        auto& script = entity.GetComponent<CppScriptComponent>();
        // Type 说 Float,值却是 string:写入前必须被拦下。
        script.Properties.push_back(ScriptProperty{ "Value", Schema::Kind::Float, Schema::Value(std::string("hand-edited")) });
        // 未设值(monostate)同样跳过:保留脚本默认,不写 0。
        script.Properties.push_back(ScriptProperty{ "MissingField", Schema::Kind::Float, Schema::Value {} });

        fixture.World->OnScriptStart();
        CHECK(script.Runtime.State == ScriptInstanceState::Running);
        CHECK(script.Instance != nullptr);
        CHECK(static_cast<NativeProbe*>(script.Instance)->Value == 7.5f);   // 构造默认值,不是 0 / 不是 Faulted
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Creates == 1);
        fixture.Stop();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Deletes == 1);
    }

    void CloneOnlyConfiguration()
    {
        Fixture fixture;
        auto source = fixture.AddNative();
        source.AddComponent<LuauScriptComponent>("scripts/tests/LifecycleProbe.lua");
        source.AddComponent<TransformComponent>();
        source.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Dynamic;
        source.AddComponent<BoxCollider2DComponent>();
        source.GetComponent<CppScriptComponent>().Properties.push_back(
            ScriptProperty{ "Value", Schema::Kind::Float, Schema::Value(19.0f) });
        SetLuaString(source, "Mode", "normal");
        fixture.World->OnRuntimeStart();
        CHECK(b2Body_IsValid(source.GetComponent<RigidBody2DComponent>().RuntimeBodyId));
        auto clone = CreateRef<Scene>(TestContext());
        Scene::CopyScene(fixture.World, clone);
        auto view = static_cast<const Scene&>(*clone).GetRegistry().view<CppScriptComponent, LuauScriptComponent, RigidBody2DComponent>();
        CHECK(view.begin() != view.end());
        Entity copy(clone.get(), *view.begin());
        auto& native = copy.GetComponent<CppScriptComponent>();
        CHECK(native.Instance == nullptr && native.Runtime.State == ScriptInstanceState::Pending && native.Runtime.Generation == 0);
        // 克隆只带配置:脚本引用 + 属性表;运行实例/状态从 Pending 重新起跑。
        CHECK(native.ScriptName == source.GetComponent<CppScriptComponent>().ScriptName);
        const ScriptProperty* clonedValue = ScriptProperties::Find(native.Properties, "Value");
        CHECK(clonedValue != nullptr && std::get<float>(clonedValue->Value) == 19.0f);
        CheckLuaReleased(copy);
        CHECK(copy.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Pending);
        CHECK(copy.GetComponent<LuauScriptComponent>().Runtime.Generation == 0);
        const ScriptProperty* clonedMode = ScriptProperties::Find(copy.GetComponent<LuauScriptComponent>().Properties, "Mode");
        CHECK(clonedMode != nullptr && std::get<std::string>(clonedMode->Value) == "normal");
        CHECK(B2_IS_NULL(copy.GetComponent<RigidBody2DComponent>().RuntimeBodyId));
        clone.reset();
        CHECK(source.GetComponent<CppScriptComponent>().Instance != nullptr);
        CHECK(source.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
        fixture.Stop();
        fixture.World->DuplicateEntity(source);
        CHECK(static_cast<const Scene&>(*fixture.World).GetRegistry().view<CppScriptComponent>().size() == 2);
    }

    void InheritedLuaCallbacksAndLookupErrors()
    {
        {
            Fixture fixture;
            ScriptEngine::GetState().SetGlobal("T02Inheritance", World::ScriptValue::Boolean(true));
            const auto first = fixture.AddLua();
            const auto second = fixture.AddLua();
            fixture.World->OnScriptStart();
            fixture.Step();
            fixture.Step();
            fixture.Stop();
            for (auto entity : { first, second })
            {
                const auto& counts = fixture.Context.Lua.at(static_cast<uint32_t>(entity));
                CHECK(counts.Creates == 1 && counts.Updates == 2 && counts.Destroys == 1);
                CHECK(entity.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
                CheckLuaReleased(entity);
            }
            CHECK(fixture.Context.CorrectThread && fixture.Context.SeparateTables);
        }
        {
            Fixture fixture;
            ScriptEngine::GetState().SetGlobal("T02LoadMode", World::ScriptValue::String("index"));
            auto bad = fixture.AddLua("scripts/tests/CallbackErrors.lua");
            auto healthy = fixture.AddLua();
            fixture.World->OnScriptStart();
            fixture.Step();
            auto& script = bad.GetComponent<LuauScriptComponent>();
            CHECK(script.Runtime.State == ScriptInstanceState::Faulted);
            CHECK(script.Runtime.LastError.find("intentional inherited lookup failure") != std::string::npos);
            CHECK(script.Runtime.LastError.find("CallbackErrors.lua") != std::string::npos);
            CHECK(script.Runtime.LastError.find("stack traceback") != std::string::npos);
            CheckLuaReleased(bad);
            const auto error = script.Runtime.LastError;
            fixture.Step();
            CHECK(script.Runtime.LastError == error);
            CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(bad)) == fixture.Context.Lua.end());
            CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(healthy)).Updates == 2);
        }
    }

    void PhysicsRemovalAndDependencies()
    {
        Fixture fixture;
        auto body = Entity::CreateEntity(fixture.World.get());
        body.AddComponent<TransformComponent>();
        body.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Dynamic;
        body.AddComponent<BoxCollider2DComponent>();
        auto colliderOnly = Entity::CreateEntity(fixture.World.get());
        colliderOnly.AddComponent<TransformComponent>();
        colliderOnly.AddComponent<CircleCollider2DComponent>();
        CHECK(RejectsLogic([&] { colliderOnly.RemoveComponent<TransformComponent>(); }));
        fixture.World->OnRuntimeStart();
        const auto firstBody = body.GetComponent<RigidBody2DComponent>().RuntimeBodyId;
        CHECK(b2Body_IsValid(firstBody));
        CHECK(RejectsLogic([&] { body.RemoveComponent<BoxCollider2DComponent>(); }));
        std::string reason;
        CHECK(!body.CanRemoveComponent(entt::type_id<TransformComponent>().hash(), &reason) && !reason.empty());
        CHECK(!colliderOnly.CanAddComponent(entt::type_id<RigidBody2DComponent>().hash(), &reason) && !reason.empty());
        CHECK(fixture.World->DeferStructuralChange([&fixture, colliderOnly](Scene&) mutable {
            fixture.Context.Observed["physics rejected"] += RejectsLogic([&] { colliderOnly.AddComponent<RigidBody2DComponent>(); });
        }));
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Observed["physics rejected"] == 1);
        body.RemoveComponent<RigidBody2DComponent>();
        body.RemoveComponent<RigidBody2DComponent>();
        fixture.World->FlushStructuralChanges();
        CHECK(!body.HasComponent<RigidBody2DComponent>() && !b2Body_IsValid(firstBody));
        fixture.Step();
        fixture.Stop();
        body.AddComponent<RigidBody2DComponent>();
        fixture.World->OnSimulationStart();
        const auto secondBody = body.GetComponent<RigidBody2DComponent>().RuntimeBodyId;
        CHECK(b2Body_IsValid(secondBody));
        Entity::DestroyEntity(fixture.World.get(), body);
        fixture.World->FlushStructuralChanges();
        CHECK(!body && !b2Body_IsValid(secondBody));
        fixture.World->OnSimulationStop();
        fixture.World->OnSimulationStop();
        CHECK(!fixture.World->IsActive());
    }

    void CameraReacquisitionAndExpiredHandles()
    {
        Fixture fixture;
        CHECK(!fixture.World->GetPrimaryCameraEntity());
        auto camera = Entity::CreateEntity(fixture.World.get());
        camera.AddComponent<TransformComponent>();
        camera.AddComponent<CameraComponent>();
        CHECK(fixture.World->GetPrimaryCameraEntity() == camera);
        fixture.World->OnScriptStart();
        camera.RemoveComponent<CameraComponent>();
        CHECK(!fixture.World->GetPrimaryCameraEntity());
        fixture.World->FlushStructuralChanges();
        CHECK(camera && !camera.HasComponent<CameraComponent>());
        fixture.Stop();
        camera.RemoveComponent<TransformComponent>();
        CHECK(!camera.HasComponent<TransformComponent>());
        Entity old;
        {
            auto shortScene = CreateRef<Scene>(TestContext());
            old = Entity::CreateEntity(shortScene.get());
            ScriptEngine::GetState().SetGlobal("T02Entity", MakeEntityValue(old));
        }
        CHECK(!old.IsValid());
        CHECK(RejectsLogic([&] { old.GetScene(); }));
        CHECK(RejectsLogic([&] { old.HasComponent<TagComponent>(); }));
        RunLua("assert(not T02Entity:IsValid()); local ok, err = pcall(function() return T02Entity:GetID() end); assert(not ok and err ~= nil)");
        auto deleted = Entity::CreateEntity(fixture.World.get());
        const auto previous = static_cast<uint32_t>(deleted);
        Entity::DestroyEntity(fixture.World.get(), deleted);
        auto replacement = Entity::CreateEntity(fixture.World.get());
        CHECK(!deleted && replacement && static_cast<uint32_t>(replacement) != previous);
    }

    struct RecordingLayer final : Layer
    {
        RecordingLayer(int id, std::vector<int>& events, int& destructed) : Id(id), Events(events), Destructed(destructed) {}
        ~RecordingLayer() override { ++Destructed; }
        void OnAttach() override { Events.push_back(Id); if (FailAttach) throw std::runtime_error("attach probe"); }
        void OnDetach() override { Events.push_back(-Id); if (FailDetach) throw std::runtime_error("detach probe"); }
        int Id;
        std::vector<int>& Events;
        int& Destructed;
        bool FailAttach = false;
        bool FailDetach = false;
    };

    void NonOwningLayerDetachOrder()
    {
        std::vector<int> events;
        int destructed = 0;
        {
            RecordingLayer overlayLayer(1, events, destructed), editor(2, events, destructed), extra(3, events, destructed), failed(4, events, destructed);
            {
                LayerStack stack;
                stack.PushOverLay(&overlayLayer); // Attached first; intentionally last in render order.
                stack.PushLayer(&editor);
                stack.PushLayer(&extra);
                CHECK(RejectsLogic([&] { stack.PushLayer(&editor); }));
                stack.PopLayer(&extra);
                stack.PopLayer(&extra);
                failed.FailAttach = true;
                bool failedAttach = false;
                try { stack.PushLayer(&failed); } catch (const std::runtime_error&) { failedAttach = true; }
                CHECK(failedAttach);
                editor.FailDetach = true;
                stack.DetachAll();
                stack.DetachAll();
                CHECK(destructed == 0);
                CHECK((events == std::vector<int> { 1, 2, 3, -3, 4, -2, -1 }));
            }
            CHECK(destructed == 0);
        }
        CHECK(destructed == 4);
    }

    void NativePropertiesConfigureFactoryInstance()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        auto& script = entity.GetComponent<CppScriptComponent>();

        // 编辑态(2026-09-26 重写):不实例化脚本,属性表就是唯一配置来源。
        CHECK(script.Instance == nullptr);
        CHECK(script.Runtime.State == ScriptInstanceState::Pending);
        script.Properties.push_back(ScriptProperty{ "Value", Schema::Kind::Float, Schema::Value(19.0f) });
        const ScriptProperty* saved = ScriptProperties::Find(script.Properties, "Value");
        CHECK(saved != nullptr);
        CHECK(std::get<float>(saved->Value) == 19.0f);
        CHECK(fixture.Context.Native.find(static_cast<uint32_t>(entity)) == fixture.Context.Native.end());

        // Play:实例由 schema 的脚本工厂按 ScriptName 创建,创建前把保存的属性值套用到实例字段。
        fixture.World->OnScriptStart();
        CHECK(script.Runtime.State == ScriptInstanceState::Running);
        CHECK(script.Instance != nullptr);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Creates == 1);
        auto* instance = static_cast<NativeProbe*>(script.Instance);
        CHECK(instance->Value == 19.0f);   // 场景保存的属性值覆盖了脚本构造默认值 7.5

        // 运行期读取不新建、不销毁实例。
        CHECK(entity.GetComponent<CppScriptComponent>().Instance == script.Instance);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Deletes == 0);

        fixture.Step();
        fixture.Stop();
        CHECK(script.Instance == nullptr);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Destroys == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Deletes == 1);
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot read " + path.u8string());
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    fs::path s_OutputDirectory;

    void SyntaxErrorsAndPreviewCache()
    {
        const auto invalidPath = s_OutputDirectory / "invalid.lua";
        { std::ofstream file(invalidPath, std::ios::binary); file << "return { OnCreate = function( }\n"; CHECK(file.good()); }
        Fixture fixture;
        auto invalid = fixture.AddLua(invalidPath.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string());
        fixture.World->OnScriptStart();
        CHECK(invalid.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Faulted);
        CHECK(invalid.GetComponent<LuauScriptComponent>().Runtime.LastError.find("phase=Load") != std::string::npos);
        CheckLuaReleased(invalid);
        fixture.Stop();
        auto preview = fixture.AddLua("scripts/tests/CallbackErrors.lua");
        auto& script = preview.GetComponent<LuauScriptComponent>();
        CHECK(ScriptEngine::SyncScriptDeclarations(script, nullptr, nullptr));
        SetLuaString(preview, "FailStage", "Saved editor value");
        // 换成语法坏掉的脚本:声明同步读不到默认值(只留诊断),但**不碰**组件属性表,
        // 也不把编辑态组件打成 Faulted(SCRIPT-V7:旧 InitScriptForEditor 的 EditorLoad 失败语义已删除)。
        script.ScriptPath = invalid.GetComponent<LuauScriptComponent>().ScriptPath;
        std::vector<std::string> declarationDiagnostics;
        CHECK(ScriptEngine::SyncScriptDeclarations(script, &declarationDiagnostics, nullptr));
        CHECK(!declarationDiagnostics.empty());   // 读默认值失败有可读诊断
        const ScriptProperty* savedFailStage = ScriptProperties::Find(script.Properties, "FailStage");
        CHECK(savedFailStage != nullptr);
        CHECK(std::get<std::string>(savedFailStage->Value) == "Saved editor value");
        CHECK(script.Runtime.State != ScriptInstanceState::Faulted);
        CHECK(!script.LuaEnv.IsValid() && !script.ScriptTable.IsValid());
    }

    // VEC-A1(D2):向量属性 —— 注解 `vec3/vec2/vec4` → 属性表;默认值取脚本表里的向量 userdata
    // (**拷贝**出来,不存指针);同名同类型保值;未设保持 monostate;类型不符 → 诊断 + 不进属性表;
    // C++ 侧 schema 的 Vec3 字段走同一条 SyncFromSchema 模型。
    void VectorPropertiesFromAnnotationsAndSchema()
    {
        const auto vectorPath = s_OutputDirectory / "vec_properties.lua";
        {
            std::ofstream file(vectorPath, std::ios::binary);
            file <<
                "---@field Offset vec3 位置偏移\n"
                "---@field Scale vec2 缩放\n"
                "---@field UnsetVec vec4\n"
                "return { Offset = vec3.new(1.5, 2.5, 3.5), Scale = vec2.new(0.25, 0.5) }\n";
            CHECK(file.good());
        }
        const std::string logical = vectorPath.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();

        // ① 注解 vec3/vec2/vec4 → Schema::Kind 的对应支;Doc 取注解第三段。
        std::vector<ScriptProperties::Declaration> declarations;
        std::vector<std::string> diagnostics;
        std::string error;
        CHECK(ScriptEngine::DescribeScriptDeclarations(logical, declarations, &diagnostics, &error));
        CHECK(error.empty());
        CHECK(diagnostics.empty());
        CHECK(declarations.size() == 3);
        CHECK(declarations[0].Name == "Offset" && declarations[0].Type == Schema::Kind::Vec3);
        CHECK(declarations[0].Doc == "位置偏移");
        CHECK(declarations[1].Name == "Scale" && declarations[1].Type == Schema::Kind::Vec2);
        CHECK(declarations[1].Doc == "缩放");
        CHECK(declarations[2].Name == "UnsetVec" && declarations[2].Type == Schema::Kind::Vec4);

        // ② 默认值 = 脚本表里的 vec3.new(...)/vec2.new(...),从 userdata 拷出来(不存指针)。
        CHECK(std::holds_alternative<glm::vec3>(declarations[0].Default));
        CHECK(std::get<glm::vec3>(declarations[0].Default) == glm::vec3(1.5f, 2.5f, 3.5f));
        CHECK(std::holds_alternative<glm::vec2>(declarations[1].Default));
        CHECK(std::get<glm::vec2>(declarations[1].Default) == glm::vec2(0.25f, 0.5f));
        // ④ 注解在、脚本表里没有 → 默认值保持未设(monostate),绝不写零值。
        CHECK(std::holds_alternative<std::monostate>(declarations[2].Default));

        // 夹具同时只允许一个(Fixture 构造断言)。
        {
            Fixture fixture;
            auto entity = fixture.AddLua(logical);
            auto& script = entity.GetComponent<LuauScriptComponent>();
            CHECK(ScriptEngine::SyncScriptDeclarations(script, &diagnostics, &error));
            CHECK(error.empty());
            CHECK(script.Properties.size() == 3);
            const ScriptProperty* offset = ScriptProperties::Find(script.Properties, "Offset");
            CHECK(offset != nullptr && offset->Type == Schema::Kind::Vec3);
            CHECK(std::get<glm::vec3>(offset->Value) == glm::vec3(1.5f, 2.5f, 3.5f));
            CHECK(ScriptProperties::IsUnset(*ScriptProperties::Find(script.Properties, "UnsetVec")));

            // ③ 编辑器改值 → 重新同步(同名同类型)保留改过的值;未设字段仍是未设。
            std::get<glm::vec3>(ScriptProperties::Find(script.Properties, "Offset")->Value) = glm::vec3(9.0f, 8.0f, 7.0f);
            CHECK(ScriptEngine::SyncScriptDeclarations(script, nullptr, &error));
            CHECK(std::get<glm::vec3>(ScriptProperties::Find(script.Properties, "Offset")->Value) == glm::vec3(9.0f, 8.0f, 7.0f));
            CHECK(ScriptProperties::IsUnset(*ScriptProperties::Find(script.Properties, "UnsetVec")));

            // 运行期:属性以 vec3 userdata 写回脚本表(类型不丢,坐标可读)。
            fixture.World->OnScriptStart();
            CHECK(script.Runtime.State == ScriptInstanceState::Running);
            World::ScriptValue runtimeOffset = script.ScriptTable.GetField("Offset");
            CHECK(ScriptEngine::GetBindingContext().IsUserdataOfType("vec3", runtimeOffset));
            glm::vec3* runtimePointer = nullptr;
            CHECK(ScriptEngine::GetBindingContext().Unwrap("vec3", runtimeOffset, &runtimePointer) && runtimePointer != nullptr);
            CHECK(*runtimePointer == glm::vec3(9.0f, 8.0f, 7.0f));
            fixture.Stop();
        }

        // ⑤ 注解写 vec3、脚本表里放 number → 诊断 + 该字段不进属性表(不静默)。
        const auto mismatchPath = s_OutputDirectory / "vec_mismatch.lua";
        {
            std::ofstream file(mismatchPath, std::ios::binary);
            file << "---@field Offset vec3 位置偏移\nreturn { Offset = 1.0 }\n";
            CHECK(file.good());
        }
        const std::string mismatchLogical = mismatchPath.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();
        std::vector<ScriptProperties::Declaration> mismatchDeclarations;
        std::vector<std::string> mismatchDiagnostics;
        CHECK(ScriptEngine::DescribeScriptDeclarations(mismatchLogical, mismatchDeclarations, &mismatchDiagnostics, &error));
        CHECK(error.empty());
        CHECK(mismatchDeclarations.empty());
        CHECK(mismatchDiagnostics.size() == 1);
        CHECK(mismatchDiagnostics.front().find("different value type") != std::string::npos);
        {
            Fixture mismatchFixture;
            auto mismatchEntity = mismatchFixture.AddLua(mismatchLogical);
            auto& mismatchScript = mismatchEntity.GetComponent<LuauScriptComponent>();
            std::vector<std::string> syncDiagnostics;
            CHECK(ScriptEngine::SyncScriptDeclarations(mismatchScript, &syncDiagnostics, &error));
            CHECK(error.empty());
            CHECK(syncDiagnostics.size() == 1);
            CHECK(mismatchScript.Properties.empty());
        }

        // ⑥ C++ 侧:Schema::TypeSchema 里的 Vec3 字段经 SyncFromSchema 进同一份属性模型;
        //    KindName/KindFromName 是存档里 `Type: Vec3` 的读写对。
        Schema::FieldSchema vectorField;
        vectorField.Id = Schema::FieldId{ Schema::Fnv1a64("VecPropertyProbe.Offset") };
        vectorField.Name = "Offset";
        vectorField.K = Schema::Kind::Vec3;
        vectorField.Default = glm::vec3(1.0f, 2.0f, 3.0f);
        Schema::TypeSchema vectorType;
        vectorType.Id = Schema::TypeId("VecPropertyProbe");
        vectorType.DisplayName = "VecPropertyProbe";
        vectorType.Category = Schema::TypeCategory::Script;
        vectorType.Fields.push_back(vectorField);
        std::vector<ScriptProperty> schemaProperties;
        ScriptProperties::SyncFromSchema(schemaProperties, vectorType);
        CHECK(schemaProperties.size() == 1);
        CHECK(schemaProperties[0].Name == "Offset" && schemaProperties[0].Type == Schema::Kind::Vec3);
        CHECK(std::get<glm::vec3>(schemaProperties[0].Value) == glm::vec3(1.0f, 2.0f, 3.0f));
        CHECK(std::string(ScriptProperties::KindName(Schema::Kind::Vec3)) == "Vec3");
        CHECK(ScriptProperties::KindFromName("Vec3") == Schema::Kind::Vec3);
        CHECK(ScriptProperties::KindFromName("Nope") == Schema::Kind::None);
    }

    // VEC-E1(E3①,2026-09-27 用户口径「第 45 行 ExtraInfo 这种裸 table 要全部进面板」):
    // `---@field X table` 不再只给一行只读摘要 —— 用脚本表里的值递归推断结构:
    // 字符串键 → 结构化子行(可展开/可编辑/随场景保存);连续整数键 1..n → 数组行(可增删);
    // 推不出来的形态(数字与字符串混键 / 数字键不连续)仍然只读摘要 + 诊断(不猜)。
    void BareTableAnnotationsInferStructure()
    {
        const auto path = s_OutputDirectory / "bare_table_inference.lua";
        {
            std::ofstream file(path, std::ios::binary);
            file <<
                "---@field ExtraInfo table 任意表(裸 table)\n"
                "---@field RawNumbers table 裸 table 数组\n"
                "---@field Opaque table 推不出来的表\n"
                "---@field Unset table 没有初值\n"
                "return {\n"
                "    ExtraInfo = { note = \"运行期自用\", level = 1, inner = { depth = 2 } },\n"
                "    RawNumbers = { 10, 20, 30 },\n"
                "    Opaque = { [1] = 1, extra = 2 },\n"
                "}\n";
            CHECK(file.good());
        }
        const std::string logical = path.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();

        const auto findDeclaration = [](const std::vector<ScriptProperties::Declaration>& list,
            const std::string& name) -> const ScriptProperties::Declaration*
        {
            for (const ScriptProperties::Declaration& item : list)
                if (item.Name == name)
                    return &item;
            return nullptr;
        };

        std::vector<ScriptProperties::Declaration> declarations;
        std::vector<std::string> diagnostics;
        std::string error;
        CHECK(ScriptEngine::DescribeScriptDeclarations(logical, declarations, &diagnostics, &error));
        CHECK(error.empty());

        // ① 字符串键 → 结构化子行:子字段带类型与脚本里的初值(嵌套表递归)。
        const ScriptProperties::Declaration* extra = findDeclaration(declarations, "ExtraInfo");
        CHECK(extra != nullptr);
        CHECK(extra->Type == Schema::Kind::Object);
        CHECK(extra->Collection == ScriptPropertyCollection::Struct);
        CHECK(extra->TypeName == "table");
        CHECK(!extra->ReadOnly);
        CHECK(extra->Doc == "任意表(裸 table)");
        CHECK(extra->Fields.size() == 3);
        {
            const ScriptProperties::Declaration* note = findDeclaration(extra->Fields, "note");
            const ScriptProperties::Declaration* level = findDeclaration(extra->Fields, "level");
            const ScriptProperties::Declaration* inner = findDeclaration(extra->Fields, "inner");
            CHECK(note != nullptr && note->Type == Schema::Kind::String);
            CHECK(std::get<std::string>(note->Default) == "运行期自用");
            CHECK(level != nullptr && level->Type == Schema::Kind::Int32);
            CHECK(std::get<int32_t>(level->Default) == 1);
            CHECK(inner != nullptr && inner->Collection == ScriptPropertyCollection::Struct);
            CHECK(inner->Fields.size() == 1 && inner->Fields[0].Name == "depth");
            CHECK(std::get<int32_t>(inner->Fields[0].Default) == 2);
        }

        // ② 连续整数键 1..n → 数组行(元素类型 + 初值;面板侧同一条 +/- 增删路径)。
        const ScriptProperties::Declaration* raw = findDeclaration(declarations, "RawNumbers");
        CHECK(raw != nullptr);
        CHECK(raw->Collection == ScriptPropertyCollection::Array);
        CHECK(raw->ElementKind == Schema::Kind::Int32);
        CHECK(!raw->ReadOnly);
        CHECK(raw->Fields.size() == 3);
        CHECK(raw->Fields[0].Name == "1" && std::get<int32_t>(raw->Fields[0].Default) == 10);
        CHECK(std::get<int32_t>(raw->Fields[2].Default) == 30);

        // ③ 推不出来的形态 → 只读摘要 + 诊断(与"不静默丢弃、也不猜"同一口径)。
        const ScriptProperties::Declaration* opaque = findDeclaration(declarations, "Opaque");
        CHECK(opaque != nullptr);
        CHECK(opaque->ReadOnly);
        CHECK(opaque->Fields.empty());
        CHECK(diagnostics.size() == 1);
        CHECK(diagnostics.front().find("non-consecutive") != std::string::npos
            || diagnostics.front().find("mixes numeric and string keys") != std::string::npos);

        // ④ 只有声明、没有初值 → 连结构都推不出来,保持只读摘要(不凭空展开)。
        const ScriptProperties::Declaration* unset = findDeclaration(declarations, "Unset");
        CHECK(unset != nullptr);
        CHECK(unset->ReadOnly);
        CHECK(unset->Fields.empty());

        // ⑤ Luau 组件同步入口同样吃到这份结构(面板/存档用的是 ScriptProperty)。
        Fixture fixture;
        auto entity = fixture.AddLua(logical);
        auto& script = entity.GetComponent<LuauScriptComponent>();
        std::vector<std::string> syncDiagnostics;
        CHECK(ScriptEngine::SyncScriptDeclarations(script, &syncDiagnostics, &error));
        CHECK(error.empty());
        const ScriptProperty* property = ScriptProperties::Find(script.Properties, "ExtraInfo");
        CHECK(property != nullptr);
        CHECK(!property->ReadOnly);
        CHECK(property->Collection == ScriptPropertyCollection::Struct);
        CHECK(ScriptProperties::Find(property->Children, "note") != nullptr);
        CHECK(std::get<std::string>(ScriptProperties::Find(property->Children, "note")->Value) == "运行期自用");
        const ScriptProperty* numbers = ScriptProperties::Find(script.Properties, "RawNumbers");
        CHECK(numbers != nullptr && numbers->Collection == ScriptPropertyCollection::Array);
        CHECK(numbers->Children.size() == 3);
    }

    // VEC-C1(数组/映射):注解 `{T}` / `{K: V}`、未注解数组的类型推导、合并对齐与 Luau 读写。
    void CollectionPropertiesFromAnnotationsAndInference()
    {
        const auto path = s_OutputDirectory / "collection_properties.lua";
        {
            std::ofstream file(path, std::ios::binary);
            file <<
                "---@field Scores {number} 得分列表\n"
                "---@field Config {string: number} 数值配置\n"
                "---@field Grid {{number}} 网格(数组的数组)\n"
                "---@field Vecs {string: vec3} 向量表\n"
                "---@field Unset {number}\n"
                "return {\n"
                "    Scores = { 12.5, 13.5 },\n"
                "    Config = { hp = 10.0, mp = 5.0 },\n"
                "    Grid = { { 1.5, 2.5 }, { 3.5, 4.5 } },\n"
                "    Vecs = { eye = vec3.new(1.0, 2.0, 3.0) },\n"
                "    RawScores = { 1, 2, 3 },\n"
                "    BadList = { 1, \"two\" },\n"
                "}\n";
            CHECK(file.good());
        }
        const std::string logical = path.lexically_relative(fs::path(WLD_ASSETPATH)).generic_string();

        const auto findDeclaration = [](const std::vector<ScriptProperties::Declaration>& list,
            const std::string& name) -> const ScriptProperties::Declaration*
        {
            for (const ScriptProperties::Declaration& item : list)
                if (item.Name == name)
                    return &item;
            return nullptr;
        };

        // ① 注解:`{number}` → Array(Float);`{string: number}` → Map(String→Float);
        //    嵌套 `{{number}}` / `{string: vec3}` 在护栏内支持一层;元素初值 = 脚本表里的值。
        std::vector<ScriptProperties::Declaration> declarations;
        std::vector<std::string> diagnostics;
        std::string error;
        CHECK(ScriptEngine::DescribeScriptDeclarations(logical, declarations, &diagnostics, &error));
        CHECK(error.empty());
        CHECK(declarations.size() == 7);   // 注解 5 条 + 推断 2 条(RawScores / BadList)

        const ScriptProperties::Declaration* scores = findDeclaration(declarations, "Scores");
        CHECK(scores != nullptr);
        CHECK(scores->Type == Schema::Kind::Object);
        CHECK(scores->Collection == ScriptPropertyCollection::Array);
        CHECK(scores->ElementKind == Schema::Kind::Float);
        CHECK(scores->Doc == "得分列表");
        CHECK(scores->Fields.size() == 2);
        CHECK(scores->Fields[0].Name == "1" && scores->Fields[0].Type == Schema::Kind::Float);
        CHECK(std::get<float>(scores->Fields[0].Default) == 12.5f);
        CHECK(std::get<float>(scores->Fields[1].Default) == 13.5f);

        const ScriptProperties::Declaration* config = findDeclaration(declarations, "Config");
        CHECK(config != nullptr);
        CHECK(config->Collection == ScriptPropertyCollection::Map);
        CHECK(config->KeyKind == Schema::Kind::String);
        CHECK(config->ElementKind == Schema::Kind::Float);
        CHECK(config->Fields.size() == 2);
        {
            const ScriptProperties::Declaration* hp = findDeclaration(config->Fields, "hp");
            const ScriptProperties::Declaration* mp = findDeclaration(config->Fields, "mp");
            CHECK(hp != nullptr && std::get<float>(hp->Default) == 10.0f);
            CHECK(mp != nullptr && std::get<float>(mp->Default) == 5.0f);
        }

        const ScriptProperties::Declaration* grid = findDeclaration(declarations, "Grid");
        CHECK(grid != nullptr);
        CHECK(grid->Collection == ScriptPropertyCollection::Array);
        CHECK(grid->ElementKind == Schema::Kind::Object);   // 元素本身是集合
        CHECK(grid->Fields.size() == 2);
        CHECK(grid->Fields[0].Name == "1");
        CHECK(grid->Fields[0].Collection == ScriptPropertyCollection::Array);
        CHECK(grid->Fields[0].ElementKind == Schema::Kind::Float);
        CHECK(grid->Fields[0].Fields.size() == 2);
        CHECK(std::get<float>(grid->Fields[0].Fields[1].Default) == 2.5f);
        CHECK(std::get<float>(grid->Fields[1].Fields[0].Default) == 3.5f);

        const ScriptProperties::Declaration* vecs = findDeclaration(declarations, "Vecs");
        CHECK(vecs != nullptr);
        CHECK(vecs->Collection == ScriptPropertyCollection::Map);
        CHECK(vecs->ElementKind == Schema::Kind::Vec3);
        CHECK(vecs->Fields.size() == 1 && vecs->Fields[0].Name == "eye");
        CHECK(std::get<glm::vec3>(vecs->Fields[0].Default) == glm::vec3(1.0f, 2.0f, 3.0f));

        // 只有注解、脚本表里没有 → 可编辑的空数组(不是只读摘要)。
        const ScriptProperties::Declaration* unset = findDeclaration(declarations, "Unset");
        CHECK(unset != nullptr);
        CHECK(unset->Collection == ScriptPropertyCollection::Array);
        CHECK(unset->ElementKind == Schema::Kind::Float);
        CHECK(unset->Fields.empty() && !unset->ReadOnly);

        // ② 未注解 `{1, 2, 3}` → 连续整数键推断成 Array,元素初值 1/2/3。
        const ScriptProperties::Declaration* raw = findDeclaration(declarations, "RawScores");
        CHECK(raw != nullptr);
        CHECK(raw->Collection == ScriptPropertyCollection::Array);
        CHECK(raw->ElementKind == Schema::Kind::Int32);
        CHECK(raw->Fields.size() == 3);
        CHECK(raw->Fields[0].Name == "1" && std::get<int32_t>(raw->Fields[0].Default) == 1);
        CHECK(std::get<int32_t>(raw->Fields[2].Default) == 3);

        // ③ 异质数组 `{1, "two"}` → 只读摘要 + 诊断(不静默丢弃、也不猜类型)。
        const ScriptProperties::Declaration* bad = findDeclaration(declarations, "BadList");
        CHECK(bad != nullptr);
        CHECK(bad->ReadOnly);
        CHECK(bad->Collection == ScriptPropertyCollection::Array);
        CHECK(bad->Fields.empty());
        CHECK(diagnostics.size() == 1);
        CHECK(diagnostics.front().find("different value types") != std::string::npos);

        // ⑤ 合并:同名同类型(含数组元素下标 / 映射键)保值;元素/键增删后按名对齐。
        const auto arrayDeclaration = [](const std::vector<Schema::Value>& values)
        {
            ScriptProperties::Declaration declaration;
            declaration.Name = "Scores";
            declaration.Type = Schema::Kind::Object;
            declaration.Collection = ScriptPropertyCollection::Array;
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                ScriptProperties::Declaration child;
                child.Name = std::to_string(index + 1);
                child.Type = Schema::Kind::Float;
                child.Default = values[index];
                declaration.Fields.push_back(std::move(child));
            }
            declaration.ElementKind = Schema::Kind::Float;
            return declaration;
        };
        const auto mapDeclaration = [](const std::vector<std::pair<std::string, Schema::Value>>& entries)
        {
            ScriptProperties::Declaration declaration;
            declaration.Name = "Config";
            declaration.Type = Schema::Kind::Object;
            declaration.Collection = ScriptPropertyCollection::Map;
            declaration.KeyKind = Schema::Kind::String;
            declaration.ElementKind = Schema::Kind::Float;
            for (const auto& [key, value] : entries)
            {
                ScriptProperties::Declaration child;
                child.Name = key;
                child.Type = Schema::Kind::Float;
                child.Default = value;
                declaration.Fields.push_back(std::move(child));
            }
            return declaration;
        };

        std::vector<ScriptProperty> merged;
        ScriptProperties::SyncFromDeclarations(merged, { arrayDeclaration({ 1.0f, 2.0f, 3.0f }) });
        CHECK(merged.size() == 1 && merged[0].Children.size() == 3);
        std::get<float>(merged[0].Children[1].Value) = 99.0f;   // 编辑器改过第 2 个元素
        ScriptProperties::SyncFromDeclarations(merged, { arrayDeclaration({ 0.0f, 0.0f }) });
        CHECK(merged[0].Children.size() == 2);                   // 第 3 个元素删除
        // D1:第 1 个元素从没被编辑过(值 == 旧声明默认值 1.0)→ 视为"未设",取新默认值 0.0;
        // 第 2 个元素是编辑器改过的(99.0 ≠ 旧默认值 2.0)→ 按下标(名字)对齐保值。
        CHECK(std::get<float>(merged[0].Children[0].Value) == 0.0f);
        CHECK(std::get<float>(merged[0].Children[1].Value) == 99.0f);

        ScriptProperties::SyncFromDeclarations(merged,
            { mapDeclaration({ { "hp", Schema::Value(10.0f) }, { "mp", Schema::Value(5.0f) } }) });
        ScriptProperty* configProperty = ScriptProperties::Find(merged, "Config");
        CHECK(configProperty != nullptr && configProperty->Children.size() == 2);
        std::get<float>(ScriptProperties::Find(configProperty->Children, "hp")->Value) = 77.0f;
        ScriptProperties::SyncFromDeclarations(merged,
            { mapDeclaration({ { "hp", Schema::Value(0.0f) }, { "sp", Schema::Value(3.0f) } }) });
        configProperty = ScriptProperties::Find(merged, "Config");
        CHECK(configProperty->Children.size() == 2);
        CHECK(ScriptProperties::Find(configProperty->Children, "mp") == nullptr);   // 删掉的键丢弃
        CHECK(std::get<float>(ScriptProperties::Find(configProperty->Children, "hp")->Value) == 77.0f);
        CHECK(std::get<float>(ScriptProperties::Find(configProperty->Children, "sp")->Value) == 3.0f);

        // 元素类型变了(Array(Float) → Array(Int32))→ 回新默认值,不拿旧值按新形状解释。
        {
            ScriptProperties::Declaration changed = arrayDeclaration({ 7.0f, 8.0f });
            changed.ElementKind = Schema::Kind::Int32;
            for (ScriptProperties::Declaration& child : changed.Fields)
            {
                child.Type = Schema::Kind::Int32;
                child.Default = static_cast<int32_t>(std::get<float>(child.Default));
            }
            ScriptProperties::SyncFromDeclarations(merged, { changed });
            CHECK(std::get<int32_t>(merged[0].Children[1].Value) == 8);
        }

        // 元素行读不出来(无 VM 的注解声明)时保留场景里的元素值,不按"空数组"清掉。
        {
            ScriptProperties::Declaration unknown;
            unknown.Name = "Scores";
            unknown.Type = Schema::Kind::Object;
            unknown.Collection = ScriptPropertyCollection::Array;
            unknown.ElementKind = Schema::Kind::Int32;
            unknown.FieldsUnknown = true;
            ScriptProperties::SyncFromDeclarations(merged, { unknown });
            CHECK(merged[0].Children.size() == 2);
            CHECK(std::get<int32_t>(merged[0].Children[1].Value) == 8);
        }

        // ⑥ Luau 读写:属性表 → 脚本表是**数组表**(连续下标)与**键值表**;嵌套集合递归建表。
        {
            Fixture fixture;
            auto entity = fixture.AddLua(logical);
            auto& script = entity.GetComponent<LuauScriptComponent>();
            CHECK(ScriptEngine::SyncScriptDeclarations(script, nullptr, &error));
            CHECK(error.empty());
            ScriptProperty* runtimeScores = ScriptProperties::Find(script.Properties, "Scores");
            CHECK(runtimeScores != nullptr && runtimeScores->Children.size() == 2);
            std::get<float>(runtimeScores->Children[0].Value) = 100.0f;   // 编辑器/场景值
            ScriptProperty* runtimeConfig = ScriptProperties::Find(script.Properties, "Config");
            CHECK(runtimeConfig != nullptr && runtimeConfig->Children.size() == 2);
            std::get<float>(ScriptProperties::Find(runtimeConfig->Children, "hp")->Value) = 42.0f;

            fixture.World->OnScriptStart();
            CHECK(script.Runtime.State == ScriptInstanceState::Running);
            CHECK(script.Runtime.LastError.empty());

            ScriptTableRef scoresTable;
            CHECK(script.ScriptTable.GetField("Scores").AsTable(&scoresTable) && scoresTable.IsValid());
            CHECK(scoresTable.Length() == 2);
            const std::vector<ScriptValue> runtimeElements = scoresTable.GetArray();
            double first = 0.0;
            double second = 0.0;
            CHECK(runtimeElements[0].AsNumber(&first) && first == 100.0);
            CHECK(runtimeElements[1].AsNumber(&second) && second == 13.5);

            ScriptTableRef configTable;
            CHECK(script.ScriptTable.GetField("Config").AsTable(&configTable) && configTable.IsValid());
            double hp = 0.0;
            CHECK(configTable.GetField("hp").AsNumber(&hp) && hp == 42.0);

            ScriptTableRef gridTable;
            CHECK(script.ScriptTable.GetField("Grid").AsTable(&gridTable) && gridTable.IsValid());
            ScriptTableRef innerGrid;
            CHECK(gridTable.GetField("1").IsNil());   // 数组表:不能用字符串 "1" 读
            const std::vector<ScriptValue> gridRows = gridTable.GetArray();
            CHECK(gridRows.size() == 2);
            CHECK(gridRows[0].AsTable(&innerGrid) && innerGrid.Length() == 2);
            double inner = 0.0;
            CHECK(innerGrid.GetArray()[1].AsNumber(&inner) && inner == 2.5);

            // 只读摘要(BadList)不进脚本表覆盖:脚本自己的值保持原样(仍是 { 1, "two" })。
            ScriptTableRef badTable;
            CHECK(script.ScriptTable.GetField("BadList").AsTable(&badTable));
            CHECK(badTable.Length() == 2);
            std::string secondText;
            CHECK(badTable.GetArray()[1].AsString(&secondText) && secondText == "two");
            fixture.Stop();
        }
    }

    // VEC-C1:数组/映射的存档往返(`Type: Array|Map` + ElementType/KeyType/ValueType + Value)。
    void CollectionPropertiesRoundTripThroughSceneSerializer()
    {
        const fs::path scenePath = s_OutputDirectory / "collections_round_trip.wd";
        {
            Ref<Scene> scene = CreateRef<Scene>(TestContext());
            Entity entity = Entity::CreateEntity(scene.get(), "collection save probe");
            LuauScriptComponent& script =
                entity.AddComponent<LuauScriptComponent>("scripts/tests/Collections.lua");

            ScriptProperty scores;
            scores.Name = "Scores";
            scores.Type = Schema::Kind::Object;
            scores.Collection = ScriptPropertyCollection::Array;
            scores.ElementKind = Schema::Kind::Float;
            for (int index = 0; index < 3; ++index)
            {
                ScriptProperty element;
                element.Name = std::to_string(index + 1);
                element.Type = Schema::Kind::Float;
                element.Value = 12.5f + static_cast<float>(index);
                scores.Children.push_back(std::move(element));
            }
            script.Properties.push_back(std::move(scores));

            ScriptProperty config;
            config.Name = "Config";
            config.Type = Schema::Kind::Object;
            config.Collection = ScriptPropertyCollection::Map;
            config.KeyKind = Schema::Kind::String;
            config.ElementKind = Schema::Kind::Float;
            for (const auto& [key, value] : std::vector<std::pair<const char*, float>> {
                    { "hp", 10.5f }, { "mp", 5.5f } })
            {
                ScriptProperty entry;
                entry.Name = key;
                entry.Type = Schema::Kind::Float;
                entry.Value = value;
                config.Children.push_back(std::move(entry));
            }
            script.Properties.push_back(std::move(config));

            ScriptProperty grid;
            grid.Name = "Grid";
            grid.Type = Schema::Kind::Object;
            grid.Collection = ScriptPropertyCollection::Array;
            grid.ElementKind = Schema::Kind::Object;   // 元素本身是数组 → 子条目 seq
            for (int row = 0; row < 2; ++row)
            {
                ScriptProperty inner;
                inner.Name = std::to_string(row + 1);
                inner.Type = Schema::Kind::Object;
                inner.Collection = ScriptPropertyCollection::Array;
                inner.ElementKind = Schema::Kind::Float;
                for (int column = 0; column < 2; ++column)
                {
                    ScriptProperty element;
                    element.Name = std::to_string(column + 1);
                    element.Type = Schema::Kind::Float;
                    element.Value = static_cast<float>(row * 2 + column) + 0.5f;
                    inner.Children.push_back(std::move(element));
                }
                grid.Children.push_back(std::move(inner));
            }
            script.Properties.push_back(std::move(grid));

            ScriptProperty extra;
            extra.Name = "Extra";
            extra.Type = Schema::Kind::Object;
            extra.Collection = ScriptPropertyCollection::Struct;
            extra.TypeName = "table";
            extra.ReadOnly = true;   // 只读摘要:看得到、不进存档
            script.Properties.push_back(std::move(extra));

            SceneSerializer writer(scene);
            CHECK(writer.Serialize(scenePath.string()));
        }

        const std::string yaml = ReadFile(scenePath);
        const std::size_t scoresAt = yaml.find("Name: Scores");
        const std::size_t configAt = yaml.find("Name: Config", scoresAt);
        const std::size_t gridAt = yaml.find("Name: Grid", configAt);
        const std::size_t extraAt = yaml.find("Name: Extra", gridAt);
        CHECK(scoresAt != std::string::npos && configAt != std::string::npos);
        CHECK(gridAt != std::string::npos && extraAt != std::string::npos);

        // 数组:`Type: Array` + `ElementType: Float` + `Value: [12.5, 13.5, …]`(纯值 seq)。
        const std::string scoresBlock = yaml.substr(scoresAt, configAt - scoresAt);
        CHECK(scoresBlock.find("Type: Array") != std::string::npos);
        CHECK(scoresBlock.find("ElementType: Float") != std::string::npos);
        CHECK(scoresBlock.find("Value: [") != std::string::npos);
        CHECK(scoresBlock.find("12.5") != std::string::npos);
        CHECK(scoresBlock.find("14.5") != std::string::npos);
        CHECK(scoresBlock.find("Name:", 1) == std::string::npos);   // 不是子条目 seq

        // 映射:`Type: Map` + `KeyType: String` + `ValueType: Float` + `Value: { hp: …, mp: … }`。
        const std::string configBlock = yaml.substr(configAt, gridAt - configAt);
        CHECK(configBlock.find("Type: Map") != std::string::npos);
        CHECK(configBlock.find("KeyType: String") != std::string::npos);
        CHECK(configBlock.find("ValueType: Float") != std::string::npos);
        CHECK(configBlock.find("Value: {") != std::string::npos);
        CHECK(configBlock.find("hp") != std::string::npos);
        CHECK(configBlock.find("Name:", 1) == std::string::npos);   // 不是子条目 seq

        // 嵌套集合(`{{number}}`):`ElementType: Object` + 子条目 seq(每项自带 Name/Type)。
        const std::string gridBlock = yaml.substr(gridAt, extraAt - gridAt);
        CHECK(gridBlock.find("Type: Array") != std::string::npos);
        CHECK(gridBlock.find("ElementType: Object") != std::string::npos);
        CHECK(gridBlock.find("- Name:") != std::string::npos);

        // 只读摘要整条不写 Value(与"看得到、不进存档"同口径)。
        const std::size_t componentEnd = yaml.find("\n    World::", extraAt);
        const std::string extraBlock = yaml.substr(extraAt,
            componentEnd == std::string::npos ? std::string::npos : componentEnd - extraAt);
        CHECK(extraBlock.find("Type: Object") != std::string::npos);
        CHECK(extraBlock.find("Value:") == std::string::npos);

        // 读回:结构与值都还在(元素/键按名字对齐)。
        Ref<Scene> loaded = CreateRef<Scene>(TestContext());
        SceneSerializer reader(loaded);
        CHECK(reader.Deserialize(scenePath.string()));
        CHECK(reader.GetLastError().empty());
        LuauScriptComponent* loadedScript = nullptr;
        for (const entt::entity handle : loaded->GetRegistry().view<LuauScriptComponent>())
            loadedScript = &loaded->GetRegistry().get<LuauScriptComponent>(handle);
        CHECK(loadedScript != nullptr);

        const ScriptProperty* loadedScores = ScriptProperties::Find(loadedScript->Properties, "Scores");
        CHECK(loadedScores != nullptr);
        CHECK(loadedScores->Collection == ScriptPropertyCollection::Array);
        CHECK(loadedScores->ElementKind == Schema::Kind::Float);
        CHECK(!loadedScores->ReadOnly && loadedScores->Children.size() == 3);
        CHECK(loadedScores->Children[0].Name == "1");
        CHECK(std::get<float>(loadedScores->Children[2].Value) == 14.5f);

        const ScriptProperty* loadedConfig = ScriptProperties::Find(loadedScript->Properties, "Config");
        CHECK(loadedConfig != nullptr);
        CHECK(loadedConfig->Collection == ScriptPropertyCollection::Map);
        CHECK(loadedConfig->KeyKind == Schema::Kind::String);
        CHECK(loadedConfig->ElementKind == Schema::Kind::Float);
        CHECK(loadedConfig->Children.size() == 2);
        CHECK(std::get<float>(ScriptProperties::Find(loadedConfig->Children, "hp")->Value) == 10.5f);

        const ScriptProperty* loadedGrid = ScriptProperties::Find(loadedScript->Properties, "Grid");
        CHECK(loadedGrid != nullptr);
        CHECK(loadedGrid->Collection == ScriptPropertyCollection::Array);
        CHECK(loadedGrid->ElementKind == Schema::Kind::Object);
        CHECK(loadedGrid->Children.size() == 2);
        CHECK(loadedGrid->Children[1].Collection == ScriptPropertyCollection::Array);
        CHECK(loadedGrid->Children[1].ElementKind == Schema::Kind::Float);
        CHECK(std::get<float>(loadedGrid->Children[1].Children[0].Value) == 2.5f);

        const ScriptProperty* loadedExtra = ScriptProperties::Find(loadedScript->Properties, "Extra");
        CHECK(loadedExtra != nullptr);
        CHECK(loadedExtra->ReadOnly && loadedExtra->Children.empty());
    }

    // VEC-D1(用户口径:复位 = 回到"未设"):声明默认值只用于展示/Play 兜底 ——
    //   * 没编辑过 / 复位过的字段不进场景(整条不写;改脚本默认值后老场景跟着变);
    //   * 编辑过的字段才落盘,且不被新默认值覆盖;
    //   * 结构化表的子字段同一条规则(只写被记录的子行)。
    void ScriptDefaultsStayOutOfTheScene()
    {
        const fs::path scenePath = s_OutputDirectory / "d1_unset_round_trip.wd";

        const auto declarations = [](float speed, float damage)
        {
            ScriptProperties::Declaration speedField;
            speedField.Name = "Speed";
            speedField.Type = Schema::Kind::Float;
            speedField.Default = Schema::Value(speed);

            ScriptProperties::Declaration statsField;
            statsField.Name = "Stats";
            statsField.Type = Schema::Kind::Object;
            statsField.Collection = ScriptPropertyCollection::Struct;
            statsField.TypeName = "ProbeStats";
            ScriptProperties::Declaration damageField;
            damageField.Name = "Damage";
            damageField.Type = Schema::Kind::Float;
            damageField.Default = Schema::Value(damage);
            statsField.Fields.push_back(std::move(damageField));

            std::vector<ScriptProperties::Declaration> list;
            list.push_back(std::move(speedField));
            list.push_back(std::move(statsField));
            return list;
        };
        const auto writeScene = [](const fs::path& path, const std::vector<ScriptProperty>& properties)
        {
            Ref<Scene> scene = CreateRef<Scene>(TestContext());
            Entity entity = Entity::CreateEntity(scene.get(), "unset semantics probe");
            LuauScriptComponent& script =
                entity.AddComponent<LuauScriptComponent>("scripts/tests/UnsetProbe.lua");
            script.Properties = properties;
            SceneSerializer writer(scene);
            CHECK(writer.Serialize(path.string()));
        };

        // ① 新字段:Value = 声明默认值(面板不显示 0),但"场景没记录过"。
        std::vector<ScriptProperty> properties;
        ScriptProperties::SyncFromDeclarations(properties, declarations(5.0f, 12.0f));
        CHECK(properties.size() == 2);
        const ScriptProperty* speed = ScriptProperties::Find(properties, "Speed");
        CHECK(speed != nullptr && std::get<float>(speed->Value) == 5.0f);
        CHECK(ScriptProperties::IsDefaultValue(*speed));
        CHECK(!ScriptProperties::IsSceneRecorded(*speed));
        const ScriptProperty* stats = ScriptProperties::Find(properties, "Stats");
        CHECK(stats != nullptr && stats->Children.size() == 1);
        CHECK(std::get<float>(stats->Children[0].Value) == 12.0f);
        CHECK(!ScriptProperties::IsSceneRecorded(*stats));

        // ② 存场景:两条都不写,Properties 段整段消失。
        writeScene(scenePath, properties);
        {
            const std::string yaml = ReadFile(scenePath);
            CHECK(yaml.find("Name: Speed") == std::string::npos);
            CHECK(yaml.find("Name: Stats") == std::string::npos);
            CHECK(yaml.find("Properties:") == std::string::npos);
        }

        // ③ 编辑 Speed(7.0)+ 结构化表子字段(20.0)→ 只有这两行落盘。
        std::get<float>(ScriptProperties::Find(properties, "Speed")->Value) = 7.0f;
        std::get<float>(ScriptProperties::Find(ScriptProperties::Find(properties, "Stats")->Children,
            "Damage")->Value) = 20.0f;
        ScriptProperties::SyncFromDeclarations(properties, declarations(5.0f, 12.0f));
        CHECK(std::get<float>(ScriptProperties::Find(properties, "Speed")->Value) == 7.0f);
        CHECK(ScriptProperties::IsSceneRecorded(*ScriptProperties::Find(properties, "Speed")));
        writeScene(scenePath, properties);
        {
            const std::string yaml = ReadFile(scenePath);
            const std::size_t speedAt = yaml.find("Name: Speed");
            CHECK(speedAt != std::string::npos);
            CHECK(yaml.find("7", speedAt) != std::string::npos);
            const std::size_t statsAt = yaml.find("Name: Stats");
            CHECK(statsAt != std::string::npos);
            CHECK(yaml.find("Name: Damage", statsAt) != std::string::npos);
            CHECK(yaml.find("20", statsAt) != std::string::npos);
        }

        // ④ 复位(值清成 monostate;面板 `↺` 走同一条)+ 再同步 → 回到"未设":显示脚本默认值、场景不写。
        ScriptProperties::Find(properties, "Speed")->Value = Schema::Value {};
        ScriptProperties::SyncFromDeclarations(properties, declarations(5.0f, 12.0f));
        const ScriptProperty* resetSpeed = ScriptProperties::Find(properties, "Speed");
        CHECK(std::get<float>(resetSpeed->Value) == 5.0f);   // 展示值 = 脚本默认值
        CHECK(!ScriptProperties::IsSceneRecorded(*resetSpeed));
        writeScene(scenePath, properties);
        {
            const std::string yaml = ReadFile(scenePath);
            CHECK(yaml.find("Name: Speed") == std::string::npos);
            CHECK(yaml.find("Name: Stats") != std::string::npos);   // Damage 改过 → 结构体仍在
        }

        // ⑤ 改脚本默认值(5 → 9):没记过的字段跟着走;记过的字段保持场景值。
        ScriptProperties::SyncFromDeclarations(properties, declarations(9.0f, 12.0f));
        CHECK(std::get<float>(ScriptProperties::Find(properties, "Speed")->Value) == 9.0f);
        CHECK(!ScriptProperties::IsSceneRecorded(*ScriptProperties::Find(properties, "Speed")));
        std::get<float>(ScriptProperties::Find(properties, "Speed")->Value) = 7.0f;
        ScriptProperties::SyncFromDeclarations(properties, declarations(9.0f, 12.0f));
        CHECK(std::get<float>(ScriptProperties::Find(properties, "Speed")->Value) == 7.0f);
        CHECK(ScriptProperties::IsSceneRecorded(*ScriptProperties::Find(properties, "Speed")));

        // ⑥ 值相等判定:variant 同支才算相等(类型不符的坏存档不会误判成"未设")。
        CHECK(ScriptProperties::ValuesEqual(Schema::Value(3.5f), Schema::Value(3.5f)));
        CHECK(!ScriptProperties::ValuesEqual(Schema::Value(3.5f), Schema::Value(3.25f)));
        CHECK(!ScriptProperties::ValuesEqual(Schema::Value(3.5f), Schema::Value(3.5)));
        CHECK(ScriptProperties::ValuesEqual(Schema::Value(glm::vec3(1.0f, 2.0f, 3.0f)),
            Schema::Value(glm::vec3(1.0f, 2.0f, 3.0f))));
        CHECK(!ScriptProperties::ValuesEqual(Schema::Value(glm::vec3(1.0f, 2.0f, 3.0f)),
            Schema::Value(glm::vec3(1.0f, 2.0f, 3.5f))));
    }

    // VEC-D2(用户口径:集合形状跨进程以场景为准):场景里记下的元素个数/键名在重开 + 声明同步后
    // 必须保留(声明只给默认形状与行默认值);未设的行写 `~` → 脚本改默认值后这些行照样跟着走。
    void SceneOwnedCollectionShapesSurviveRestart()
    {
        const fs::path firstPath = s_OutputDirectory / "d2_shape_first.wd";
        const fs::path secondPath = s_OutputDirectory / "d2_shape_second.wd";

        // ① 场景:4 个元素的数组(第 4 个元素是场景自己的值)+ 3 个键的映射(第 3 个键 = "编辑器新增")。
        {
            Ref<Scene> scene = CreateRef<Scene>(TestContext());
            Entity entity = Entity::CreateEntity(scene.get(), "scene shape probe");
            LuauScriptComponent& script =
                entity.AddComponent<LuauScriptComponent>("scripts/tests/ShapeProbe.lua");

            ScriptProperty scores;
            scores.Name = "Scores";
            scores.Type = Schema::Kind::Object;
            scores.Collection = ScriptPropertyCollection::Array;
            scores.ElementKind = Schema::Kind::Float;
            const float values[] = { 1.5f, 2.5f, 3.5f, 9.5f };
            for (int index = 0; index < 4; ++index)
            {
                ScriptProperty element;
                element.Name = std::to_string(index + 1);
                element.Type = Schema::Kind::Float;
                element.Value = values[index];
                scores.Children.push_back(std::move(element));
            }
            script.Properties.push_back(std::move(scores));

            ScriptProperty config;
            config.Name = "Config";
            config.Type = Schema::Kind::Object;
            config.Collection = ScriptPropertyCollection::Map;
            config.KeyKind = Schema::Kind::String;
            config.ElementKind = Schema::Kind::Float;
            for (const auto& [key, value] : std::vector<std::pair<const char*, float>> {
                    { "hp", 10.0f }, { "mp", 20.0f }, { "sp", 30.0f } })
            {
                ScriptProperty entry;
                entry.Name = key;
                entry.Type = Schema::Kind::Float;
                entry.Value = value;
                config.Children.push_back(std::move(entry));
            }
            script.Properties.push_back(std::move(config));

            SceneSerializer writer(scene);
            CHECK(writer.Serialize(firstPath.string()));
        }

        // ② 声明只有 3 个元素 / hp+mp(脚本侧默认形状)。
        std::vector<ScriptProperties::Declaration> declared;
        {
            ScriptProperties::Declaration scoresDeclaration;
            scoresDeclaration.Name = "Scores";
            scoresDeclaration.Type = Schema::Kind::Object;
            scoresDeclaration.Collection = ScriptPropertyCollection::Array;
            scoresDeclaration.ElementKind = Schema::Kind::Float;
            for (const float value : { 1.5f, 2.5f, 3.5f })
            {
                ScriptProperties::Declaration element;
                element.Name = std::to_string(scoresDeclaration.Fields.size() + 1);
                element.Type = Schema::Kind::Float;
                element.Default = Schema::Value(value);
                scoresDeclaration.Fields.push_back(std::move(element));
            }
            declared.push_back(std::move(scoresDeclaration));

            ScriptProperties::Declaration configDeclaration;
            configDeclaration.Name = "Config";
            configDeclaration.Type = Schema::Kind::Object;
            configDeclaration.Collection = ScriptPropertyCollection::Map;
            configDeclaration.KeyKind = Schema::Kind::String;
            configDeclaration.ElementKind = Schema::Kind::Float;
            for (const auto& [key, value] : std::vector<std::pair<const char*, float>> {
                    { "hp", 10.0f }, { "mp", 20.0f } })
            {
                ScriptProperties::Declaration entry;
                entry.Name = key;
                entry.Type = Schema::Kind::Float;
                entry.Default = Schema::Value(value);
                configDeclaration.Fields.push_back(std::move(entry));
            }
            declared.push_back(std::move(configDeclaration));
        }

        // ③ 读回 + 同步:场景形状(4 个元素 / 3 个键)必须保留,声明只按行名补默认值。
        Ref<Scene> loaded = CreateRef<Scene>(TestContext());
        {
            SceneSerializer reader(loaded);
            CHECK(reader.Deserialize(firstPath.string()));
            CHECK(reader.GetLastError().empty());
            LuauScriptComponent* script = nullptr;
            for (const entt::entity handle : loaded->GetRegistry().view<LuauScriptComponent>())
                script = &loaded->GetRegistry().get<LuauScriptComponent>(handle);
            CHECK(script != nullptr);
            ScriptProperties::SyncFromDeclarations(script->Properties, declared);

            const ScriptProperty* scores = ScriptProperties::Find(script->Properties, "Scores");
            CHECK(scores != nullptr && scores->Children.size() == 4);
            CHECK(scores->ShapeFromScene);
            CHECK(std::get<float>(scores->Children[0].Value) == 1.5f);   // 未设行:显示声明默认值
            CHECK(!ScriptProperties::IsSceneRecorded(scores->Children[0]));
            CHECK(std::get<float>(scores->Children[3].Value) == 9.5f);   // 场景自己的值
            CHECK(ScriptProperties::IsSceneRecorded(scores->Children[3]));
            CHECK(ScriptProperties::IsSceneRecorded(*scores));

            const ScriptProperty* config = ScriptProperties::Find(script->Properties, "Config");
            CHECK(config != nullptr && config->Children.size() == 3);
            CHECK(config->ShapeFromScene);
            CHECK(ScriptProperties::Find(config->Children, "sp") != nullptr);   // 新增键保留
            CHECK(!ScriptProperties::IsSceneRecorded(*ScriptProperties::Find(config->Children, "hp")));
            CHECK(std::get<float>(ScriptProperties::Find(config->Children, "sp")->Value) == 30.0f);
        }

        // ④ 再存盘:形状仍在(4 个元素:3 个未设写 `~` + 1 个场景值),再读回仍是 4 个元素 / 3 个键。
        {
            SceneSerializer writer(loaded);
            CHECK(writer.Serialize(secondPath.string()));
            const std::string yaml = ReadFile(secondPath);
            const std::size_t scoresAt = yaml.find("Name: Scores");
            const std::size_t configAt = yaml.find("Name: Config");
            CHECK(scoresAt != std::string::npos && configAt != std::string::npos);
            const std::size_t valueAt = yaml.find("Value: [", scoresAt);
            CHECK(valueAt != std::string::npos && valueAt < configAt);
            const std::size_t valueEnd = yaml.find(']', valueAt);
            CHECK(valueEnd != std::string::npos);
            const std::string scoresValue = yaml.substr(valueAt, valueEnd - valueAt);
            CHECK(std::count(scoresValue.begin(), scoresValue.end(), ',') == 3);   // 4 个元素
            CHECK(std::count(scoresValue.begin(), scoresValue.end(), '~') == 3);   // 3 行回到"未设"
            CHECK(scoresValue.find("9.5") != std::string::npos);                   // 场景自己的值还在

            Ref<Scene> second = CreateRef<Scene>(TestContext());
            SceneSerializer reader(second);
            CHECK(reader.Deserialize(secondPath.string()));
            LuauScriptComponent* script = nullptr;
            for (const entt::entity handle : second->GetRegistry().view<LuauScriptComponent>())
                script = &second->GetRegistry().get<LuauScriptComponent>(handle);
            CHECK(script != nullptr);
            const ScriptProperty* scores = ScriptProperties::Find(script->Properties, "Scores");
            CHECK(scores != nullptr && scores->Children.size() == 4);
            const ScriptProperty* config = ScriptProperties::Find(script->Properties, "Config");
            CHECK(config != nullptr && config->Children.size() == 3);
        }
    }

    void StubGenerationContracts()
    {
        auto types = LuaReflectionRegistry::GetTable();
        std::string initial, shuffled, error;
        CHECK(LuaStubGenerator::Render(types, initial, error));
        CHECK(initial.find("---@meta") == 0);
        CHECK(initial.find("function Entity:GetID(") != std::string::npos);
        CHECK(initial.find("userdata|nil") != std::string::npos);
        CHECK(initial.find("WorldScript =") == std::string::npos);
        CHECK(initial.find("---@class TransformComponent") == std::string::npos);
        std::reverse(types.begin(), types.end());
        for (auto& type : types)
        {
            std::reverse(type.Properties.begin(), type.Properties.end());
            std::reverse(type.Methods.begin(), type.Methods.end());
            std::reverse(type.Constructors.begin(), type.Constructors.end());
            std::reverse(type.Operators.begin(), type.Operators.end());
        }
        CHECK(LuaStubGenerator::Render(types, shuffled, error));
        CHECK(initial == shuffled);
        const auto destination = s_OutputDirectory / "WorldEngineAPI.lua";
        CHECK(LuaStubGenerator::Generate(destination, types, error));
        const auto timestamp = fs::last_write_time(destination);
        CHECK(LuaStubGenerator::Generate(destination, types, error));
        CHECK(fs::last_write_time(destination) == timestamp && ReadFile(destination) == initial);

        auto invalid = types;
        invalid.push_back(invalid.front());
        std::string output = "preserve on invalid metadata";
        CHECK(!LuaStubGenerator::Render(invalid, output, error));
        CHECK(output == "preserve on invalid metadata" && !error.empty());
        CHECK(!LuaStubGenerator::Generate(destination, invalid, error));
        CHECK(ReadFile(destination) == initial && fs::last_write_time(destination) == timestamp);
        invalid = types;
        auto methodType = std::find_if(invalid.begin(), invalid.end(), [](const auto& type) { return !type.Methods.empty(); });
        CHECK(methodType != invalid.end());
        methodType->Methods.front().Parameters.push_back({ "", "number", "Missing name" });
        CHECK(!LuaStubGenerator::Render(invalid, output, error) && !error.empty());
        invalid = types;
        invalid.front().Properties.push_back({ "Injected", "TypeThatIsNotBound", "" });
        CHECK(!LuaStubGenerator::Render(invalid, output, error) && !error.empty());

        types.front().Properties.push_back({ "TestMetadataChange", "number", "Changed description" });
#ifdef _WIN32
        struct ReadLock
        {
            HANDLE File;
            ~ReadLock() { if (File != INVALID_HANDLE_VALUE) CloseHandle(File); }
        };
        {
            // Deny delete-sharing to force the final atomic replacement to fail,
            // after the temporary file has successfully been created and written.
            ReadLock locked { CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            CHECK(locked.File != INVALID_HANDLE_VALUE);
            CHECK(!LuaStubGenerator::Generate(destination, types, error));
            CHECK(!error.empty() && error.find(destination.u8string()) != std::string::npos);
            CHECK(ReadFile(destination) == initial && fs::last_write_time(destination) == timestamp);
        }
#endif
        CHECK(LuaStubGenerator::Generate(destination, types, error));
        CHECK(ReadFile(destination).find("TestMetadataChange") != std::string::npos);
        for (const auto& entry : fs::directory_iterator(s_OutputDirectory))
            CHECK(entry.path().filename().u8string().find("WorldEngineAPI.lua.tmp.") != 0);
    }

    void RealBindingsAndTemplate()
    {
        Fixture fixture;
        auto entity = fixture.AddLua("scripts/templates/WorldScript.lua");
        ScriptEngine::GetState().SetGlobal("T02Entity", MakeEntityValue(entity));
        RunLua(R"lua(
            assert(WorldScript == nil)
            assert(T02Entity:IsValid() and type(T02Entity:GetID()) == "number")
            assert(T02Entity:HasComponent("TagComponent"))
            assert(T02Entity:GetComponent("TransformComponent") == nil)
            local opaque = T02Entity:GetComponent("TagComponent")
            assert(type(opaque) == "userdata")
            local ok = pcall(function() return T02Entity:HasComponent("vec3") end)
            assert(not ok)
            local v = vec3.new(3.0, 4.0, 0.0)
            assert(v:length() == 5.0 and v:dot(v) == 25.0)
            local m3, m4 = mat3.new(1.0), mat4.new(1.0)
            assert(m3:determinant() == 1.0 and m4:determinant() == 1.0)
            assert((m3 * v).x == 3.0 and (m4 * vec4.new(1.0, 2.0, 3.0, 1.0)).y == 2.0)
            assert(m3[0].x == 1.0 and m4[3].w == 1.0)
            assert(not pcall(function() return m3[-1] end))
            assert(not pcall(function() return m3[3] end))
            assert(not pcall(function() m4[4] = vec4.new(0.0) end))
            assert(not pcall(function() m3[-1] = vec3.new(0.0) end))
            print(42, true, false, nil, { test = "tostring semantics" })
        )lua");
        fixture.World->OnScriptStart();
        CHECK(entity.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
        fixture.Step();
        CHECK(entity.GetComponent<LuauScriptComponent>().Runtime.State == ScriptInstanceState::Running);
        fixture.Stop();
        CheckLuaReleased(entity);
        CHECK(entity.GetComponent<LuauScriptComponent>().Runtime.LastError.empty());
    }

    void VmRestartKeepsUniqueMetadata()
    {
        const auto count = LuaReflectionRegistry::GetTable().size();
        auto* state = &ScriptEngine::GetState();
        ScriptEngine::Init();
        CHECK(&ScriptEngine::GetState() == state);
        CHECK(LuaReflectionRegistry::GetTable().size() == count);
        ScriptEngine::Shutdown();
        ScriptEngine::Shutdown();
        CHECK(!ScriptEngine::IsInitialized());
        ScriptEngine::Init();
        CHECK(LuaReflectionRegistry::GetTable().size() == count);
        RunLua("assert(mat3.new(1.0):determinant() == 1.0 and mat4.new(1.0):determinant() == 1.0 and Entity ~= nil)");
    }

    std::string FirstLine(const std::string& text)
    {
        const std::size_t at = text.find('\n');
        return at == std::string::npos ? text : text.substr(0, at);
    }

    // W6:沙箱预算(指令口径)。
    // 1) BudgetSpin.lua 的 OnUpdate 死循环被预算中断:只有该实例 Faulted,同场景好脚本
    //    继续更新、场景仍 active;诊断带路径 + phase=OnUpdate + stack traceback。
    //    测试用 RAII 恢复默认策略(同进程共享 VM,后续组必须看到默认值)。
    // 2) 默认预算(1e6 指令)余量标定:32 个真实夹具脚本一帧的总命中数。
    // SCRIPT-V7 P2-①:销毁时工厂已经消失(脚本模块被卸载 / schema 注销)不能静默丢指针 ——
    // 走"警告 + 只置空一次"的路径,不崩、不重复释放(实例此时无法安全释放)。
    // 本用例会注销 Test 模块的脚本 schema,所以必须排在测试列表**最后**(注册不再恢复)。
    void UnregisteredFactoryIsReportedInsteadOfSilentlyLeaked()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        fixture.World->OnScriptStart();
        auto& script = entity.GetComponent<CppScriptComponent>();
        CHECK(script.Instance != nullptr);
        CHECK(script.Runtime.State == ScriptInstanceState::Running);

        // 拔掉工厂:与"Game.dll 没加载 / 模块被卸载"同一个失败面。
        TestContext().Schemas().UnregisterModule(Schema::ModuleId{ "Test", 1 });

        fixture.Stop();   // OnDestroy 仍走实例(引用还在),Destroy 工厂取不到 → 警告,不重复释放
        CHECK(script.Instance == nullptr);
        CHECK(script.Runtime.State != ScriptInstanceState::Running);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Destroys == 1);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Deletes == 0);   // 工厂没了:实例无法释放(泄漏),但不会被二次使用
    }

    // SCRIPT-V11:脚本 `print` 必须真的落进引擎日志(默认 INFO 级、`[script] ` 前缀)。
    // 回归点:ScriptEngine::Init 曾用 trace 级的 `[Lua] …` 实现覆盖 LuauVm 装好的 print,
    // 默认 log_level=info 下用户在编辑器里完全看不到脚本输出(RESUME 遗留项 ③)。
    void ScriptPrintReachesEngineLog()
    {
        const std::string token = "SCRIPT-V11-PRINT-TOKEN-" + std::to_string(GetCurrentProcessId());
        RunLua("print('" + token + "')");

        const std::vector<std::string> lines = World::Log::RecentLines(200);
        bool found = false;
        for (const std::string& line : lines)
        {
            if (line.find("[script]") != std::string::npos && line.find(token) != std::string::npos)
                found = true;
        }
        CHECK(found);
    }

    void SandboxBudgetIsolationAndMargin()
    {
        const Sandbox::Policy defaultPolicy = ScriptEngine::GetSandboxPolicy();
        CHECK(defaultPolicy.Instructions == 1000000);
        CHECK(defaultPolicy.TimeMs == 0);

        {
            struct PolicyGuard
            {
                Sandbox::Policy Saved;
                explicit PolicyGuard(const Sandbox::Policy& saved) : Saved(saved) {}
                ~PolicyGuard() { ScriptEngine::SetSandboxPolicy(Saved); }
            } guard(defaultPolicy);

            // 收紧到 1e5:死循环在毫秒级被拦下,同时远高于夹具正常调用的用量。
            ScriptEngine::SetSandboxPolicy(Sandbox::Policy{ 100000, 0 });
            CHECK(ScriptEngine::GetSandboxPolicy().Instructions == 100000);

            Fixture fixture;
            auto spin = fixture.AddLua("scripts/tests/BudgetSpin.lua");
            auto good = fixture.AddLua();
            fixture.World->OnScriptStart();
            fixture.Step();

            auto& script = spin.GetComponent<LuauScriptComponent>();
            CHECK(script.Runtime.State == ScriptInstanceState::Faulted);
            CHECK(script.Runtime.LastError.find("script budget exceeded") != std::string::npos);
            CHECK(script.Runtime.LastError.find("scripts/tests/BudgetSpin.lua") != std::string::npos);
            CHECK(script.Runtime.LastError.find("instructions") != std::string::npos);
            CHECK(script.Runtime.LastError.find("phase=OnUpdate") != std::string::npos);
            CHECK(script.Runtime.LastError.find("stack traceback") != std::string::npos);
            CheckLuaReleased(spin);
            std::cout << "[W6] BudgetSpin fault: " << FirstLine(script.Runtime.LastError) << '\n';

            fixture.Step();
            CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(good)).Updates == 2);
            CHECK(fixture.World->IsActive());
            const std::string error = script.Runtime.LastError;
            fixture.Step();
            CHECK(script.Runtime.LastError == error);
            fixture.Stop();
        }
        CHECK(ScriptEngine::GetSandboxPolicy().Instructions == defaultPolicy.Instructions);

        // 标定:外层 Scope 只放大上限用于计数(命中数与上限无关),测 32 个 LifecycleProbe
        // 的 OnCreate + OnUpdate 一帧总命中数;总量 < 默认预算 → 每个受保护调用都远低于预算。
        {
            Fixture fixture;
            for (int index = 0; index < 32; ++index)
                fixture.AddLua();
            Sandbox::Scope measurement(ScriptEngine::GetState().State(), Sandbox::Policy{ 100000000, 0 });
            fixture.World->OnScriptStart();
            fixture.Step();
            const std::uint64_t hits = measurement.Result().Used;
            CHECK(!measurement.Exceeded());
            CHECK(hits > 0);
            CHECK(hits < defaultPolicy.Instructions);
            std::cout << "[W6] calibration: 32 fixture scripts OnCreate+OnUpdate used " << hits
                << " hits (default budget " << defaultPolicy.Instructions << ")\n";
            fixture.Stop();
        }
    }
}

namespace
{
    // CPPT-2:Enum/Asset/只读摘要探针 —— 与生成物里的真实脚本字段走同一条 schema 通道。
    const Schema::EnumSchema* ProbeModeEnum()
    {
        static const Schema::EnumSchema schema = {
            "ProbeMode", true, 4, { { "Off", 0 }, { "On", 1 } },
        };
        return &schema;
    }

    // CPPT-2(F-1/D-D):纯模型层验收 —— Enum/Asset 放行、未支持 Kind 只读摘要、
    // "只有显式 Default 才是声明默认值"(不再把类型零值当默认值)。
    void CppSchemaPropertyModel()
    {
        Schema::SchemaRegistry schemas;
        CHECK(schemas.RegisterEnum({ "Test", 1 }, *ProbeModeEnum()) == Schema::SchemaRegistry::Status::Ok);

        static const Schema::EnumSchema* s_Enum = schemas.FindEnum("ProbeMode");
        CHECK(s_Enum != nullptr);
        const auto getEnum = +[]() -> const Schema::EnumSchema* { return s_Enum; };

        Schema::FieldSchema unsetFloat;
        unsetFloat.Name = "Speed";
        unsetFloat.K = Schema::Kind::Float;
        unsetFloat.Default = Schema::Value {};          // 生成器:Category==Script 且无 Default(...) → monostate
        Schema::FieldSchema defaultFloat;
        defaultFloat.Name = "Health";
        defaultFloat.K = Schema::Kind::Float;
        defaultFloat.Default = Schema::Value(100.0f);
        defaultFloat.Meta.Unit = "hp";
        Schema::FieldSchema enumField;
        enumField.Name = "Mode";
        enumField.K = Schema::Kind::Enum;
        enumField.GetEnum = getEnum;
        enumField.Default = Schema::Value {};
        Schema::FieldSchema assetField;
        assetField.Name = "Icon";
        assetField.K = Schema::Kind::Asset;
        assetField.AssetTypeName = "Texture2D";
        assetField.Default = Schema::Value {};
        Schema::FieldSchema cellField;
        cellField.Name = "Cell";
        cellField.K = Schema::Kind::IVec3;              // 面板没有行控件 → 只读摘要

        Schema::TypeSchema type;
        type.Id = Schema::TypeId { "Test::CppSchemaProbe" };
        type.DisplayName = "CppSchemaProbe";
        type.Category = Schema::TypeCategory::Script;
        type.Fields = { unsetFloat, defaultFloat, enumField, assetField, cellField };

        std::vector<ScriptProperty> properties;
        ScriptProperties::SyncFromSchema(properties, type);
        CHECK(properties.size() == 5);
        // 顺序 = 声明顺序;未声明默认值 → 未设(类型零值不再是默认值)。
        CHECK(properties[0].Name == "Speed");
        CHECK(ScriptProperties::IsUnset(properties[0]));
        CHECK(std::holds_alternative<std::monostate>(properties[0].Default));
        CHECK(!ScriptProperties::IsSceneRecorded(properties[0]));
        // 显式默认值:展示值 = 声明值,未改过不进场景;改值后进场景。
        CHECK(properties[1].Name == "Health" && std::get<float>(properties[1].Value) == 100.0f);
        CHECK(!ScriptProperties::IsSceneRecorded(properties[1]));
        properties[1].Value = 42.0f;
        CHECK(ScriptProperties::IsSceneRecorded(properties[1]));
        // Enum:TypeName = 枚举名(读回要用),值走整数;Asset:TypeName = 资产类型名,值走字符串。
        CHECK(properties[2].Type == Schema::Kind::Enum && properties[2].TypeName == "ProbeMode");
        CHECK(ScriptProperties::ValueMatchesKind(Schema::Value(static_cast<int64_t>(1)), Schema::Kind::Enum));
        CHECK(!ScriptProperties::ValueMatchesKind(Schema::Value(std::string("On")), Schema::Kind::Enum));
        CHECK(properties[3].Type == Schema::Kind::Asset && properties[3].TypeName == "Texture2D");
        CHECK(ScriptProperties::ValueMatchesKind(Schema::Value(std::string("textures/Icon.wtex")), Schema::Kind::Asset));
        // 未支持 Kind:只读摘要行 —— 看得到、不进存档、值不匹配任何 kind。
        CHECK(properties[4].Type == Schema::Kind::IVec3 && properties[4].ReadOnly);
        CHECK(!ScriptProperties::IsSceneRecorded(properties[4]));
        CHECK(!ScriptProperties::ValueMatchesKind(Schema::Value(glm::ivec3(1)), Schema::Kind::IVec3));
        // 再次同步:同名同类型保值(场景/编辑器值优先),容器形状不变。
        properties[0].Value = 3.5f;
        properties[2].Value = Schema::Value(static_cast<int64_t>(1));
        ScriptProperties::SyncFromSchema(properties, type);
        CHECK(properties.size() == 5);
        CHECK(std::get<float>(properties[0].Value) == 3.5f);
        CHECK(std::get<float>(properties[1].Value) == 42.0f);
        CHECK(std::get<int64_t>(properties[2].Value) == 1);
        // 声明里删掉的字段丢弃(脚本即事实源)。
        type.Fields = { unsetFloat };
        ScriptProperties::SyncFromSchema(properties, type);
        CHECK(properties.size() == 1 && properties[0].Name == "Speed");
        CHECK(std::get<float>(properties[0].Value) == 3.5f);
    }

    // CPPT-2:Enum/Asset 属性经场景序列化往返 —— 存档写 Type: Enum/Asset + TypeName,
    // 读回用 SchemaRegistry::FindEnum 合成枚举 probe(F-6)。
    void CppSchemaPropertiesRoundTrip()
    {
        const fs::path scenePath = s_OutputDirectory / "cpp_schema_round_trip.wd";
        const auto setProperty = [](std::vector<ScriptProperty>& properties, const char* name, Schema::Value value)
        {
            ScriptProperty* property = ScriptProperties::Find(properties, name);
            CHECK(property != nullptr);
            property->Value = std::move(value);
        };

        {
            Ref<Scene> scene = CreateRef<Scene>(TestContext());
            Entity entity = Entity::CreateEntity(scene.get(), "cpp schema probe");
            entity.AddComponent<CppScriptComponent>().ScriptName = "T02NativeProbe";
            auto& script = entity.GetComponent<CppScriptComponent>();
            const Schema::TypeSchema* type = TestContext().Schemas().Find("T02NativeProbe");
            CHECK(type != nullptr);
            ScriptProperties::SyncFromSchema(script.Properties, *type);
            setProperty(script.Properties, "Value", Schema::Value(19.0f));
            setProperty(script.Properties, "Mode", Schema::Value(static_cast<int64_t>(1)));
            setProperty(script.Properties, "Icon", Schema::Value(std::string("textures/Icon.wtex")));

            SceneSerializer writer(scene);
            CHECK(writer.Serialize(scenePath.string()));
        }

        const std::string yaml = ReadFile(scenePath);
        CHECK(yaml.find("Type: Enum") != std::string::npos);
        CHECK(yaml.find("TypeName: ProbeMode") != std::string::npos);
        CHECK(yaml.find("Type: Asset") != std::string::npos);
        CHECK(yaml.find("TypeName: Texture2D") != std::string::npos);
        CHECK(yaml.find("textures/Icon.wtex") != std::string::npos);

        {
            Ref<Scene> loaded = CreateRef<Scene>(TestContext());
            SceneSerializer reader(loaded);
            CHECK(reader.Deserialize(scenePath.string()));
            CppScriptComponent* reloaded = nullptr;
            for (const entt::entity handle : loaded->GetRegistry().view<CppScriptComponent>())
                reloaded = &loaded->GetRegistry().get<CppScriptComponent>(handle);
            CHECK(reloaded != nullptr);
            const ScriptProperty* value = ScriptProperties::Find(reloaded->Properties, "Value");
            const ScriptProperty* mode = ScriptProperties::Find(reloaded->Properties, "Mode");
            const ScriptProperty* icon = ScriptProperties::Find(reloaded->Properties, "Icon");
            const ScriptProperty* cell = ScriptProperties::Find(reloaded->Properties, "Cell");
            CHECK(value && std::get<float>(value->Value) == 19.0f);
            CHECK(mode && mode->Type == Schema::Kind::Enum && mode->TypeName == "ProbeMode");
            CHECK(mode && std::get<int64_t>(mode->Value) == 1);
            CHECK(icon && icon->Type == Schema::Kind::Asset && icon->TypeName == "Texture2D");
            CHECK(icon && std::get<std::string>(icon->Value) == "textures/Icon.wtex");
            CHECK(cell && cell->Type == Schema::Kind::IVec3 && cell->ReadOnly);
        }
    }

    // CPPT-2(T5b):卸载前的实例收容(OnDestroy 各一次、配置态原样)→ 加载后的配置态迁移 + Pending 重跑。
    void ModuleReloadDrainAndRestore()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        auto& script = entity.GetComponent<CppScriptComponent>();
        const Schema::TypeSchema* type = TestContext().Schemas().Find("T02NativeProbe");
        CHECK(type != nullptr);
        ScriptProperties::SyncFromSchema(script.Properties, *type);
        ScriptProperty* value = ScriptProperties::Find(script.Properties, "Value");
        CHECK(value != nullptr);
        value->Value = 19.0f;

        fixture.World->OnScriptStart();
        CHECK(script.Runtime.State == ScriptInstanceState::Running && script.Instance != nullptr);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Creates == 1);
        const uint64_t firstGeneration = script.Runtime.Generation;
        CHECK(fixture.World->CanApplyScriptReload());

        CHECK(fixture.World->DrainNativeScriptInstances() == 1);
        CHECK(script.Instance == nullptr);
        CHECK(script.Runtime.State == ScriptInstanceState::Stopped);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Destroys == 1);
        // 配置态(ScriptName + Properties)是热重载迁移的输入:原样保留。
        CHECK(script.ScriptName == "T02NativeProbe");
        CHECK(std::get<float>(ScriptProperties::Find(script.Properties, "Value")->Value) == 19.0f);

        CHECK(fixture.World->RestoreNativeScriptInstances() == 1);
        CHECK(script.Runtime.State == ScriptInstanceState::Pending);
        CHECK(script.Instance == nullptr);
        CHECK(std::get<float>(ScriptProperties::Find(script.Properties, "Value")->Value) == 19.0f);

        // 下一安全点:既有 pending 机制起新实例(Generation 换代),Play 套用配置态。
        fixture.Step();
        CHECK(script.Runtime.State == ScriptInstanceState::Running && script.Instance != nullptr);
        CHECK(script.Runtime.Generation != firstGeneration);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Creates == 2);
        CHECK(static_cast<NativeProbe*>(script.Instance)->Value == 19.0f);

        fixture.Stop();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Destroys == 2);
    }

    // CPPT-2(T5b):重载编排的安全点门 —— 脚本回调里拒绝(NotSafePoint),回调外的帧边界报真实状态
    // (本测试进程没有 Game 模块 → NotFound);两种情况都不假装卸载/加载过。
    void ModuleReloadOrchestrationGuards()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        fixture.World->OnScriptStart();

        Modules::GameModuleReloadResult result;
        bool rejectedInCallback = false;
        fixture.Context.NativeAction = [&](Entity, const std::string& phase)
        {
            if (phase != "update")
                return;
            rejectedInCallback = !Modules::GameModuleReload::Unload(TestContext(), fixture.World.get(), &result)
                && result.Status == Modules::ModuleManager::Status::NotSafePoint
                && !result.ModuleUnloaded;
        };
        fixture.Step();
        CHECK(rejectedInCallback);
        CHECK(fixture.World->CanApplyScriptReload());

        // 结果变量同时给出一致的"没加载"状态:未加载不是重载失败。
        CHECK(Modules::GameModuleReload::IsUnloaded(TestContext()));
        CHECK(!Modules::GameModuleReload::Unload(TestContext(), fixture.World.get(), &result));
        CHECK(result.Status == Modules::ModuleManager::Status::NotFound);
        CHECK(!result.ModuleUnloaded && !result.RolledBack && result.InstancesDrained == 0);
        fixture.Stop();
    }

    // CPPT-2(T5b)实机取证(不属于常规单测:依赖构建产物 Game.dll):
    //   `WorldScriptTests.exe --module-probe <Game.dll>` 走一遍真实模块的 加载 → 卸载 → 再加载,
    //   覆盖:ABI 等值门(旧 DLL 必须 AbiMismatch)、schema/行为注销与重建(F-5 接线)、
    //   实例收容与配置态迁移(同名同类型保值 + Pending)。退出码 0 = 全绿。
    int RunModuleProbe(const char* modulePath)
    {
        WorldContext& context = TestContext();
        std::string error;
        const auto status = context.Modules().Load(modulePath, context, &error);
        std::cout << "[probe] load status=" << Modules::ModuleManager::StatusName(status)
            << " error=" << error << '\n';
        if (status != Modules::ModuleManager::Status::Ok)
            return 2;
        const Modules::WeModule* module = context.Modules().FindById("game");
        std::cout << "[probe] abi=" << (module ? module->AbiVersion : 0u)
            << " behaviors=" << BehaviorRegistry::Instance().Size() << '\n';
        if (!module || module->AbiVersion != Modules::WE_MODULE_ABI_VERSION)
            return 3;

        Ref<Scene> scene = CreateRef<Scene>(context);
        Entity entity = Entity::CreateEntity(scene.get(), "cpp module probe");
        CppScriptComponent& script = entity.AddComponent<CppScriptComponent>();
        script.ScriptName = "Game::StressTest";
        const Schema::TypeSchema* type = context.Schemas().Find("Game::StressTest");
        if (!type)
        {
            std::cout << "[probe] Game::StressTest is not registered\n";
            return 4;
        }
        ScriptProperties::SyncFromSchema(script.Properties, *type);
        ScriptProperty* weight = ScriptProperties::Find(script.Properties, "Weight");
        ScriptProperty* icon = ScriptProperties::Find(script.Properties, "Icon");
        if (!weight || !icon)
        {
            std::cout << "[probe] expected Weight/Icon property rows\n";
            return 5;
        }
        weight->Value = Schema::Value(static_cast<int32_t>(4));
        icon->Value = Schema::Value(std::string("textures/Icon.wtex"));

        Modules::GameModuleReloadResult unloaded;
        if (!Modules::GameModuleReload::Unload(context, scene.get(), &unloaded))
        {
            std::cout << "[probe] unload failed: " << unloaded.Message << '\n';
            return 6;
        }
        std::cout << "[probe] unload drained=" << unloaded.InstancesDrained
            << " unloaded=" << unloaded.ModuleUnloaded << " msg=" << unloaded.Message << '\n';
        if (context.Schemas().Find("Game::StressTest") || BehaviorRegistry::Instance().Find("Game::StressTest"))
        {
            std::cout << "[probe] schema or behavior survived unload\n";
            return 7;
        }
        if (std::get<int32_t>(ScriptProperties::Find(script.Properties, "Weight")->Value) != 4)
        {
            std::cout << "[probe] configuration lost during unload\n";
            return 8;
        }

        Modules::GameModuleReloadResult loaded;
        if (!Modules::GameModuleReload::Load(context, scene.get(), &loaded))
        {
            std::cout << "[probe] reload failed: " << loaded.Message << '\n';
            return 9;
        }
        std::cout << "[probe] reload abi=" << loaded.AbiVersion
            << " restored=" << loaded.InstancesRestored << '\n';
        const ScriptProperty* weightAfter = ScriptProperties::Find(script.Properties, "Weight");
        const ScriptProperty* iconAfter = ScriptProperties::Find(script.Properties, "Icon");
        if (!weightAfter || std::get<int32_t>(weightAfter->Value) != 4)
            return 10;
        if (!iconAfter || std::get<std::string>(iconAfter->Value) != "textures/Icon.wtex")
            return 11;
        if (loaded.AbiVersion != Modules::WE_MODULE_ABI_VERSION
            || loaded.InstancesRestored != 1
            || script.Runtime.State != ScriptInstanceState::Pending
            || !BehaviorRegistry::Instance().Find("Game::StressTest"))
            return 12;

        // 收尾:可重复卸载,退出时不留 Game 模块。
        Modules::GameModuleReloadResult cleanup;
        if (!Modules::GameModuleReload::Unload(context, scene.get(), &cleanup))
            return 13;
        std::cout << "[probe] ok\n";
        return 0;
    }
}

int main(int argc, char** argv)
{
    try
    {
        World::Log::Init();
        World::ScriptEngine::Init();
        if (argc == 2 && std::string(argv[1]) == "--generate-stubs")
        {
            const bool generated = World::ScriptEngine::GenerateLuaStubs();
            World::ScriptEngine::Shutdown();
            return generated ? 0 : 1;
        }
        // CPPT-2(T5b)实机取证入口:加载/卸载/再加载真实 Game.dll(见 RunModuleProbe 注释)。
        if (argc == 3 && std::string(argv[1]) == "--module-probe")
        {
            const int probe = RunModuleProbe(argv[2]);
            World::ScriptEngine::Shutdown();
            return probe;
        }
        if (argc != 1) throw std::runtime_error("Usage: WorldScriptTests [--generate-stubs]");
        s_OutputDirectory = fs::path(WORLD_SCRIPT_TEST_OUTPUT_DIR) / ("run-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(s_OutputDirectory);
        {
            // 2026-09-26 重写:Category==Script 的 schema 绑定 = 工厂(Create/Destroy),
            // 组件里不再有函数指针;工厂这里需要当前 ProbeContext 才能建出 NativeProbe。
            static const Schema::ScriptBinding probeBinding = {
                []() -> ScriptableEntity* {
                    if (!s_ProbeContext) throw std::logic_error("Missing test context");
                    return new NativeProbe(*s_ProbeContext);
                },
                [](ScriptableEntity* instance) { delete instance; },
            };
            static const Schema::FieldSchema probeValue = {
                Schema::FieldId{ Schema::Fnv1a64("T02NativeProbe.Value") },
                "Value",
                Schema::Kind::Float,
                [](const void* instance) { return Schema::Value(static_cast<const NativeProbe*>(instance)->Value); },
                [](void* instance, const Schema::Value& value) { static_cast<NativeProbe*>(instance)->Value = std::get<float>(value); },
                nullptr, nullptr, nullptr, nullptr, nullptr,
                Schema::FieldMetadata{},
                Schema::Value(0.0f),
            };
            // CPPT-2:Enum / Asset / 未支持 Kind(IVec3 只读摘要)三条探针字段。
            static const Schema::FieldSchema probeMode = {
                Schema::FieldId{ Schema::Fnv1a64("T02NativeProbe.Mode") },
                "Mode",
                Schema::Kind::Enum,
                [](const void* instance) { return Schema::Value(static_cast<const NativeProbe*>(instance)->Mode); },
                [](void* instance, const Schema::Value& value) { static_cast<NativeProbe*>(instance)->Mode = std::get<int64_t>(value); },
                nullptr, nullptr, nullptr, &ProbeModeEnum, nullptr,
                Schema::FieldMetadata{},
                Schema::Value {},
            };
            static const Schema::FieldSchema probeIcon = {
                Schema::FieldId{ Schema::Fnv1a64("T02NativeProbe.Icon") },
                "Icon",
                Schema::Kind::Asset,
                [](const void* instance) { return Schema::Value(static_cast<const NativeProbe*>(instance)->Icon); },
                [](void* instance, const Schema::Value& value) { static_cast<NativeProbe*>(instance)->Icon = std::get<std::string>(value); },
                nullptr, nullptr, nullptr, nullptr, "Texture2D",
                Schema::FieldMetadata{},
                Schema::Value {},
            };
            static const Schema::FieldSchema probeCell = {
                Schema::FieldId{ Schema::Fnv1a64("T02NativeProbe.Cell") },
                "Cell",
                Schema::Kind::IVec3,
                [](const void* instance) { return Schema::Value(static_cast<const NativeProbe*>(instance)->Cell); },
                [](void* instance, const Schema::Value& value) { static_cast<NativeProbe*>(instance)->Cell = std::get<glm::ivec3>(value); },
                nullptr, nullptr, nullptr, nullptr, nullptr,
                Schema::FieldMetadata{},
                Schema::Value {},
            };
            static const Schema::TypeSchema probeSchema = {
                Schema::TypeId{ "T02NativeProbe" },
                "T02NativeProbe",
                Schema::WE_SCHEMA_ABI_VERSION,
                sizeof(NativeProbe),
                Schema::TypeCategory::Script,
                { probeValue, probeMode, probeIcon, probeCell },
                nullptr,
                &probeBinding,
            };
            CHECK(TestContext().Schemas().RegisterEnum({ "Test", 1 }, *ProbeModeEnum()) == Schema::SchemaRegistry::Status::Ok);
            CHECK(TestContext().Schemas().Register({ "Test", 1 }, probeSchema) == Schema::SchemaRegistry::Status::Ok);
        }

        const std::pair<const char*, void(*)()> tests[] = {
            { "130 Lua instances, native counts and restart", ManyInstancesAndRestart },
            { "owner-thread guards", OwnerThreadGuards },
            { "self destroy, repeated remove and callback completion", SelfDestroyAndRemove },
            { "delete victim before its update", DestroyBeforeVictimUpdate },
            { "deferred creation, batch boundary and paused flush", DeferredCreationAndPausedFlush },
            { "cancel work from destroyed, removed and faulted sources", CancelCommandsFromEndedSources },
            { "nested commands retain the original script source", NestedCommandsRetainScriptSource },
            { "synchronous whitelist and deferred cleanup", SynchronousWhitelistAndDeferredCleanup },
            { "replace rejection and remove-then-add", ReplacementRequiresCleanup },
            { "stop inside create and pending cancellation", StopFromCallbackAndPendingCancellation },
            { "native errors and exactly-once cleanup", NativeErrorsReleaseExactlyOnce },
            { "Lua load/type/callback errors and isolation", LuaErrorsReleaseExactlyOnce },
            { "inherited Lua lifecycle callbacks and protected lookup", InheritedLuaCallbacksAndLookupErrors },
            { "clone configuration without instances or bodies", CloneOnlyConfiguration },
            { "mismatched or unset property variants are skipped", MismatchedPropertyVariantIsSkipped },
            { "body removal and component dependencies", PhysicsRemovalAndDependencies },
            { "camera reacquisition and expired Entity", CameraReacquisitionAndExpiredHandles },
            { "non-owning LayerStack detach order", NonOwningLayerDetachOrder },
            { "native properties configure the factory instance", NativePropertiesConfigureFactoryInstance },
            { "syntax errors preserve preview cache", SyntaxErrorsAndPreviewCache },
            { "Vec2/Vec3/Vec4 properties from annotations and schema", VectorPropertiesFromAnnotationsAndSchema },
            { "array/map properties from annotations, inference and Luau read/write",
                CollectionPropertiesFromAnnotationsAndInference },
            { "bare table annotations infer struct/array rows", BareTableAnnotationsInferStructure },
            { "array/map properties round-trip through the scene serializer",
                CollectionPropertiesRoundTripThroughSceneSerializer },
            { "declaration defaults stay out of the scene (reset = unset)", ScriptDefaultsStayOutOfTheScene },
            { "scene-owned collection shapes survive restart and declaration sync",
                SceneOwnedCollectionShapesSurviveRestart },
            { "deterministic and atomic stub generation", StubGenerationContracts },
            { "real static-link bindings and template", RealBindingsAndTemplate },
            { "VM restart keeps metadata unique", VmRestartKeepsUniqueMetadata },
            { "script print reaches the engine log at INFO with the [script] prefix", ScriptPrintReachesEngineLog },
            { "sandbox budget isolation and headroom", SandboxBudgetIsolationAndMargin },
            { "C++ schema properties: enum/asset editable and unsupported kinds read-only", CppSchemaPropertyModel },
            { "C++ schema enum/asset properties round-trip through the scene serializer", CppSchemaPropertiesRoundTrip },
            { "module reload drains instances and restores configuration as pending", ModuleReloadDrainAndRestore },
            { "module reload orchestration guards (safe point, unloaded state)", ModuleReloadOrchestrationGuards },
            // 注意:本用例注销 Test 模块的脚本 schema(不再恢复),必须排在最后。
            { "unregistered script factory is reported instead of silently leaked", UnregisteredFactoryIsReportedInsteadOfSilentlyLeaked }
        };
        int failures = 0;
        for (const auto& [name, test] : tests)
        {
            try { test(); std::cout << "[PASS] " << name << '\n'; }
            catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
            catch (...) { ++failures; std::cerr << "[FAIL] " << name << ": unknown exception\n"; }
        }
        World::ScriptEngine::Shutdown();
        std::cout << std::size(tests) - failures << "/" << std::size(tests) << " test groups passed\n";
        return failures ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Test setup failed: " << error.what() << '\n';
        if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
        return 1;
    }
}
