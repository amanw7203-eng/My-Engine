#pragma once

#include <mutex>
#include <string>
#include <vector>

// The editor's "Console" panel: messages from scripts (Log) and from
// building them. Messages may be added from any thread (the build runs on
// its own); each also goes to stdout / stderr.
class Console
{
public:
    enum class Level
    {
        Info,
        Warning,
        Error,
    };

    void Log(Level level, std::string message);
    void Clear();

    // Call between ImGuiLayer::BeginFrame and EndFrame.
    void Draw();

private:
    struct Line
    {
        Level level;
        std::string text;
    };

    std::mutex m_Mutex;
    std::vector<Line> m_Lines;
};
