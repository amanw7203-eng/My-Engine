// Compiled into the scripts DLL only (see the GameScripts target in
// CMakeLists.txt): the two functions the engine looks up after loading it.

#include "Scripting/Script.h"

extern "C" __declspec(dllexport) int GetScriptApiVersion()
{
    return kScriptApiVersion;
}

// Every script registered with REGISTER_SCRIPT in this DLL.
extern "C" __declspec(dllexport) const ScriptRegistry::Entry* GetScripts(int* count)
{
    const std::vector<ScriptRegistry::Entry>& entries = ScriptRegistry::Entries();
    *count = static_cast<int>(entries.size());
    return entries.data();
}
