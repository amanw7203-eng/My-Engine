#version 330 core

// Writes which entity covers this pixel as a plain integer, so the engine
// can read back what was clicked. 0 means nothing.
uniform uint uEntityId;

layout (location = 0) out uint EntityId;

void main()
{
    EntityId = uEntityId;
}
