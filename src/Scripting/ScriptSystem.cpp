#include "Scripting/ScriptSystem.h"

#include "Assets/AssetLibrary.h"
#include "Platform/FileSystem.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"
#include "UI/Console.h"

#ifdef _MSC_VER
#include <excpt.h>
#endif

#include <algorithm>
#include <sstream>
#include <utility>

namespace
{
    // How often the DLL's time stamp is checked for a new build.
    constexpr std::chrono::milliseconds kReloadCheckInterval{ 300 };

    // Calls fn(context), returning false instead of crashing if it faults
    // (access violation, divide by zero, an uncaught C++ exception...).
    // Kept free of C++ objects with destructors, which __try doesn't allow.
    bool RunGuarded(void (*fn)(void*), void* context)
    {
#ifdef _MSC_VER
        __try
        {
            fn(context);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        fn(context);
        return true;
#endif
    }

    // "GameScripts-live-3.dll" beside "GameScripts.dll": the copy we load.
    std::filesystem::path CopyPath(const std::filesystem::path& dll, int number)
    {
        return dll.parent_path() /
               Platform::FromUtf8(Platform::ToUtf8(dll.stem()) + "-live-" + std::to_string(number) + ".dll");
    }
}

ScriptSystem::ScriptSystem(Scene& scene, AssetLibrary& assets, Camera& camera, Console& console,
                           std::filesystem::path dllPath, BuildCommand buildCommand)
    : m_Scene(scene)
    , m_Assets(assets)
    , m_Camera(camera)
    , m_Console(console)
    , m_DllPath(std::move(dllPath))
    , m_BuildCommand(std::move(buildCommand))
    , m_Api(MakeApi(this))
{
    // Copies left behind by an editor that didn't shut down cleanly.
    std::error_code error;
    const std::string prefix = Platform::ToUtf8(m_DllPath.stem()) + "-live-";
    for (const auto& entry : std::filesystem::directory_iterator(m_DllPath.parent_path(), error))
    {
        const std::string name = Platform::ToUtf8(entry.path().filename());
        if (name.rfind(prefix, 0) == 0)
            std::filesystem::remove(entry.path(), error); // fails if another editor has it loaded: fine
    }

    Load();
}

ScriptSystem::~ScriptSystem()
{
    if (m_BuildThread.joinable())
        m_BuildThread.join();
    Unload();
}

// --- Loading ----------------------------------------------------------------

bool ScriptSystem::Load()
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(m_DllPath, error);
    if (error)
        return false; // not built yet
    // Recorded even if loading fails, so a broken DLL isn't retried (and
    // reported) again until it changes.
    m_LoadedTime = time;
    m_SeenTime = time;

    // Load a copy: Windows locks a loaded DLL, and the original must stay
    // free for the next build to overwrite.
    const std::filesystem::path copy = CopyPath(m_DllPath, ++m_CopyNumber);
    std::filesystem::copy_file(m_DllPath, copy, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
        m_Console.Log(Console::Level::Error, "Couldn't copy the scripts DLL: " + error.message());
        return false;
    }

    m_Library = SDL_LoadObject(Platform::ToUtf8(copy).c_str());
    if (!m_Library)
    {
        m_Console.Log(Console::Level::Error, std::string("Couldn't load the scripts DLL: ") + SDL_GetError());
        std::filesystem::remove(copy, error);
        return false;
    }
    m_LoadedCopy = copy;

    const auto getVersion = reinterpret_cast<GetScriptApiVersionFn>(SDL_LoadFunction(m_Library, "GetScriptApiVersion"));
    const auto getScripts = reinterpret_cast<GetScriptsFn>(SDL_LoadFunction(m_Library, "GetScripts"));
    if (!getVersion || !getScripts)
    {
        m_Console.Log(Console::Level::Error, "The scripts DLL doesn't export GetScripts / GetScriptApiVersion.");
        Unload();
        return false;
    }
    // Built against a different Script.h: its classes' layout may not
    // match what the engine expects, so calling them could crash.
    if (getVersion() != kScriptApiVersion)
    {
        m_Console.Log(Console::Level::Error,
                      "The scripts DLL was built for scripting API version " + std::to_string(getVersion()) +
                          ", the engine has " + std::to_string(kScriptApiVersion) + ". Build Scripts to update it.");
        Unload();
        return false;
    }

