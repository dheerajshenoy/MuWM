// Lua API: the `muwm` global table, its `muwm.opts` option proxies, and
// loading of the user's init.lua.

#include "MuWM.hpp"

#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <expected>
#include <filesystem>
#include <variant>

namespace
{

struct Group;

struct Field
{
    enum Kind
    {
        String,
        Enum,
        Bool,
        Float,
        Int,
        Color,
        Sub,
        StringList,
        Func
    } kind;
    const char *name;
    void *ptr        = nullptr; // std::string*, int*, float[4], or Group*
    lua_CFunction fn = nullptr;
    int changes      = 0; // MuWM::Change flags to apply after a write
    int min          = INT_MIN;
    int max          = INT_MAX;
    const std::vector<std::string> *enum_names = nullptr; // for Kind::Enum
    float fmin = -FLT_MAX; // range for Kind::Float
    float fmax = FLT_MAX;
};

struct Group
{
    std::vector<Field> fields;
};

Field
stringField(const char *name, std::string *p, int changes = 0)
{
    return {Field::String, name, p, nullptr, changes};
}

Field
intField(const char *name, int *p, int min = INT_MIN, int max = INT_MAX,
         int changes = 0)
{
    return {Field::Int, name, p, nullptr, changes, min, max};
}

Field
stringListField(const char *name, std::vector<std::string> *p, int changes = 0)
{
    return {Field::StringList, name, p, nullptr, changes};
}

Field
enumField(const char *name, void *p, const std::vector<std::string> *enum_names,
          int changes = 0)
{
    return {Field::Enum, name,    p,       nullptr,
            changes,     INT_MIN, INT_MAX, enum_names};
}

Field
boolField(const char *name, bool *p, int changes = 0)
{
    return {Field::Bool, name, p, nullptr, changes};
}

Field
floatField(const char *name, float *p, int changes = 0, float fmin = -FLT_MAX,
           float fmax = FLT_MAX)
{
    return {Field::Float, name,    p,       nullptr, changes,
            INT_MIN,      INT_MAX, nullptr, fmin,    fmax};
}

Field
colorField(const char *name, float *p, int changes = 0)
{
    return {Field::Color, name, p, nullptr, changes};
}

Field
subField(const char *name, Group *g)
{
    return {Field::Sub, name, g};
}

Field
funcField(const char *name, lua_CFunction fn)
{
    return {Field::Func, name, nullptr, fn};
}

const Field *
findField(const Group *g, const char *name)
{
    for (const Field &f : g->fields)
        if (std::strcmp(f.name, name) == 0)
            return &f;
    return nullptr;
}

char kWmKey; // its address is used as a registry key

MuWM *
wmFrom(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kWmKey);
    auto *wm = static_cast<MuWM *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return wm;
}

// The proxy closures carry their Group as upvalue 1.
const Group *
groupFrom(lua_State *L)
{
    return static_cast<const Group *>(lua_touserdata(L, lua_upvalueindex(1)));
}

// std::filesystem doesn't expand "~", so resolve the path ourselves.
std::filesystem::path
configFilePath()
{
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / PROJECT_NAME / "init.lua";
    if (const char *home = getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / PROJECT_NAME
               / "init.lua";
    return {};
}

bool
parseHexColor(const char *str, float out[4])
{
    if (!str || str[0] != '#')
        return false;
    const size_t len = std::strlen(str + 1);
    if (len != 6 && len != 8)
        return false;

    unsigned v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < len / 2; i++)
    {
        char pair[3] = {str[1 + 2 * i], str[2 + 2 * i], '\0'};
        // strtoul alone would accept " f", "+f" etc., so check the digits
        if (!std::isxdigit(static_cast<unsigned char>(pair[0]))
            || !std::isxdigit(static_cast<unsigned char>(pair[1])))
            return false;
        v[i] = static_cast<unsigned>(std::strtoul(pair, nullptr, 16));
    }
    for (int i = 0; i < 4; i++)
        out[i] = static_cast<float>(v[i]) / 255.0f;
    return true;
}

