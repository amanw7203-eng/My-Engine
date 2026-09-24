#pragma once

// The gameplay scripting interface, shared by the engine and the scripts
// DLL (GameScripts, built from assets/scripts/). A script is a C++ class:
//
//     #include "Scripting/Script.h"
//
//     class Spinner : public Script
//     {
//     public:
//         float speed = 90.0f; // degrees per second
//
//         void OnUpdate(float deltaTime) override
//         {
//             GetTransform().rotation.y += speed * deltaTime;
//         }
//     };
//     REGISTER_SCRIPT(Spinner)
//
// Pick it in an entity's Inspector (Script), press Play, and it runs: one
// instance per entity, OnStart once, then OnUpdate every frame, OnDestroy
// when play stops or the entity is destroyed. Build Scripts (Ctrl+B) in
// the editor recompiles the DLL and swaps it in without a restart.
//
// Entities' plain fields (name, transform, mesh, material, light, visible,
// script) can be read and changed directly; everything else that needs the
// engine goes through the functions below.

#include "Scene/Entity.h"

#include <SDL3/SDL_scancode.h>
#include <glm/glm.hpp>

#include <cstdio>
#include <vector>

class Mesh;
struct Material;

// Bumped whenever this file changes in a way that breaks existing DLLs
// (the engine refuses a DLL built against a different version).
inline constexpr int kScriptApiVersion = 1;

// What the engine offers scripts: plain function pointers, so the DLL
// needs nothing from the engine's code, only this header. `engine` is
// passed back to each.
struct ScriptApi
{
    void* engine = nullptr;

    Entity* (*findEntity)(void* engine, const char* name);
    Entity* (*createEntity)(void* engine, const char* name, Entity* parent);
    void (*destroyEntity)(void* engine, Entity* entity);
    const Mesh* (*findMesh)(void* engine, const char* name);
    Material* (*findMaterial)(void* engine, const char* name);

    bool (*isKeyDown)(void* engine, SDL_Scancode key);
    bool (*wasKeyPressed)(void* engine, SDL_Scancode key);
    float (*time)(void* engine);
    void (*log)(void* engine, const char* message);

    glm::vec3 (*getCameraPosition)(void* engine);
    void (*setCameraPosition)(void* engine, glm::vec3 position);
    void (*cameraLookAt)(void* engine, glm::vec3 target);
};

class Script
{
public:
    virtual ~Script() = default;

    // Once, before the first OnUpdate (when play starts, or when the entity
    // appears during play).
    virtual void OnStart() {}
    // Every frame while playing. deltaTime: seconds since the last frame.
    virtual void OnUpdate(float /*deltaTime*/) {}
    // When play stops, the entity is destroyed, or the scripts are reloaded.
    virtual void OnDestroy() {}

protected:
    // --- This entity ---
    Entity& GetEntity() { return *m_Entity; }
    Transform& GetTransform() { return m_Entity->transform; }

    // --- The scene ---
    // The first entity with this name, or nullptr.
    Entity* FindEntity(const char* name) { return m_Api->findEntity(m_Api->engine, name); }
    Entity& CreateEntity(const char* name, Entity* parent = nullptr)
    {
        return *m_Api->createEntity(m_Api->engine, name, parent);
    }
    // Also destroys its children. (Everything reverts when play stops.)
    void DestroyEntity(Entity& entity) { m_Api->destroyEntity(m_Api->engine, &entity); }
    // Assets by their names in the Assets panel, or nullptr.
    const Mesh* FindMesh(const char* name) { return m_Api->findMesh(m_Api->engine, name); }
    Material* FindMaterial(const char* name) { return m_Api->findMaterial(m_Api->engine, name); }

    // --- Input (keys by position, e.g. SDL_SCANCODE_W) ---
    bool IsKeyDown(SDL_Scancode key) { return m_Api->isKeyDown(m_Api->engine, key); }
    // Pressed this frame (not held since before).
    bool WasKeyPressed(SDL_Scancode key) { return m_Api->wasKeyPressed(m_Api->engine, key); }

    // Seconds since play started.
    float Time() { return m_Api->time(m_Api->engine); }

    // --- The camera ---
    glm::vec3 GetCameraPosition() { return m_Api->getCameraPosition(m_Api->engine); }
    void SetCameraPosition(const glm::vec3& position) { m_Api->setCameraPosition(m_Api->engine, position); }
    void CameraLookAt(const glm::vec3& target) { m_Api->cameraLookAt(m_Api->engine, target); }

    // To the editor's Console, printf style.
    template <typename... Args>
    void Log(const char* format, Args... args)
    {
        char message[1024];
        if constexpr (sizeof...(Args) == 0)
            std::snprintf(message, sizeof(message), "%s", format);
        else
            std::snprintf(message, sizeof(message), format, args...);
        m_Api->log(m_Api->engine, message);
    }

private:
    friend class ScriptSystem; // sets these when it creates the script
    const ScriptApi* m_Api = nullptr;
    Entity* m_Entity = nullptr;
};

// --- Registration (used by the scripts DLL) ---
namespace ScriptRegistry
{
    struct Entry
    {
        const char* name;
        Script* (*create)();
    };

    // Every script in this module (the DLL has its own list).
    inline std::vector<Entry>& Entries()
    {
        static std::vector<Entry> entries;
        return entries;
    }

    struct Registrar
    {
        Registrar(const char* name, Script* (*create)()) { Entries().push_back({ name, create }); }
    };
}

// Makes the class available to pick in the editor, under its class name.
#define REGISTER_SCRIPT(Type)                                                                       \
    static ScriptRegistry::Registrar s_ScriptRegistrar_##Type(#Type, []() -> Script* { return new Type(); });

// What the DLL exports (defined in ScriptModule.cpp, compiled into it).
using GetScriptApiVersionFn = int (*)();
using GetScriptsFn = const ScriptRegistry::Entry* (*)(int* count);