    int count = 0;
    const ScriptRegistry::Entry* entries = getScripts(&count);
    m_Entries.assign(entries, entries + count);
    std::string list;
    for (const ScriptRegistry::Entry& entry : m_Entries)
    {
        m_Names.emplace_back(entry.name);
        list += (list.empty() ? "" : ", ") + m_Names.back();
    }
    std::sort(m_Names.begin(), m_Names.end());
    m_Console.Log(Console::Level::Info, "Loaded " + std::to_string(count) + " script" + (count == 1 ? "" : "s") +
                                            (count ? ": " + list : ""));
    return true;
}

void ScriptSystem::Unload()
{
    // Instances' code (vtables) lives in the DLL: they go first.
    DestroyInstances();
    m_Entries.clear();
    m_Names.clear();
    if (m_Library)
        SDL_UnloadObject(m_Library);
    m_Library = nullptr;
    if (!m_LoadedCopy.empty())
    {
        std::error_code error;
        std::filesystem::remove(m_LoadedCopy, error);
        m_LoadedCopy.clear();
    }
}

void ScriptSystem::PollReload()
{
    // A build finished (on its thread): collect it, and load it right away.
    if (m_BuildFinished.exchange(false))
    {
        if (m_BuildThread.joinable())
            m_BuildThread.join();
        m_LastBuildResult = m_BuildSucceeded ? BuildResult::Succeeded : BuildResult::Failed;
        if (m_BuildSucceeded)
        {
            Unload();
            Load();
            return;
        }
    }
    if (m_Building)
        return;

    // A build from outside the editor (e.g. VS Code): notice the DLL
    // changing, and load it once it has stopped changing.
    const auto now = std::chrono::steady_clock::now();
    if (now - m_LastCheck < kReloadCheckInterval)
        return;
    m_LastCheck = now;

    std::error_code error;
    const auto time = std::filesystem::last_write_time(m_DllPath, error);
    if (error || time == m_LoadedTime)
        return;
    if (time != m_SeenTime)
    {
        m_SeenTime = time; // still being written, perhaps: check again next time
        return;
    }
    m_Console.Log(Console::Level::Info, "Scripts DLL changed: reloading.");
    Unload();
    Load();
}

// --- Building ---------------------------------------------------------------

void ScriptSystem::StartBuild()
{
    if (m_Building)
        return;
    if (m_BuildThread.joinable())
        m_BuildThread.join();

    m_Building = true;
    m_LastBuildResult = BuildResult::None;
    m_Console.Log(Console::Level::Info, "Building scripts...");

    m_BuildThread = std::thread([this] {
        const BuildCommand& command = m_BuildCommand;
        const char* args[] = { command.cmake.c_str(), "--build", command.buildDir.c_str(), "--config",
                               command.config.c_str(), "--target", "GameScripts", nullptr };

        // Capture what the build prints (errors included) for the Console.
        const SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void*>(args));
        SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);

        bool succeeded = false;
        if (!process)
        {
            m_Console.Log(Console::Level::Error, std::string("Couldn't run CMake: ") + SDL_GetError());
        }
        else
        {
            size_t size = 0;
            int exitCode = -1;
            // Waits for the build to finish, collecting its output.
            if (void* output = SDL_ReadProcess(process, &size, &exitCode))
            {
                std::istringstream lines(std::string(static_cast<const char*>(output), size));
                SDL_free(output);
                for (std::string line; std::getline(lines, line);)
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (line.empty())
                        continue;
                    const Console::Level level = line.find(": error") != std::string::npos ? Console::Level::Error
                                               : line.find(": warning") != std::string::npos ? Console::Level::Warning
                                                                                             : Console::Level::Info;
                    m_Console.Log(level, line);
                }
            }
            succeeded = exitCode == 0;
            SDL_DestroyProcess(process);
        }

        m_Console.Log(succeeded ? Console::Level::Info : Console::Level::Error,
                      succeeded ? "Scripts built." : "Building scripts failed: see the errors above.");
        m_BuildSucceeded = succeeded;
        m_Building = false;
        m_BuildFinished = true;
    });
}

