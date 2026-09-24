#include "Scene/Entity.h"

glm::mat4 Entity::GetWorldMatrix() const
{
    const glm::mat4 local = transform.GetMatrix();
    return m_Parent ? m_Parent->GetWorldMatrix() * local : local;
}

bool Entity::IsAncestorOf(const Entity& other) const
{
    for (const Entity* p = other.m_Parent; p; p = p->m_Parent)
    {
        if (p == this)
            return true;
    }
    return false;
}

bool Entity::IsVisibleInHierarchy() const
{
    for (const Entity* e = this; e; e = e->m_Parent)
    {
        if (!e->visible)
            return false;
    }
    return true;
}
