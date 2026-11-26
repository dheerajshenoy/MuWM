#pragma once

enum class EventType
{
    Startup = 0,
    Quit,
    ConfigReload,

    WindowOpen,
    WindowClose,
    WindowFocus,
    WindowUnfocus,
    WindowMove,
    WindowResize,
    WindowMinimize,
    WindowMaximize,
    WindowFullscreen,

    WorkspaceChange,
    WorkspaceAdd,
    WorkspaceRemove,
    WorkspaceRename,
    WorkspaceReorder,
};
