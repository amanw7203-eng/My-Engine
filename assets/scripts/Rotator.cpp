#include "Scripting/Script.h"

// Spins its entity around the vertical axis.
class Rotator : public Script
{
public:
    float speed = 45.0f; // degrees per second

    void OnUpdate(float deltaTime) override
    {
        GetTransform().rotation.y += speed * deltaTime;
    }
};
REGISTER_SCRIPT(Rotator)
