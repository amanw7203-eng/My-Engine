#include "Scripting/Script.h"

#include <cmath>

// Floats its entity up and down around where it started.
class Bobber : public Script
{
public:
    float height = 0.3f; // how far up and down
    float speed = 1.5f;  // cycles per second

    void OnStart() override
    {
        m_StartY = GetTransform().position.y;
    }

    void OnUpdate(float) override
    {
        GetTransform().position.y = m_StartY + height * std::sin(Time() * speed * 6.2831853f);
    }

private:
    float m_StartY = 0.0f;
};
REGISTER_SCRIPT(Bobber)
