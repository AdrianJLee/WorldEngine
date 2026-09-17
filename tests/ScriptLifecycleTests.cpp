#include "wldpch.h"
#include "World/Core/Memory/PoolAllocator.h"
#include "World/Core/WorldContext.h"
#include "World/Core/LayerStack.h"
#include "World/Scene/Components.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Scene/LuaStubGenerator.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptValue.h"

#include <box2d/box2d.h>
#include <atomic>
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

    void BindProbe(NativeScriptComponent& script)
    {
        script.ScriptName = "T02NativeProbe";
        script.InstantiateScript = []() -> ScriptableEntity* {
            if (!s_ProbeContext) throw std::logic_error("Missing test context");
            return new NativeProbe(*s_ProbeContext);
        };
        script.DestroyScript = [](ScriptableEntity*& instance) { delete instance; instance = nullptr; };
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
            BindProbe(entity.AddComponent<NativeScriptComponent>());
            return entity;
        }
        Entity AddLua(const std::string& path = "scripts/tests/LifecycleProbe.lua")
        {
            auto entity = Entity::CreateEntity(World.get(), "Lua probe");
            entity.AddComponent<LuaScriptComponent>(path);
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
        entity.GetComponent<LuaScriptComponent>().CachedFields[name] = { LuaFieldType::String, value };
    }

    void CheckLuaReleased(Entity entity)
    {
        const auto& script = entity.GetComponent<LuaScriptComponent>();
        CHECK(!script.IsLoaded);
        CHECK(!script.CreateEntered);
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
                    entity.RemoveComponent<NativeScriptComponent>();
                    entity.RemoveComponent<NativeScriptComponent>();
                }
            };
            fixture.World->OnScriptStart();
            fixture.Step();
            const auto& n = fixture.Context.Native.at(static_cast<uint32_t>(native));
            const auto& l = fixture.Context.Lua.at(static_cast<uint32_t>(lua));
            CHECK(n.Updates == 1 && n.Returns == 1 && n.Destroys == 1 && n.Deletes == 1);
            CHECK(l.Updates == 1 && l.Returns == 1 && l.Destroys == 1);
            if (mode == "destroy") CHECK(!native && !lua);
            else CHECK(native && lua && !native.HasComponent<NativeScriptComponent>() && !lua.HasComponent<LuaScriptComponent>());
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
        auto view = static_cast<const Scene&>(*fixture.World).GetRegistry().view<NativeScriptComponent>();
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
                BindProbe(child.AddComponent<NativeScriptComponent>());
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
        for (auto handle : registry.view<NativeScriptComponent>())
            if (handle != static_cast<entt::entity>(source)) child = Entity(fixture.World.get(), handle);
        CHECK(child && child.HasComponent<TransformComponent>());
        CHECK(child.GetComponent<NativeScriptComponent>().State == ScriptInstanceState::Pending);
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
                    if (lua) entity.RemoveComponent<LuaScriptComponent>();
                    else entity.RemoveComponent<NativeScriptComponent>();
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
                if (lua) CHECK(source.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Faulted);
                else CHECK(source.GetComponent<NativeScriptComponent>().State == ScriptInstanceState::Faulted);
            }
        }
    }

    void RejectSynchronousCallbackWrites()
    {
        Fixture fixture;
        auto source = fixture.AddNative();
        fixture.Context.NativeAction = [&fixture](Entity entity, const std::string& phase) {
            if (phase != "update") return;
            auto* scene = entity.GetScene();
            fixture.Context.Observed["rejections"] += RejectsLogic([&] { Entity::CreateEntity(scene); });
            fixture.Context.Observed["rejections"] += RejectsLogic([&] { entity.AddComponent<TransformComponent>(); });
            fixture.Context.Observed["rejections"] += RejectsLogic([&] { scene->GetRegistry(); });
            fixture.Context.Observed["rejections"] += RejectsLogic([&] { entity.AddOrReplaceComponent<NativeScriptComponent>(); });
        };
        fixture.World->OnScriptStart();
        fixture.Step();
        CHECK(fixture.Context.Observed["rejections"] == 4);
        CHECK(!source.HasComponent<TransformComponent>());
        CHECK(source.GetComponent<NativeScriptComponent>().State == ScriptInstanceState::Running);
        auto lua = Entity{};
        CHECK(fixture.World->DeferStructuralChange([&](Scene&) { lua = fixture.AddLua(); SetLuaString(lua, "Mode", "add"); }));
        fixture.World->FlushStructuralChanges();
        fixture.Step();
        fixture.Step();
        CHECK(lua.HasComponent<TransformComponent>()); // Actual Lua AddComponent queues a type, not a raw pointer.
        CHECK(lua.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Running);
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
                    else if (cause == "remove") source.RemoveComponent<NativeScriptComponent>();
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
            else if (cause == "remove") CHECK(source && !source.HasComponent<NativeScriptComponent>());
            else if (cause == "fault")
            {
                const auto& script = source.GetComponent<NativeScriptComponent>();
                CHECK(script.State == ScriptInstanceState::Faulted && script.Instance == nullptr);
                CHECK(script.LastError.find("phase=StructuralChange") != std::string::npos);
                CHECK(script.LastError.find("intentional C1 failure") != std::string::npos);
            }
        }
    }

    void ReplacementRequiresCleanup()
    {
        Fixture fixture;
        auto runningNative = fixture.AddNative();
        auto runningLua = fixture.AddLua();
        auto faultNative = fixture.AddNative();
        faultNative.GetComponent<NativeScriptComponent>().InstantiateScript = []() -> ScriptableEntity* { return nullptr; };
        auto faultLua = fixture.AddLua("scripts/tests/does-not-exist.lua");
        fixture.World->OnScriptStart();
        Entity pendingNative, pendingLua;
        CHECK(fixture.World->DeferStructuralChange([&](Scene&) {
            pendingNative = fixture.AddNative(); pendingLua = fixture.AddLua();
        }));
        fixture.World->FlushStructuralChanges();
        auto* oldInstance = runningNative.GetComponent<NativeScriptComponent>().Instance;
        CHECK(fixture.World->DeferStructuralChange([&](Scene&) {
            for (auto entity : { runningNative, pendingNative, faultNative })
                fixture.Context.Observed["replace rejected"] += RejectsLogic([&] { entity.AddOrReplaceComponent<NativeScriptComponent>(); });
            for (auto entity : { runningLua, pendingLua, faultLua })
                fixture.Context.Observed["replace rejected"] += RejectsLogic([&] { entity.AddOrReplaceComponent<LuaScriptComponent>(); });
        }));
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Observed["replace rejected"] == 6);
        CHECK(runningNative.GetComponent<NativeScriptComponent>().Instance == oldInstance);
        CHECK(pendingNative.GetComponent<NativeScriptComponent>().State == ScriptInstanceState::Pending);
        CHECK(faultNative.GetComponent<NativeScriptComponent>().State == ScriptInstanceState::Faulted);
        CHECK(faultLua.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Faulted);
        CHECK(faultNative.GetComponent<NativeScriptComponent>().Instance == nullptr);
        CHECK(fixture.Context.Native.find(static_cast<uint32_t>(faultNative)) == fixture.Context.Native.end());
        CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(faultLua)) == fixture.Context.Lua.end());
        runningNative.RemoveComponent<NativeScriptComponent>();
        runningLua.RemoveComponent<LuaScriptComponent>();
        fixture.World->FlushStructuralChanges();
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(runningNative)).Deletes == 1);
        CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(runningLua)).Destroys == 1);
        CHECK(fixture.World->DeferStructuralChange([=](Scene&) mutable {
            BindProbe(runningNative.AddComponent<NativeScriptComponent>());
            runningLua.AddComponent<LuaScriptComponent>("scripts/tests/LifecycleProbe.lua");
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
                entity.RemoveComponent<NativeScriptComponent>();
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
            auto& script = bad.GetComponent<NativeScriptComponent>();
            CHECK(script.State == ScriptInstanceState::Faulted && script.Instance == nullptr && !script.CreateEntered);
            CHECK(script.LastError.find("T02NativeProbe") != std::string::npos);
            CHECK(script.LastError.find("entity=") != std::string::npos);
            const auto error = script.LastError;
            fixture.Step();
            fixture.Stop();
            fixture.Stop();
            CHECK(script.LastError == error);
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
            auto& script = bad.GetComponent<LuaScriptComponent>();
            CHECK(script.State == ScriptInstanceState::Faulted);
            CHECK(script.LastError.find("CallbackErrors.lua") != std::string::npos);
            CHECK(script.LastError.find("entity=") != std::string::npos);
            CHECK(script.LastError.find("phase=" + stage) != std::string::npos);
            CHECK(script.LastError.find("stack traceback") != std::string::npos);
            CheckLuaReleased(bad);
            const auto error = script.LastError;
            fixture.Step();
            fixture.Stop();
            fixture.Stop();
            CHECK(script.LastError == error);
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
            CHECK(bad.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Faulted);
            CheckLuaReleased(bad);
            CHECK(fixture.Context.Lua.find(static_cast<uint32_t>(bad)) == fixture.Context.Lua.end());
            CHECK(fixture.Context.Lua.at(static_cast<uint32_t>(good)).Updates == 1);
        }
    }

    void CloneOnlyConfiguration()
    {
        Fixture fixture;
        auto source = fixture.AddNative();
        source.AddComponent<LuaScriptComponent>("scripts/tests/LifecycleProbe.lua");
        source.AddComponent<TransformComponent>();
        source.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Dynamic;
        source.AddComponent<BoxCollider2DComponent>();
        source.GetComponent<NativeScriptComponent>().FieldValues["Value"] = 19.0f;
        SetLuaString(source, "Mode", "normal");
        fixture.World->OnRuntimeStart();
        CHECK(b2Body_IsValid(source.GetComponent<RigidBody2DComponent>().RuntimeBodyId));
        auto clone = CreateRef<Scene>(TestContext());
        Scene::CopyScene(fixture.World, clone);
        auto view = static_cast<const Scene&>(*clone).GetRegistry().view<NativeScriptComponent, LuaScriptComponent, RigidBody2DComponent>();
        CHECK(view.begin() != view.end());
        Entity copy(clone.get(), *view.begin());
        auto& native = copy.GetComponent<NativeScriptComponent>();
        CHECK(native.Instance == nullptr && native.State == ScriptInstanceState::Pending && native.Generation == 0);
        CHECK(native.InstantiateScript == source.GetComponent<NativeScriptComponent>().InstantiateScript);
        CHECK(std::get<float>(native.FieldValues.at("Value")) == 19.0f);
        CheckLuaReleased(copy);
        CHECK(copy.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Pending);
        CHECK(copy.GetComponent<LuaScriptComponent>().Generation == 0);
        CHECK(std::any_cast<std::string>(copy.GetComponent<LuaScriptComponent>().CachedFields.at("Mode").Value) == "normal");
        CHECK(B2_IS_NULL(copy.GetComponent<RigidBody2DComponent>().RuntimeBodyId));
        clone.reset();
        CHECK(source.GetComponent<NativeScriptComponent>().Instance != nullptr);
        CHECK(source.GetComponent<LuaScriptComponent>().IsLoaded);
        fixture.Stop();
        fixture.World->DuplicateEntity(source);
        CHECK(static_cast<const Scene&>(*fixture.World).GetRegistry().view<NativeScriptComponent>().size() == 2);
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
                CHECK(entity.GetComponent<LuaScriptComponent>().LastError.empty());
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
            auto& script = bad.GetComponent<LuaScriptComponent>();
            CHECK(script.State == ScriptInstanceState::Faulted);
            CHECK(script.LastError.find("intentional inherited lookup failure") != std::string::npos);
            CHECK(script.LastError.find("CallbackErrors.lua") != std::string::npos);
            CHECK(script.LastError.find("stack traceback") != std::string::npos);
            CheckLuaReleased(bad);
            const auto error = script.LastError;
            fixture.Step();
            CHECK(script.LastError == error);
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

    void NativeInspectorBorrowsRunningInstance()
    {
        Fixture fixture;
        auto entity = fixture.AddNative();
        fixture.World->OnScriptStart();
        auto& script = entity.GetComponent<NativeScriptComponent>();
        auto* instance = script.Instance;
        CHECK(instance != nullptr);

        // T04：经与 UI 框架解耦的字段访问合同，验证“借用运行实例、不新建、不销毁、FieldValues 回填”。
        bool owned = false;
        ScriptableEntity* preview = script.GetOrCreateEditorInstance(!fixture.World->IsActive(), owned);
        CHECK(preview == instance);
        CHECK(!owned);
        const Schema::TypeSchema* typeSchema = TestContext().Schemas().Find(script.ScriptName);
        CHECK(typeSchema != nullptr);
        if (typeSchema)
        {
            for (const Schema::FieldSchema& field : typeSchema->Fields)
                script.GetErasedFieldValue(*typeSchema, field, preview);
        }
        script.ReleaseEditorInstance(preview); // 借用路径不销毁

        CHECK(entity.GetComponent<NativeScriptComponent>().Instance == instance);
        CHECK(fixture.Context.Native.at(static_cast<uint32_t>(entity)).Deletes == 0);
        CHECK(entity.GetComponent<NativeScriptComponent>().FieldValues.count("Value") == 1); // Field body was actually visited.
        fixture.Step();
        fixture.Stop();
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
        CHECK(invalid.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Faulted);
        CHECK(invalid.GetComponent<LuaScriptComponent>().LastError.find("phase=Load") != std::string::npos);
        CheckLuaReleased(invalid);
        fixture.Stop();
        auto preview = fixture.AddLua("scripts/tests/CallbackErrors.lua");
        auto& script = preview.GetComponent<LuaScriptComponent>();
        CHECK(ScriptEngine::InitScriptForEditor(script));
        SetLuaString(preview, "FailStage", "Saved editor value");
        script.ScriptFilePath = invalid.GetComponent<LuaScriptComponent>().ScriptFilePath;
        CHECK(!ScriptEngine::InitScriptForEditor(script));
        CHECK(std::any_cast<std::string>(script.CachedFields.at("FailStage").Value) == "Saved editor value");
        CHECK(script.State == ScriptInstanceState::Faulted && !script.IsLoaded);
        CHECK(!script.LuaEnv.IsValid() && !script.ScriptTable.IsValid());
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
        CHECK(entity.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Running);
        fixture.Step();
        CHECK(entity.GetComponent<LuaScriptComponent>().State == ScriptInstanceState::Running);
        fixture.Stop();
        CheckLuaReleased(entity);
        CHECK(entity.GetComponent<LuaScriptComponent>().LastError.empty());
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
        if (argc != 1) throw std::runtime_error("Usage: WorldScriptTests [--generate-stubs]");
        s_OutputDirectory = fs::path(WORLD_SCRIPT_TEST_OUTPUT_DIR) / ("run-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(s_OutputDirectory);
        {
            static const Schema::ScriptBinding probeBinding = {
                [](void* raw) { BindProbe(*static_cast<NativeScriptComponent*>(raw)); }
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
            static const Schema::TypeSchema probeSchema = {
                Schema::TypeId{ "T02NativeProbe" },
                "T02NativeProbe",
                Schema::WE_SCHEMA_ABI_VERSION,
                sizeof(NativeProbe),
                Schema::TypeCategory::Script,
                { probeValue },
                nullptr,
                &probeBinding,
            };
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
            { "synchronous write rejection and Lua dynamic add", RejectSynchronousCallbackWrites },
            { "replace rejection and remove-then-add", ReplacementRequiresCleanup },
            { "stop inside create and pending cancellation", StopFromCallbackAndPendingCancellation },
            { "native errors and exactly-once cleanup", NativeErrorsReleaseExactlyOnce },
            { "Lua load/type/callback errors and isolation", LuaErrorsReleaseExactlyOnce },
            { "inherited Lua lifecycle callbacks and protected lookup", InheritedLuaCallbacksAndLookupErrors },
            { "clone configuration without instances or bodies", CloneOnlyConfiguration },
            { "body removal and component dependencies", PhysicsRemovalAndDependencies },
            { "camera reacquisition and expired Entity", CameraReacquisitionAndExpiredHandles },
            { "non-owning LayerStack detach order", NonOwningLayerDetachOrder },
            { "native Inspector borrows live instance", NativeInspectorBorrowsRunningInstance },
            { "syntax errors preserve preview cache", SyntaxErrorsAndPreviewCache },
            { "deterministic and atomic stub generation", StubGenerationContracts },
            { "real static-link bindings and template", RealBindingsAndTemplate },
            { "VM restart keeps metadata unique", VmRestartKeepsUniqueMetadata }
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