// --- Play mode --------------------------------------------------------------

void ScriptSystem::BeginPlay()
{
    m_Playing = true;
    m_StopRequested = false;
    m_Time = 0.0f;
    m_Keys.clear();
    m_PreviousKeys.clear();
    // Instances are created by the first Update, for every entity with a script.
}

void ScriptSystem::EndPlay()
{
    DestroyInstances();
    m_Playing = false;
}

bool ScriptSystem::TakeStopRequest()
{
    return std::exchange(m_StopRequested, false);
}

void ScriptSystem::Update(float deltaTime, bool keyboardFree)
{
    if (!m_Playing)
        return;

    // Keys now and last frame, for IsKeyDown / WasKeyPressed.
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    m_PreviousKeys = std::move(m_Keys);
    m_Keys.assign(keys, keys + keyCount);
    m_PreviousKeys.resize(m_Keys.size(), 0);
    m_KeyboardFree = keyboardFree;
    m_Time += deltaTime;

    for (Entity* entity : SyncInstances())
    {
        if (m_StopRequested)
            break;
        auto it = m_Instances.find(entity);
        if (it == m_Instances.end() || it->second.crashed)
            continue;
        Call(entity, it->second, "OnUpdate", [](Script* script, float dt) { script->OnUpdate(dt); }, deltaTime);
    }
}