void
expectType(lua_State *L, int val, int type, const char *key, const char *what)
{
    if (lua_type(L, val) != type)
        luaL_error(L, "option '%s' expects %s", key, what);
}

// Reads a "#rrggbb[aa]" string or {r, g, b, a} table into `out`.
void
readColor(lua_State *L, int val, const char *key, float out[4])
{
    if (lua_type(L, val) == LUA_TSTRING)
    {
        if (!parseHexColor(lua_tostring(L, val), out))
            luaL_error(L,
                       "option '%s': invalid color string (expected "
                       "\"#rrggbb\" or \"#rrggbbaa\")",
                       key);
        return;
    }
    if (lua_type(L, val) != LUA_TTABLE || lua_rawlen(L, val) != 4)
        luaL_error(L,
                   "option '%s' expects a color: \"#rrggbb[aa]\" or a table "
                   "{r, g, b, a}",
                   key);
    for (int i = 0; i < 4; i++)
    {
        lua_rawgeti(L, val, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER)
            luaL_error(L, "option '%s': entry %d is not a number", key, i + 1);
        out[i] = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
}

void
setField(lua_State *L, const Group *g, const char *key, int val);

// Assigns each entry of the table at `val` to the matching field of `g`.
void
mergeTable(lua_State *L, const Group *g, int val, const char *key)
{
    expectType(L, val, LUA_TTABLE, key, "a table");
    lua_pushnil(L);
    while (lua_next(L, val) != 0)
    {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "option '%s': keys must be strings", key);
        setField(L, g, lua_tostring(L, -2), lua_gettop(L));
        lua_pop(L, 1);
    }
}

// Write the value at absolute stack index `val` into field `key`. Raises a Lua
// error if invalid.
void
setField(lua_State *L, const Group *g, const char *key, int val)
{
    const Field *f = findField(g, key);
    if (!f)
        luaL_error(L, "unknown option '%s'", key);

    switch (f->kind)
    {
        case Field::StringList:
        {
            expectType(L, val, LUA_TTABLE, key, "a table of strings");
            std::vector<std::string> tmp;
            lua_pushnil(L);
            while (lua_next(L, val) != 0)
            {
                if (lua_type(L, -1) != LUA_TSTRING)
                    luaL_error(L, "option '%s': all entries must be strings",
                               key);
                tmp.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            if (tmp.empty())
                luaL_error(L, "option '%s' needs at least one name", key);
            *static_cast<std::vector<std::string> *>(f->ptr) = std::move(tmp);
            break;
        }

        case Field::Enum:
        {
            expectType(L, val, LUA_TSTRING, key, "a string");
            const char *s = lua_tostring(L, val);
            for (int i = 0; i < (int)f->enum_names->size(); i++)
                if (std::strcmp(s, f->enum_names->at(i).c_str()) == 0)
                {
                    *static_cast<int *>(f->ptr) = i;
                    goto done;
                }
            {
                // Built with a Lua buffer: luaL_error doesn't return, so no
                // std::string may be alive here.
                luaL_Buffer b;
                luaL_buffinit(L, &b);
                for (size_t i = 0; i < f->enum_names->size(); i++)
                {
                    if (i)
                        luaL_addstring(&b, ", ");
                    luaL_addstring(&b, f->enum_names->at(i).c_str());
                }
                luaL_pushresult(&b);
                luaL_error(L, "option '%s' expects one of: %s", key,
                           lua_tostring(L, -1));
            }
        done:
            break;
        }

        case Field::Float:
        {
            expectType(L, val, LUA_TNUMBER, key, "a number");
            const lua_Number n = lua_tonumber(L, val);
            if (!(n >= f->fmin && n <= f->fmax)) // also rejects NaN
                luaL_error(L, "option '%s' must be between %g and %g", key,
                           static_cast<double>(f->fmin),
                           static_cast<double>(f->fmax));
            *static_cast<float *>(f->ptr) = static_cast<float>(n);
            break;
        }

        case Field::Int:
        {
            expectType(L, val, LUA_TNUMBER, key, "a number");
            const lua_Number n = lua_tonumber(L, val);
            if (!(n == std::floor(n))) // also rejects NaN
                luaL_error(L, "option '%s' expects an integer", key);
            if (n < f->min || n > f->max)
                luaL_error(L, "option '%s' must be between %d and %d", key,
                           f->min, f->max);
            *static_cast<int *>(f->ptr) = static_cast<int>(n);
            break;
        }

        case Field::Bool:
        {
            expectType(L, val, LUA_TBOOLEAN, key, "a boolean");
            *static_cast<bool *>(f->ptr) = lua_toboolean(L, val);
            break;
        }

        case Field::String:
            expectType(L, val, LUA_TSTRING, key, "a string");
            *static_cast<std::string *>(f->ptr) = lua_tostring(L, val);
            break;

        case Field::Color:
        {
            float tmp[4];
            readColor(L, val, key, tmp);
            std::memcpy(f->ptr, tmp, sizeof tmp); // all-or-nothing
            break;
        }

        case Field::Sub:
            mergeTable(L, static_cast<const Group *>(f->ptr), val, key);
            break;

        case Field::Func:
            luaL_error(L, "'%s' is read-only", key);
            break;
    }

    if (f->changes != MuWM::ChangeNone)
        wmFrom(L)->scheduleApply(f->changes);
}

void
pushWindow(lua_State *L, const WindowInfo &w)
{
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, static_cast<lua_Integer>(w.id));
    lua_setfield(L, -2, "id");
    lua_pushstring(L, w.title.c_str());
    lua_setfield(L, -2, "title");
    lua_pushstring(L, w.appId.c_str());
    lua_setfield(L, -2, "app_id");
    lua_pushinteger(L, w.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, w.height);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, w.workspace.c_str());
    lua_setfield(L, -2, "workspace");
    lua_pushstring(L, w.output.c_str());
    lua_setfield(L, -2, "output");
    lua_pushboolean(L, w.focused);
    lua_setfield(L, -2, "focused");
}

