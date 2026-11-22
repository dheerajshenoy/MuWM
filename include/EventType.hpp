#pragma once

enum class EventType
{
    Startup = 0,
    Shutdown,
    ConfigReload,

    // Window events
    WindowOpen,
    WindowClose,
    WindowFocus,
    WindowUnfocus,
    WindowMove,
    WindowResize,
    WindowMinimize,
    WindowMaximize,
    WindowFullscreen,

    // Workspace events
    WorkspaceChange,
    WorkspaceAdd,
    WorkspaceRemove,
    WorkspaceRename,
    WorkspaceReorder,
    WorkspaceMoveWindow,
};