std::vector<Entity*> ScriptSystem::SyncInstances()
{
    // Instances whose entity is gone, or now has a different script.
    for (auto it = m_Instances.begin(); it != m_Instances.end();)
    {
        Entity* entity = it->first;
        if (!m_Scene.Contains(entity) || entity->script != it->second.name)
        {
            DestroyInstance(entity, it->second);
            it = m_Instances.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Scripts run on the entities that are shown (hiding one pauses it).
    std::vector<Entity*> active;
    m_Scene.ForEachVisible([&](Entity& entity, const glm::mat4&) {
        if (!entity.script.empty())
            active.push_back(&entity);
    });

    for (Entity* entity : active)
    {
        if (m_StopRequested)
            break;
        if (m_Instances.contains(entity))
            continue;
        const auto entry = std::find_if(m_Entries.begin(), m_Entries.end(),
                                        [&](const ScriptRegistry::Entry& e) { return entity->script == e.name; });
        if (entry == m_Entries.end())
            continue; // not in the DLL (the Inspector says so)

        // Create it (a constructor can crash too), hook it up, start it.
        struct Create
        {
            Script* (*create)();
            Script* result;
        } create{ entry->create, nullptr };
        if (!RunGuarded([](void* context) {
                auto* c = static_cast<Create*>(context);
                c->result = c->create();
            }, &create) || !create.result)
        {
            m_Console.Log(Console::Level::Error,
                          "Script '" + entity->script + "' on '" + entity->name + "' crashed while being created.");
            m_StopRequested = true;
            break;
        }
        create.result->m_Api = &m_Api;
        create.result->m_Entity = entity;
        Instance& instance = m_Instances[entity];
        instance.script = create.result;
        instance.name = entity->script;
        Call(entity, instance, "OnStart", [](Script* script, float) { script->OnStart(); });
    }
    return active;
}

void ScriptSystem::DestroyInstance(Entity* entity, Instance& instance)
{
    // A script that crashed may be in any state: leave it alone (leaked).
    if (instance.crashed || !instance.script)
        return;
    if (Call(entity, instance, "OnDestroy", [](Script* script, float) { script->OnDestroy(); }))
        Call(entity, instance, "its destructor", [](Script* script, float) { delete script; });
    instance.script = nullptr;
}

void ScriptSystem::DestroyInstances()
{
    for (auto& [entity, instance] : m_Instances)
        DestroyInstance(entity, instance);
    m_Instances.clear();
}

bool ScriptSystem::Call(Entity* entity, Instance& instance, const char* what, void (*call)(Script*, float),
                        float value)
{
    struct Context
    {
        void (*call)(Script*, float);
        Script* script;
        float value;
    } context{ call, instance.script, value };

    if (RunGuarded([](void* c) {
            auto* context = static_cast<Context*>(c);
            context->call(context->script, context->value);
        }, &context))
        return true;

    instance.crashed = true;
    m_StopRequested = true;
    m_Console.Log(Console::Level::Error, "Script '" + instance.name + "' on '" + entity->name + "' crashed in " +
                                             what + ". Play stopped.");
    return false;
}

// --- The API scripts call ---------------------------------------------------

ScriptApi ScriptSystem::MakeApi(ScriptSystem* self)
{
    ScriptApi api;
    api.engine = self;
    api.findEntity = [](void* engine, const char* name) -> Entity* {
        return name ? static_cast<ScriptSystem*>(engine)->m_Scene.FindEntity(name) : nullptr;
    };
    api.createEntity = [](void* engine, const char* name, Entity* parent) -> Entity* {
        ScriptSystem& s = *static_cast<ScriptSystem*>(engine);
        return &s.m_Scene.CreateEntity(name ? name : "Entity", parent && s.m_Scene.Contains(parent) ? parent : nullptr);
    };
    api.destroyEntity = [](void* engine, Entity* entity) {
        ScriptSystem& s = *static_cast<ScriptSystem*>(engine);
        if (entity && s.m_Scene.Contains(entity))
            s.m_Scene.DestroyEntity(*entity);
    };
    api.findMesh = [](void* engine, const char* name) -> const Mesh* {
        for (const AssetLibrary::NamedMesh& entry : static_cast<ScriptSystem*>(engine)->m_Assets.GetMeshes())
            if (name && entry.name == name)
                return entry.mesh.get();
        return nullptr;
    };
    api.findMaterial = [](void* engine, const char* name) -> Material* {
        for (const std::unique_ptr<Material>& material : static_cast<ScriptSystem*>(engine)->m_Assets.GetMaterials())
            if (name && material->name == name)
                return material.get();
        return nullptr;
    };
    api.isKeyDown = [](void* engine, SDL_Scancode key) {
        const ScriptSystem& s = *static_cast<ScriptSystem*>(engine);
        const auto k = static_cast<size_t>(key);
        return s.m_KeyboardFree && k < s.m_Keys.size() && s.m_Keys[k] != 0;
    };
    api.wasKeyPressed = [](void* engine, SDL_Scancode key) {
        const ScriptSystem& s = *static_cast<ScriptSystem*>(engine);
        const auto k = static_cast<size_t>(key);
        return s.m_KeyboardFree && k < s.m_Keys.size() && s.m_Keys[k] != 0 && s.m_PreviousKeys[k] == 0;
    };
    api.time = [](void* engine) { return static_cast<ScriptSystem*>(engine)->m_Time; };
    api.log = [](void* engine, const char* message) {
        static_cast<ScriptSystem*>(engine)->m_Console.Log(Console::Level::Info, message ? message : "");
    };
    api.getCameraPosition = [](void* engine) { return static_cast<ScriptSystem*>(engine)->m_Camera.GetPosition(); };
    api.setCameraPosition = [](void* engine, glm::vec3 position) {
        static_cast<ScriptSystem*>(engine)->m_Camera.SetPosition(position);
    };
    api.cameraLookAt = [](void* engine, glm::vec3 target) {
        static_cast<ScriptSystem*>(engine)->m_Camera.LookAt(target);
    };
    return api;
}