// Pushes the handler arguments for an event; returns how many.
int
pushEventData(lua_State *L, const EventData &data)
{
    return std::visit([L](const auto &d) -> int
    {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            return 0;
        else if constexpr (std::is_same_v<T, WindowInfo>)
        {
            pushWindow(L, d);
            return 1;
        }
    }, data);
}

void
pushColor(lua_State *L, const float c[4])
{
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; i++)
    {
        lua_pushnumber(L, c[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

void
pushProxy(lua_State *L, const Group *g);

void
pushField(lua_State *L, const Field *f)
{
    switch (f->kind)
    {
        case Field::StringList:
        {
            lua_createtable(
                L,
                static_cast<int>(
                    static_cast<std::vector<std::string> *>(f->ptr)->size()),
                0);
            int i = 1;
            for (const std::string &s :
                 *static_cast<std::vector<std::string> *>(f->ptr))
            {
                lua_pushstring(L, s.c_str());
                lua_rawseti(L, -2, i++);
            }
            break;
        }

        case Field::Enum:
            lua_pushstring(
                L, f->enum_names->at(*static_cast<int *>(f->ptr)).c_str());
            break;

        case Field::Bool:
            lua_pushboolean(L, *static_cast<bool *>(f->ptr));
            break;

        case Field::Float:
            lua_pushnumber(L, *static_cast<float *>(f->ptr));
            break;

        case Field::String:
            lua_pushstring(L, static_cast<std::string *>(f->ptr)->c_str());
            break;
        case Field::Int:
            lua_pushinteger(L, *static_cast<int *>(f->ptr));
            break;
        case Field::Color:
            pushColor(L, static_cast<float *>(f->ptr));
            break;
        case Field::Sub:
            pushProxy(L, static_cast<const Group *>(f->ptr));
            break;
        case Field::Func:
            lua_pushcfunction(L, f->fn);
            break;
    }
}

int
proxyIndex(lua_State *L)
{
    const Field *f = lua_type(L, 2) == LUA_TSTRING
                         ? findField(groupFrom(L), lua_tostring(L, 2))
                         : nullptr;
    if (f)
        pushField(L, f);
    else
        lua_pushnil(L); // reading an unknown key gives nil
    return 1;
}

int
proxyNewIndex(lua_State *L)
{
    if (lua_type(L, 2) != LUA_TSTRING)
        return luaL_error(L, "option names must be strings");
    setField(L, groupFrom(L), lua_tostring(L, 2), 3);
    return 0;
}

void
pushProxy(lua_State *L, const Group *g)
{
    const std::pair<const char *, lua_CFunction> metamethods[] = {
        {"__index", proxyIndex},
        {"__newindex", proxyNewIndex},
    };

    lua_newtable(L); // the proxy
    lua_newtable(L); // its metatable
    for (auto [name, fn] : metamethods)
    {
        lua_pushlightuserdata(L, const_cast<Group *>(g));
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    lua_setmetatable(L, -2);
}

wlr_direction
checkDirection(lua_State *L, int idx)
{
    const char *d = luaL_checkstring(L, idx);
    if (!strcmp(d, "up"))
        return WLR_DIRECTION_UP;
    if (!strcmp(d, "down"))
        return WLR_DIRECTION_DOWN;
    if (!strcmp(d, "left"))
        return WLR_DIRECTION_LEFT;
    if (!strcmp(d, "right"))
        return WLR_DIRECTION_RIGHT;
    luaL_error(L, "invalid direction '%s'", d);
    return WLR_DIRECTION_LEFT; // not reached
}

std::optional<Binding>
parseBinding(std::string_view s)
{
    Binding b;
    while (true)
    {
        size_t plus = s.find('+');
        std::string part(s.substr(0, plus));

        if (plus != std::string_view::npos)
        { // everything before the last '+' is a modifier
            if (part == "Shift")
                b.mods |= WLR_MODIFIER_SHIFT;
            else if (part == "Ctrl")
                b.mods |= WLR_MODIFIER_CTRL;
            else if (part == "Alt")
                b.mods |= WLR_MODIFIER_ALT;
            else if (part == "Super")
                b.mods |= WLR_MODIFIER_LOGO;
            else
                return std::nullopt; // unknown modifier
            s.remove_prefix(plus + 1);
            continue;
        }

        xkb_keysym_t sym
            = xkb_keysym_from_name(part.c_str(), XKB_KEYSYM_NO_FLAGS);
        if (sym == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(part.c_str(),
                                       XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym == XKB_KEY_NoSymbol)
            return std::nullopt; // unknown key
        b.sym = xkb_keysym_to_lower(sym);
        return b;
    }
}

// clang-format off
/*lua
  ---Bind a key to a Lua function
  ---@param spec string The key binding specification, e.g. "Ctrl+Alt+T"
  ---@param handler fun() The Lua function to call when the key is pressed
  function mu.keybind(spec, handler) end
*/
// clang-format on
int
keyBind(lua_State *L)
{
    const char *spec = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    std::optional<Binding> parsed = parseBinding(spec);
    if (!parsed)
        return luaL_error(L, "invalid key binding '%s'", spec);

    lua_pushvalue(L, 2);
    parsed->ref
        = luaL_ref(L, LUA_REGISTRYINDEX); // pops the copy, returns its ref
    wmFrom(L)->addBinding(*parsed);
    return 0;
}

// clang-format off
/*lua
  ---Spawn a shell command
  ---@param cmd string The command to run
  function mu.spawn(cmd) end
*/
// clang-format on
int
luaSpawn(lua_State *L)
{
    wmFrom(L)->spawn(luaL_checkstring(L, 1));
    return 0;
}

// clang-format off
/*lua
    ---Focus the next output in the given direction
    ---@param dir string One of "up", "down", "left", "right"
    function mu.focus_output(dir) end
*/
int
luaFocusOutput(lua_State *L)
{
    wlr_direction dir = checkDirection(L, 1);
    wmFrom(L)->focusOutput(dir);
    return 0;
}

/*lua
  ---Move the focused window to the output in the given direction
  ---@param dir string One of "up", "down", "left", "right"
  function mu.move_to_output(dir) end
*/
int
luaMoveToOutput(lua_State *L)
{
    wlr_direction dir = checkDirection(L, 1);
    wmFrom(L)->moveToOutput(dir);
    return 0;
}

// clang-format off
/*lua
  ---Focus a workspace by index
  ---@param index integer|string The workspace index (1-based) or name
  function mu.focus_workspace(index) end
*/
// clang-format on
int
luaFocusWorkspace(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TNUMBER)
    {
        const int index = static_cast<int>(luaL_checkinteger(L, 1));
        wmFrom(L)->focusWorkspace(index);
    }
    else if (lua_type(L, 1) == LUA_TSTRING)
    {
        const char *name = lua_tostring(L, 1);
        wmFrom(L)->focusWorkspace(name);
    }
    else
        luaL_error(L, "mu.focus_workspace expects an integer or string");
    return 0;
}

// clang-format off
/*lua
  ---Set the ratio of the master area to the whole screen
  ---@param ratio number The ratio, between 0 and 1
  function mu.set_master_ratio(ratio) end
*/
// clang-format on
int
luaSetMasterRatio(lua_State *L)
{
    auto ratio = static_cast<float>(luaL_checknumber(L, 1));
    wmFrom(L)->setMasterRatio(ratio);
    return 0;
}

// clang-format off
/*lua
  ---Move the focused window to a workspace by index or name
  ---@param index integer|string The workspace index (1-based) or name
  function mu.window_to_workspace(index) end
*/
// clang-format on
int
luaWindowToWorkspace(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TNUMBER)
    {
        const int index = static_cast<int>(luaL_checkinteger(L, 1));
        wmFrom(L)->windowToWorkspace(index);
    }
    else if (lua_type(L, 1) == LUA_TSTRING)
        wmFrom(L)->windowToWorkspace(lua_tostring(L, 1));
    else
        return luaL_error(L, "mu.window_to_workspace expects an integer or string");
    return 0;
}

// Lua function that calls a no-argument MuWM method.
template <void (MuWM::*Method)()>
int
luaCall(lua_State *L)
{
    (wmFrom(L)->*Method)();
    return 0;
}

// Releases a Lua registry ref when the last copy of its handler is destroyed.
struct LuaRef
{
    lua_State *L;
    int ref;

    LuaRef(lua_State *L, int ref) : L(L), ref(ref) {}
    LuaRef(const LuaRef &) = delete;
    ~LuaRef()
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
};

// clang-format off
/*lua
  ---A color: "#rrggbb", "#rrggbbaa" or a table {r, g, b, a} (0-1 each).
  ---@alias mu.Color string|number[]

  ---Where a new window is inserted: after or before the focused window, or
  ---at the top (master) or bottom of the layout.
  ---@alias mu.NewClientPosition "after_current"|"before_current"|"top"|"bottom"

  ---Names accepted by mu.on and mu.once.
  ---@alias mu.Event
  ---| "startup" # The compositor is up (fires once, not on reload).
  ---| "config_reload" # The config was reloaded successfully.
  ---| "window_open" # A window appeared.
  ---| "window_close" # A window went away; the table is a snapshot.

  ---Table passed to window event handlers.
  ---@class mu.Window
  ---@field id integer Stable id of the window.
  ---@field title string Window title; may still be empty when it opens.
  ---@field app_id string Application id (the class), e.g. "foot".
  ---@field width integer Current surface width in pixels.
  ---@field height integer Current surface height in pixels.
  ---@field workspace string Name of the workspace the window is on.
  ---@field output string Name of the output the window is on.
  ---@field focused boolean Whether the window had focus.
*/
// clang-format on
struct EventName
{
    const char *name;
    EventType type;
};

constexpr EventName kEventNames[] = {
    {"startup", EventType::Startup},
    {"config_reload", EventType::ConfigReload},
    {"window_open", EventType::WindowOpen},
    {"window_close", EventType::WindowClose},
    // TODO: Add the rest
    // {"window_fullscreen", EventType::WindowFullscreen},
    // {"window_focus", EventType::WindowFocus},
    // {"window_unfocus", EventType::WindowUnfocus},
    // {"window_move", EventType::WindowMove},
    // {"window_resize", EventType::WindowResize},
    // {"window_minimize", EventType::WindowMinimize},
    // {"window_maximize", EventType::WindowMaximize},
    // {"workspace_change", EventType::WorkspaceChange},
    // {"workspace_add", EventType::WorkspaceAdd},
    // {"workspace_remove", EventType::WorkspaceRemove},
    // {"workspace_rename", EventType::WorkspaceRename},
    // {"workspace_reorder", EventType::WorkspaceReorder},
    // {"workspace_move_window", EventType::WorkspaceMoveWindow},
};

std::expected<EventType, const char *>
checkEventType(lua_State *L, int arg)
{
    const char *name = luaL_checkstring(L, arg);
    for (const EventName &e : kEventNames)
        if (std::strcmp(name, e.name) == 0)
            return e.type;
    luaL_error(L, "unknown event '%s'", name);

    return std::unexpected("unreachable");
}

int
traceback(lua_State *L)
{
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// Handlers always run on the main thread, even if registered from a coroutine.
lua_State *
mainThread(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State *main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main;
}

void
callRef(lua_State *L, int ref, const EventData &data)
{
    lua_pushcfunction(L, traceback);
    const int msgh = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    const int nargs = pushEventData(L, data);
    if (lua_pcall(L, nargs, 0, msgh) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "event handler: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // the traceback function
}

// clang-format off
/*lua
  ---Register an event handler
  ---@param event mu.Event The event name
  ---@param handler fun(window?: mu.Window) The handler function; window events pass a table describing the window, startup and config_reload pass nothing
  ---@return integer id The handler ID, which can be used to remove it with mu.off(id)
  function mu.on(event, handler) end

  ---Register a one-time event handler
  ---@param event mu.Event The event name
  ---@param handler fun(window?: mu.Window) The handler function; window events pass a table describing the window, startup and config_reload pass nothing
  ---@return integer id The handler ID, which can be used to remove it with mu.off(id)
  function mu.once(event, handler) end
*/
// clang-format on
template <bool Once>
int
luaOn(lua_State *L)
{
    auto eventValue = checkEventType(L, 1);
    if (!eventValue)
        return luaL_error(L, "%s", eventValue.error());

    const EventType type = *eventValue;

    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    auto ref = std::make_shared<LuaRef>(mainThread(L),
                                        luaL_ref(L, LUA_REGISTRYINDEX));

    const int id = wmFrom(L)->eventBus().add(type, [ref](const EventData &data)
    { callRef(ref->L, ref->ref, data); }, Once);
    lua_pushinteger(L, id);
    return 1;
}

// clang-format off
/*lua
  ---Remove an event handler by ID
  ---@param id integer The handler ID returned by mu.on or mu.once
  ---@return boolean removed Whether a handler was removed
  function mu.off(id) end
*/
// clang-format on
int
luaOff(lua_State *L)
{
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, wmFrom(L)->eventBus().remove(id));
    return 1;
}

static const std::vector<std::string> kNewClientPositionNames = {
    "after_current",
    "before_current",
    "top",
    "bottom",
};

} // namespace

void
MuWM::initLua()
{
    m_lua = luaL_newstate();
    if (!m_lua)
    {
        wlr_log(WLR_ERROR, "Failed to create Lua state");
        return;
    }

    luaL_openlibs(m_lua);

    initLuaBindings();
}

// Runs the user's config. Must happen after the socket exists and
// WAYLAND_DISPLAY is set, so that muwm.spawn() clients can connect.
void
MuWM::loadConfig()
{
    if (!m_lua)
    {
        installDefaultBindings();
        return;
    }

    const auto path = configFilePath();
    if (path.empty() || !std::filesystem::exists(path))
    {
        wlr_log(WLR_INFO, "No config found; using the default keybindings");
        clearBindings();
        installDefaultBindings();
        return;
    }

    // Compile first: a syntax error must not cost the user their old bindings.
    if (luaL_loadfile(m_lua, path.c_str()) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "Failed to load Lua config: %s",
                lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        failConfig();
        return;
    }

    // On reload, bindings the new config no longer sets must go away.
    clearBindings();
    installDefaultBindings();
    m_eventBus.clear();

    if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "Failed to run Lua config: %s",
                lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        failConfig();
        return;
    }
    wlr_log(WLR_INFO, "Lua config loaded from %s", path.c_str());
    m_eventBus.emit(EventType::ConfigReload);
}

void
MuWM::initLuaBindings()
{
    // The proxies keep raw pointers to these, so they must outlive this
    // function (locals would dangle as soon as it returned).
    static Group rootGroup, optsGroup, borderGroup, keyboardGroup,
        touchpadGroup, gapsGroup, layoutGroup;

    auto &kb = m_config.keyboard;
    auto &bd = m_config.border;

    // lua ---@class mu.opts.border
    borderGroup.fields = {
        // lua ---@field width integer Border width in pixels. Must be between 0
        // and 1000.
        intField("width", &bd.width, 0, 1000, MuWM::ChangeLayout),

        // lua ---@field focused mu.Color Border color for the focused window.
        colorField("focused", bd.focused_color, MuWM::ChangeRecolor),

        // lua ---@field unfocused mu.Color Border color for unfocused windows.
        colorField("unfocused", bd.unfocused_color, MuWM::ChangeRecolor),

        // lua ---@field smart boolean Whether to hide borders when only one
        // window is visible.
        boolField("smart", &bd.smart, MuWM::ChangeLayout),

        // lua ---@field fullscreen boolean Whether to draw a border around
        // fullscreen windows.
        boolField("fullscreen", &bd.fullscreen, MuWM::ChangeLayout),
    };

    // lua ---@class mu.opts.layout
    layoutGroup.fields = {
        // lua ---@field master_ratio number The ratio of the master area to the
        // whole screen. Must be between 0 and 1.
        floatField("master_ratio", &m_config.layout.master_ratio,
                   MuWM::ChangeLayout, 0.0f, 1.0f),

    };

    // lua ---@class mu.opts.keyboard
    keyboardGroup.fields = {
        // lua ---@field rules string XKB rules file name.
        stringField("rules", &kb.rules, MuWM::ChangeKeyboard),

        // lua ---@field model string XKB model name.
        stringField("model", &kb.model, MuWM::ChangeKeyboard),

        // lua ---@field layout string XKB layout name.
        stringField("layout", &kb.layout, MuWM::ChangeKeyboard),

        // lua ---@field variant string XKB variant name.
        stringField("variant", &kb.variant, MuWM::ChangeKeyboard),

        // lua ---@field xkb_options string XKB options string, e.g.
        // "ctrl:nocaps".
        stringField("xkb_options", &kb.xkb_options, MuWM::ChangeKeyboard),

        // lua ---@field repeat_rate integer Keyboard repeat rate in characters
        // per second.
        intField("repeat_rate", &kb.repeat_rate, INT_MIN, INT_MAX,
                 MuWM::ChangeKeyboard),

        // lua ---@field repeat_delay integer Keyboard repeat delay in
        // milliseconds.
        intField("repeat_delay", &kb.repeat_delay, INT_MIN, INT_MAX,
                 MuWM::ChangeKeyboard),
    };

    // lua ---@class mu.opts.gaps
    gapsGroup.fields = {
        // lua ---@field value integer Window gap in pixels.
        intField("value", &m_config.gaps.value, 0, 1000, MuWM::ChangeLayout),

        // lua ---@field smart boolean Whether to hide gaps when only one window
        // is visible.
        boolField("smart", &m_config.gaps.smart, MuWM::ChangeLayout),
    };

    // clang-format off
    // lua ---@class mu.opts
    optsGroup.fields = {
        // lua ---@field keyboard mu.opts.keyboard Keyboard layout options.
        subField("keyboard", &keyboardGroup),

        // lua ---@field touchpad mu.opts.touchpad Touchpad options.
        subField("touchpad", &touchpadGroup),

        // lua ---@field gaps mu.opts.gaps Window gap options.
        subField("gaps", &gapsGroup),

        // lua ---@field border mu.opts.border Window border options.
        subField("border", &borderGroup),

        // lua ---@field layout mu.opts.layout Layout options.
        subField("layout", &layoutGroup),

        // lua ---@field new_client_position mu.NewClientPosition Where a new
        // window is inserted in the layout.
        enumField("new_client_position", &m_config.new_client_position,
                  &kNewClientPositionNames, MuWM::ChangeLayout),

        // lua ---@field workspaces string[] Workspace names; their number is the
        // workspace count.
        stringListField("workspaces", &m_config.workspaces,
                        MuWM::ChangeWorkspace),

    };
    // clang-format on

    // lua ---@class mu.opts.touchpad
    touchpadGroup.fields = {
        // lua ---@field natural_scroll boolean Whether to enable natural
        // scrolling on the touchpad.
        boolField("natural_scroll", &m_config.touchpad.natural_scroll,
                  MuWM::ChangePointer),

        // lua ---@field tap_to_click boolean Whether to enable tap-to-click on
        // the touchpad.
        boolField("tap_to_click", &m_config.touchpad.tap_to_click,
                  MuWM::ChangePointer),
    };

    // lua ---@class mu
    rootGroup.fields = {

        // lua ---@field opts mu.opts User-configurable options.
        subField("opts", &optsGroup),

        // lua ---@field toggle_fullscreen fun() Toggle fullscreen for the
        // focused window.
        funcField("toggle_fullscreen", luaCall<&MuWM::toggleFullscreen>),

        // lua ---@field toggle_floating fun() Toggle floating for the focused
        // window.
        funcField("toggle_floating", luaCall<&MuWM::toggleFloating>),

        // lua ---@field focus_next fun() Focus the next window.
        funcField("focus_next", luaCall<&MuWM::focusNext>),

        // lua ---@field focus_prev fun() Focus the previous window.
        funcField("focus_prev", luaCall<&MuWM::focusPrev>),

        funcField("focus_output", luaFocusOutput),

        funcField("move_to_output", luaMoveToOutput),

        // lua ---@field swap_next fun() Swap the focused window with the next
        // window.
        funcField("swap_next", luaCall<&MuWM::swapNext>),

        // lua ---@field swap_prev fun() Swap the focused window with the
        // previous window.
        funcField("swap_prev", luaCall<&MuWM::swapPrev>),

        // lua ---@field swap_master fun() Swap the focused window with the
        // master window.
        funcField("swap_master", luaCall<&MuWM::swapMaster>),

        // lua ---@field workspace_next fun() Switch to the next workspace.
        funcField("workspace_next", luaCall<&MuWM::workspaceNext>),

        // lua ---@field workspace_prev fun() Switch to the previous workspace.
        funcField("workspace_prev", luaCall<&MuWM::workspacePrev>),

        // lua ---@field close fun() Close the focused window.
        funcField("close", luaCall<&MuWM::close>),

        // lua ---@field quit fun() Quit the window manager.
        funcField("quit", luaCall<&MuWM::quit>),

        // lua ---@field reload fun() Reload the config file.
        funcField("reload", luaCall<&MuWM::loadConfig>),

        funcField("set_master_ratio", luaSetMasterRatio),
        funcField("focus_workspace", luaFocusWorkspace),
        funcField("window_to_workspace", luaWindowToWorkspace),
        funcField("spawn", luaSpawn),
        funcField("keybind", &keyBind),
        funcField("on", luaOn<false>),
        funcField("once", luaOn<true>),
        funcField("off", luaOff),
    };

    lua_pushlightuserdata(m_lua, this);
    lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kWmKey);

    pushProxy(m_lua, &rootGroup);
    lua_setglobal(m_lua, "mu");
}
