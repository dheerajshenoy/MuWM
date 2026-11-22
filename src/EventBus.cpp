#include "EventBus.hpp"

#include <algorithm>

int
EventBus::add(EventType type, Callback callback, bool once)
{
    const int id = m_nextId++;
    m_handlers[type].push_back(std::make_shared<Handler>(
        Handler{id, type, std::move(callback), once}));
    return id;
}

void
EventBus::detach(const std::shared_ptr<Handler> &h)
{
    h->dead = true;
    auto &list = m_handlers[h->type];
    list.erase(std::remove(list.begin(), list.end(), h), list.end());
}

bool
EventBus::remove(int id)
{
    for (auto &[type, list] : m_handlers)
        for (const auto &h : list)
            if (h->id == id)
            {
                // detach() erases from `list`, so copy before touching it
                auto handler = h;
                detach(handler);
                return true;
            }
    return false;
}

bool
EventBus::hasHandlers(EventType type) const
{
    auto it = m_handlers.find(type);
    return it != m_handlers.end() && !it->second.empty();
}

void
EventBus::clear()
{
    for (auto &[type, list] : m_handlers)
        for (auto &h : list)
            h->dead = true;
    m_handlers.clear();
}

void
EventBus::emit(EventType type, const EventData &data)
{
    auto it = m_handlers.find(type);
    if (it == m_handlers.end())
        return;

    // Snapshot: handlers may add/remove handlers while we iterate.
    const auto snapshot = it->second;
    for (const auto &h : snapshot)
    {
        if (h->dead)
            continue;
        if (h->once)
            detach(h);
        h->callback(data);
    }
}
