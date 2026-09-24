#pragma once

#include "Scripting/Script.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class AssetLibrary;
class Camera;
class Console;
class Scene;

// Runs the gameplay scripts (see Scripting/Script.h): loads them from the
// GameScripts DLL, hot-reloads it when it is rebuilt, builds it on request,
// and in Play mode gives each entity with a script its own instance and
// calls it every frame.
//
// Hot reload: the engine loads a copy of the DLL rather than the DLL
// itself, so the original stays free to be rebuilt while the editor runs.
// When it changes (built from the editor or from outside), the copy is
// swapped for a fresh one. Script instances are recreated (their fields
// start over) and started again.
//
// A script that crashes (a bad pointer, an exception) is caught rather than
// taking the editor down; play is then stopped (see TakeStopRequest).
class ScriptSystem
{
public:
    // How to rebuild the DLL: `cmake --build <buildDir> --config <config>
    // --target GameScripts`.
    struct BuildCommand
    {
        std::string cmake;
        std::string buildDir;
        std::string config;
    };

    // `dllPath`: the GameScripts DLL the build writes.
    ScriptSystem(Scene& scene, AssetLibrary& assets, Camera& camera, Console& console,
                 std::filesystem::path dllPath, BuildCommand buildCommand);
    ~ScriptSystem();

    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Call once per frame: swaps in a new DLL if there is one.
    void PollReload();

    // --- Play mode ---
    void BeginPlay();
    // Every frame while playing. keyboardFree: false while the editor's UI
    // has the keyboard (typing in a field), so scripts see no keys.
    void Update(float deltaTime, bool keyboardFree);
    void EndPlay();
    bool IsPlaying() const { return m_Playing; }
    // True once after a script crashed: the caller should stop play.
    bool TakeStopRequest();

    // --- Building ---
    // Rebuilds the DLL in the background; its output goes to the Console
    // and it is loaded when done.
    void StartBuild();
    bool IsBuilding() const { return m_Building; }
    enum class BuildResult
    {
        None,
        Succeeded,
        Failed,
    };
    BuildResult GetLastBuildResult() const { return m_LastBuildResult; }

    // The scripts in the loaded DLL, by name (empty if none is loaded).
    const std::vector<std::string>& GetScriptNames() const { return m_Names; }
    bool IsLoaded() const { return m_Library != nullptr; }

private:
    struct Instance
    {
        Script* script = nullptr;
        std::string name;
        bool crashed = false; // never called again (its state may be broken)
    };

    bool Load();
    void Unload();
    void DestroyInstances();
    void DestroyInstance(Entity* entity, Instance& instance);
    // Creates instances for entities that got a script, drops ones whose
    // entity is gone or changed script. Returns the entities to update.
    std::vector<Entity*> SyncInstances();
    // Calls into a script, catching a crash. `what` names the call for the
    // message. Returns false (and requests a stop) if it crashed.
    bool Call(Entity* entity, Instance& instance, const char* what, void (*call)(Script*, float), float value = 0.0f);

    // The engine side of ScriptApi.
    static ScriptApi MakeApi(ScriptSystem* self);

    Scene& m_Scene;
    AssetLibrary& m_Assets;
    Camera& m_Camera;
    Console& m_Console;
    std::filesystem::path m_DllPath;
    BuildCommand m_BuildCommand;
    ScriptApi m_Api;

    // The loaded copy of the DLL.
    SDL_SharedObject* m_Library = nullptr;
    std::filesystem::path m_LoadedCopy;
    int m_CopyNumber = 0;
    std::filesystem::file_time_type m_LoadedTime{};
    std::vector<std::string> m_Names;
    std::vector<ScriptRegistry::Entry> m_Entries;

    // Checking the DLL for changes: a few times a second, and only acting
    // once its time stamp has held still (so a half-written file from a
    // build in progress isn't loaded).
    std::chrono::steady_clock::time_point m_LastCheck{};
    std::filesystem::file_time_type m_SeenTime{};

    bool m_Playing = false;
    bool m_StopRequested = false;
    float m_Time = 0.0f;
    std::unordered_map<Entity*, Instance> m_Instances;

    // Keyboard state for WasKeyPressed: this frame's and last frame's.
    std::vector<Uint8> m_Keys;
    std::vector<Uint8> m_PreviousKeys;
    bool m_KeyboardFree = true;

    std::thread m_BuildThread;
    std::atomic<bool> m_Building = false;
    std::atomic<bool> m_BuildFinished = false;
    std::atomic<bool> m_BuildSucceeded = false;
    BuildResult m_LastBuildResult = BuildResult::None;
};
