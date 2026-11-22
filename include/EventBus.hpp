#pragma once

#include "EventType.hpp"
#include "Events.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// Dispatches events to registered handlers.
//
// Safe against handlers that add or remove handlers (including themselves)
// while an event is being emitted: emit() works on a snapshot, and a handler
// removed mid-emit is not called afterwards. A handler's captured state is
// destroyed once nothing can call it any more, so RAII captures (e.g. a Lua
// registry ref) are released on remove()/clear().
class EventBus
{
public:
    using Callback = std::function<void(const EventData &)>;

    // Returns an id usable with remove(). A `once` handler is removed just
    // before its first call.
    int add(EventType type, Callback callback, bool once = false);
    bool remove(int id);
    void clear();
    void emit(EventType type, const EventData &data = {});

    // Lets emitters skip building a payload nobody is listening for.
    bool hasHandlers(EventType type) const;

private:
    struct Handler
    {
        int id;
        EventType type;
        Callback callback;
        bool once;
        bool dead = false;
    };

    void detach(const std::shared_ptr<Handler> &h);

    int m_nextId = 1;
    std::unordered_map<EventType, std::vector<std::shared_ptr<Handler>>>
        m_handlers;
};
