// Lua API: the `mu` global table, its `mu.config` option proxies, the
// window / workspace / output handles, and loading of the user's init.lua.

#include "MuWM.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
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
        Func,
        Getter // read-only value computed on access by `fn`
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

// `fn` takes no arguments and pushes one value.
Field
getterField(const char *name, lua_CFunction fn)
{
    return {Field::Getter, name, nullptr, fn};
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
        case Field::Getter:
            luaL_error(L, "'%s' is read-only", key);
            break;
    }

    if (f->changes != MuWM::ChangeNone)
        wmFrom(L)->scheduleApply(f->changes);
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
        case Field::Getter:
            lua_pushcfunction(L, f->fn);
            lua_call(L, 0, 1);
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

// mu.config { ... } merges a table of options in one call.
int
proxyCall(lua_State *L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    mergeTable(L, groupFrom(L), 2, "config");
    return 0;
}

void
pushProxy(lua_State *L, const Group *g)
{
    const std::pair<const char *, lua_CFunction> metamethods[] = {
        {"__index", proxyIndex},
        {"__newindex", proxyNewIndex},
        {"__call", proxyCall},
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

// ---------------------------------------------------------------------------
// Handles: windows, workspaces and outputs as Lua objects.
//
// A handle is a userdata holding the object's id (an output's name) and a
// snapshot of its state. Reads go to the live object while it exists and to the
// snapshot after it is gone, so a `window.close` handler can still see the
// title. Writes to an object that is gone do nothing.

struct WindowHandle
{
    uint64_t id;
    WindowInfo snap;
};

struct WorkspaceHandle
{
    uint64_t id;
    WorkspaceInfo snap;
};

struct OutputHandle
{
    std::string name;
    OutputInfo snap;
};

void
pushWindow(lua_State *L, const WindowInfo &i)
{
    void *mem = lua_newuserdatauv(L, sizeof(WindowHandle), 0);
    new (mem) WindowHandle{i.id, i};
    luaL_setmetatable(L, "mu.window");
}

void
pushWorkspace(lua_State *L, const WorkspaceInfo &i)
{
    void *mem = lua_newuserdatauv(L, sizeof(WorkspaceHandle), 0);
    new (mem) WorkspaceHandle{i.id, i};
    luaL_setmetatable(L, "mu.workspace");
}

void
pushOutput(lua_State *L, const OutputInfo &i)
{
    void *mem = lua_newuserdatauv(L, sizeof(OutputHandle), 0);
    new (mem) OutputHandle{i.name, i};
    luaL_setmetatable(L, "mu.output");
}

// An optional boolean argument: nullopt when absent or nil, so the config
// option decides.
MuWM::MoveFocus
optBool(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx))
        return std::nullopt;
    luaL_checktype(L, idx, LUA_TBOOLEAN);
    return lua_toboolean(L, idx) != 0;
}

// The object a method was called on; nullptr when it is gone, in which case
// the method must do nothing (never fall back to the focused one).
Toplevel *
selfWindow(lua_State *L)
{
    auto *h = static_cast<WindowHandle *>(luaL_checkudata(L, 1, "mu.window"));
    return wmFrom(L)->toplevelById(h->id);
}

Workspace *
selfWorkspace(lua_State *L)
{
    auto *h
        = static_cast<WorkspaceHandle *>(luaL_checkudata(L, 1, "mu.workspace"));
    return wmFrom(L)->workspaceById(h->id);
}

Output *
selfOutput(lua_State *L)
{
    auto *h = static_cast<OutputHandle *>(luaL_checkudata(L, 1, "mu.output"));
    return wmFrom(L)->outputByName(h->name);
}

// An output given as a handle or by name; nullptr if there is no such output.
Output *
outputArg(lua_State *L, int idx)
{
    MuWM *wm = wmFrom(L);
    if (lua_type(L, idx) == LUA_TSTRING)
        return wm->outputByName(lua_tostring(L, idx));
    if (auto *h
        = static_cast<OutputHandle *>(luaL_testudata(L, idx, "mu.output")))
        return wm->outputByName(h->name);
    luaL_error(L, "expected an output or an output name");
    return nullptr;
}

// A workspace given as a handle, a 1-based index or a name. Indexes and names
// are looked up on `context`; nullptr if there is no such workspace.
Workspace *
workspaceArg(lua_State *L, int idx, Output *context)
{
    MuWM *wm = wmFrom(L);
    if (auto *h = static_cast<WorkspaceHandle *>(
            luaL_testudata(L, idx, "mu.workspace")))
        return wm->workspaceById(h->id);
    if (lua_type(L, idx) == LUA_TNUMBER)
    {
        const lua_Integer n = luaL_checkinteger(L, idx);
        const auto count
            = static_cast<lua_Integer>(wm->config().workspaces.size());
        if (!context || n < 1 || n > count)
            return nullptr;
        return wm->workspaceAt(context, static_cast<size_t>(n - 1));
    }
    if (lua_type(L, idx) == LUA_TSTRING)
        return context ? wm->workspaceNamed(context, lua_tostring(L, idx))
                       : nullptr;
    luaL_error(L, "expected a workspace, its index or its name");
    return nullptr;
}

// A property of a handle. `get` reads it from the snapshot-or-live info, `set`
// writes it to the live object (null: read-only), `method` is a function
// (then get and set are null).
template <class Info, class Obj> struct Prop
{
    const char *name;
    void (*get)(lua_State *, MuWM *, const Info &, bool live);
    void (*set)(lua_State *, MuWM *, Obj *, int val);
    lua_CFunction method;
};

template <class P, size_t N>
const P *
findProp(const P (&props)[N], const char *key)
{
    for (const P &p : props)
        if (std::strcmp(p.name, key) == 0)
            return &p;
    return nullptr;
}

using WindowProp    = Prop<WindowInfo, Toplevel>;
using WorkspaceProp = Prop<WorkspaceInfo, Workspace>;
using OutputProp    = Prop<OutputInfo, Output>;

int
windowMoveToWorkspace(lua_State *L)
{
    Toplevel *t = selfWindow(L);
    MuWM *wm    = wmFrom(L);
    Output *ctx
        = t && t->workspace ? t->workspace->output : wm->focusedOutput();
    Workspace *ws               = workspaceArg(L, 2, ctx);
    const MuWM::MoveFocus focus = optBool(L, 3);
    if (t && ws)
        wm->windowToWorkspace(t, ws, focus);
    return 0;
}

// clang-format off
const WindowProp kWindowProps[] = {
    {"id",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushinteger(L, static_cast<lua_Integer>(i.id)); },
     nullptr, nullptr},
    {"title",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushstring(L, i.title.c_str()); },
     nullptr, nullptr},
    {"app_id",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushstring(L, i.appId.c_str()); },
     nullptr, nullptr},
    {"workspace",
     [](lua_State *L, MuWM *wm, const WindowInfo &i, bool)
     {
         if (Workspace *ws = wm->workspaceById(i.workspaceId))
             pushWorkspace(L, wm->workspaceInfo(ws));
         else
             lua_pushnil(L);
     },
     [](lua_State *L, MuWM *wm, Toplevel *t, int val)
     {
         Output *ctx
             = t->workspace ? t->workspace->output : wm->focusedOutput();
         if (Workspace *ws = workspaceArg(L, val, ctx))
             wm->windowToWorkspace(t, ws);
     },
     nullptr},
    {"output",
     [](lua_State *L, MuWM *wm, const WindowInfo &i, bool)
     {
         if (Output *o = wm->outputByName(i.output))
             pushOutput(L, wm->outputInfo(o));
         else
             lua_pushnil(L);
     },
     [](lua_State *L, MuWM *wm, Toplevel *t, int val)
     {
         if (Output *o = outputArg(L, val))
             wm->moveToOutput(t, o);
     },
     nullptr},
    {"floating",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushboolean(L, i.floating); },
     [](lua_State *L, MuWM *wm, Toplevel *t, int val)
     {
         luaL_checktype(L, val, LUA_TBOOLEAN);
         if ((lua_toboolean(L, val) != 0) != t->floating)
             wm->toggleFloating(t);
     },
     nullptr},
    {"fullscreen",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushboolean(L, i.fullscreen); },
     [](lua_State *L, MuWM *wm, Toplevel *t, int val)
     {
         luaL_checktype(L, val, LUA_TBOOLEAN);
         wm->setFullscreen(t, lua_toboolean(L, val) != 0);
     },
     nullptr},
    {"focused",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushboolean(L, i.focused); },
     [](lua_State *L, MuWM *wm, Toplevel *t, int val)
     {
         luaL_checktype(L, val, LUA_TBOOLEAN);
         if (lua_toboolean(L, val))
             wm->activateToplevel(t);
     },
     nullptr},
    {"x",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushinteger(L, i.x); },
     nullptr, nullptr},
    {"y",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushinteger(L, i.y); },
     nullptr, nullptr},
    {"width",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushinteger(L, i.width); },
     nullptr, nullptr},
    {"height",
     [](lua_State *L, MuWM *, const WindowInfo &i, bool)
     { lua_pushinteger(L, i.height); },
     nullptr, nullptr},
    {"valid",
     [](lua_State *L, MuWM *, const WindowInfo &, bool live)
     { lua_pushboolean(L, live); },
     nullptr, nullptr},
    {"close", nullptr, nullptr,
     [](lua_State *L) -> int
     {
         wmFrom(L)->close(selfWindow(L));
         return 0;
     }},
    {"focus", nullptr, nullptr,
     [](lua_State *L) -> int
     {
         wmFrom(L)->activateToplevel(selfWindow(L));
         return 0;
     }},
    {"move_to_workspace", nullptr, nullptr, windowMoveToWorkspace},
    {"tag",
        [](lua_State *L, MuWM *, const WindowInfo &i, bool)
        {
            lua_pushstring(L, i.tag.c_str());
        }, nullptr, nullptr},
    {"tag_description",
        [](lua_State *L, MuWM *, const WindowInfo &i, bool)
        {
            lua_pushstring(L, i.tagDescription.c_str());
        }, nullptr, nullptr},
};

const WorkspaceProp kWorkspaceProps[] = {
    {"id",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushinteger(L, static_cast<lua_Integer>(i.id)); },
     nullptr, nullptr},
    {"name",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushstring(L, i.name.c_str()); },
     nullptr, nullptr},
    {"index",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushinteger(L, i.index); },
     nullptr, nullptr},
    {"output",
     [](lua_State *L, MuWM *wm, const WorkspaceInfo &i, bool)
     {
         if (Output *o = wm->outputByName(i.output))
             pushOutput(L, wm->outputInfo(o));
         else
             lua_pushnil(L);
     },
     nullptr, nullptr},
    {"windows",
     [](lua_State *L, MuWM *wm, const WorkspaceInfo &i, bool)
     {
         lua_newtable(L);
         Workspace *ws = wm->workspaceById(i.id);
         if (!ws)
             return;
         lua_Integer n = 0;
         for (Toplevel *t : ws->toplevels)
         {
             pushWindow(L, wm->windowInfo(t));
             lua_rawseti(L, -2, ++n);
         }
     },
     nullptr, nullptr},
    {"visible",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushboolean(L, i.visible); },
     nullptr, nullptr},
    {"urgent",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushboolean(L, i.urgent); },
     nullptr, nullptr},
    {"master_ratio",
     [](lua_State *L, MuWM *, const WorkspaceInfo &i, bool)
     { lua_pushnumber(L, i.masterRatio); },
     [](lua_State *L, MuWM *wm, Workspace *ws, int val)
     {
         const lua_Number r = luaL_checknumber(L, val);
         if (!(r >= 0.0 && r <= 1.0))
             luaL_error(L, "master_ratio must be between 0 and 1");
         wm->setMasterRatio(ws, static_cast<float>(r));
     },
     nullptr},
    {"valid",
     [](lua_State *L, MuWM *, const WorkspaceInfo &, bool live)
     { lua_pushboolean(L, live); },
     nullptr, nullptr},
    {"show", nullptr, nullptr,
     [](lua_State *L) -> int
     {
         if (Workspace *ws = selfWorkspace(L); ws && ws->output)
             wmFrom(L)->switchWorkspace(ws->output, ws);
         return 0;
     }},
};

const OutputProp kOutputProps[] = {
    {"name",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushstring(L, i.name.c_str()); },
     nullptr, nullptr},
    {"description",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushstring(L, i.description.c_str()); },
     nullptr, nullptr},
    {"x",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushinteger(L, i.x); },
     nullptr, nullptr},
    {"y",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushinteger(L, i.y); },
     nullptr, nullptr},
    {"width",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushinteger(L, i.width); },
     nullptr, nullptr},
    {"height",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushinteger(L, i.height); },
     nullptr, nullptr},
    {"scale",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushnumber(L, i.scale); },
     nullptr, nullptr},
    {"focused",
     [](lua_State *L, MuWM *, const OutputInfo &i, bool)
     { lua_pushboolean(L, i.focused); },
     nullptr, nullptr},
    {"workspace",
     [](lua_State *L, MuWM *wm, const OutputInfo &i, bool)
     {
         if (Workspace *ws = wm->workspaceById(i.workspaceId))
             pushWorkspace(L, wm->workspaceInfo(ws));
         else
             lua_pushnil(L);
     },
     [](lua_State *L, MuWM *wm, Output *o, int val)
     {
         Workspace *ws = workspaceArg(L, val, o);
         if (ws && ws->output == o)
             wm->switchWorkspace(o, ws);
     },
     nullptr},
    {"valid",
     [](lua_State *L, MuWM *, const OutputInfo &, bool live)
     { lua_pushboolean(L, live); },
     nullptr, nullptr},
    {"focus", nullptr, nullptr,
     [](lua_State *L) -> int
     {
         wmFrom(L)->focusOutput(selfOutput(L));
         return 0;
     }},
    {"neighbor", nullptr, nullptr,
     [](lua_State *L) -> int
     {
         Output *o = selfOutput(L);
         const wlr_direction dir = checkDirection(L, 2);
         MuWM *wm = wmFrom(L);
         if (Output *n = o ? wm->adjacentOutput(o, dir) : nullptr)
             pushOutput(L, wm->outputInfo(n));
         else
             lua_pushnil(L);
         return 1;
     }},
};
// clang-format on

// What the shared metamethods need to know about each kind of handle.
struct WindowKind
{
    using Handle                    = WindowHandle;
    using Info                      = WindowInfo;
    using Obj                       = Toplevel;
    static constexpr const char *mt = "mu.window";
    static const WindowProp *find(const char *key)
    {
        return findProp(kWindowProps, key);
    }
    static Obj *resolve(MuWM *wm, const Handle &h)
    {
        return wm->toplevelById(h.id);
    }
    static Info info(MuWM *wm, const Obj *o)
    {
        return wm->windowInfo(o);
    }
    static bool same(const Handle &a, const Handle &b)
    {
        return a.id == b.id;
    }
    static void describe(lua_State *L, const Handle &h)
    {
        lua_pushfstring(L, "mu.window(%I)", static_cast<lua_Integer>(h.id));
    }
};

struct WorkspaceKind
{
    using Handle                    = WorkspaceHandle;
    using Info                      = WorkspaceInfo;
    using Obj                       = Workspace;
    static constexpr const char *mt = "mu.workspace";
    static const WorkspaceProp *find(const char *key)
    {
        return findProp(kWorkspaceProps, key);
    }
    static Obj *resolve(MuWM *wm, const Handle &h)
    {
        return wm->workspaceById(h.id);
    }
    static Info info(MuWM *wm, const Obj *o)
    {
        return wm->workspaceInfo(o);
    }
    static bool same(const Handle &a, const Handle &b)
    {
        return a.id == b.id;
    }
    static void describe(lua_State *L, const Handle &h)
    {
        lua_pushfstring(L, "mu.workspace(%s)", h.snap.name.c_str());
    }
};

struct OutputKind
{
    using Handle                    = OutputHandle;
    using Info                      = OutputInfo;
    using Obj                       = Output;
    static constexpr const char *mt = "mu.output";
    static const OutputProp *find(const char *key)
    {
        return findProp(kOutputProps, key);
    }
    static Obj *resolve(MuWM *wm, const Handle &h)
    {
        return wm->outputByName(h.name);
    }
    static Info info(MuWM *wm, const Obj *o)
    {
        return wm->outputInfo(o);
    }
    static bool same(const Handle &a, const Handle &b)
    {
        return a.name == b.name;
    }
    static void describe(lua_State *L, const Handle &h)
    {
        lua_pushfstring(L, "mu.output(%s)", h.name.c_str());
    }
};

template <class K>
int
handleIndex(lua_State *L)
{
    auto *h = static_cast<typename K::Handle *>(luaL_checkudata(L, 1, K::mt));
    const char *key = luaL_checkstring(L, 2);
    const auto *p   = K::find(key);
    if (!p)
        return luaL_error(L, "%s has no property '%s'", K::mt, key);
    if (p->method)
    {
        lua_pushcfunction(L, p->method);
        return 1;
    }

    MuWM *wm              = wmFrom(L);
    auto *obj             = K::resolve(wm, *h);
    typename K::Info info = obj ? K::info(wm, obj) : h->snap;
    p->get(L, wm, info, obj != nullptr);
    return 1;
}

template <class K>
int
handleNewIndex(lua_State *L)
{
    auto *h = static_cast<typename K::Handle *>(luaL_checkudata(L, 1, K::mt));
    const char *key = luaL_checkstring(L, 2);
    const auto *p   = K::find(key);
    if (!p)
        return luaL_error(L, "%s has no property '%s'", K::mt, key);
    if (!p->set)
        return luaL_error(L, "%s.%s is read-only", K::mt, key);

    MuWM *wm  = wmFrom(L);
    auto *obj = K::resolve(wm, *h);
    if (obj) // writes to a window that is gone do nothing
        p->set(L, wm, obj, 3);
    return 0;
}

template <class K>
int
handleEq(lua_State *L)
{
    auto *a = static_cast<typename K::Handle *>(luaL_checkudata(L, 1, K::mt));
    auto *b = static_cast<typename K::Handle *>(luaL_testudata(L, 2, K::mt));
    lua_pushboolean(L, b && K::same(*a, *b));
    return 1;
}

template <class K>
int
handleToString(lua_State *L)
{
    K::describe(
        L, *static_cast<typename K::Handle *>(luaL_checkudata(L, 1, K::mt)));
    return 1;
}

template <class K>
int
handleGc(lua_State *L)
{
    using H = typename K::Handle;
    static_cast<H *>(lua_touserdata(L, 1))->~H();
    return 0;
}

template <class K>
void
registerHandle(lua_State *L)
{
    const luaL_Reg metamethods[] = {
        {"__index", handleIndex<K>}, {"__newindex", handleNewIndex<K>},
        {"__eq", handleEq<K>},       {"__tostring", handleToString<K>},
        {"__gc", handleGc<K>},       {nullptr, nullptr},
    };
    luaL_newmetatable(L, K::mt);
    luaL_setfuncs(L, metamethods, 0);
    lua_pop(L, 1);
}

// ---------------------------------------------------------------------------
// mu.window, mu.windows, ...: read on every access, so they are never stale.

int
getWindow(lua_State *L)
{
    MuWM *wm = wmFrom(L);
    if (Toplevel *t = wm->focusedToplevel())
        pushWindow(L, wm->windowInfo(t));
    else
        lua_pushnil(L);
    return 1;
}

int
getWindows(lua_State *L)
{
    MuWM *wm = wmFrom(L);
    lua_newtable(L);
    lua_Integer n = 0;
    for (Toplevel *t : wm->allToplevels())
    {
        pushWindow(L, wm->windowInfo(t));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int
getWorkspace(lua_State *L)
{
    MuWM *wm      = wmFrom(L);
    Workspace *ws = wm->currentWorkspace();
    if (ws && ws->id)
        pushWorkspace(L, wm->workspaceInfo(ws));
    else
        lua_pushnil(L);
    return 1;
}

// mu.workspaces[name] finds a workspace of the focused output by name.
int
workspacesByName(lua_State *L)
{
    MuWM *wm      = wmFrom(L);
    Workspace *ws = nullptr;
    if (lua_type(L, 2) == LUA_TSTRING && wm->focusedOutput())
        ws = wm->workspaceNamed(wm->focusedOutput(), lua_tostring(L, 2));
    if (ws)
        pushWorkspace(L, wm->workspaceInfo(ws));
    else
        lua_pushnil(L);
    return 1;
}

int
getWorkspaces(lua_State *L)
{
    MuWM *wm  = wmFrom(L);
    Output *o = wm->focusedOutput();
    lua_newtable(L);
    if (o)
    {
        const size_t count = wm->config().workspaces.size();
        for (size_t slot = 0; slot < count; slot++)
            if (Workspace *ws = wm->workspaceAt(o, slot))
            {
                pushWorkspace(L, wm->workspaceInfo(ws));
                lua_rawseti(L, -2, static_cast<lua_Integer>(slot) + 1);
            }
    }
    luaL_setmetatable(L, "mu.workspaces");
    return 1;
}

int
getOutput(lua_State *L)
{
    MuWM *wm = wmFrom(L);
    if (Output *o = wm->focusedOutput())
        pushOutput(L, wm->outputInfo(o));
    else
        lua_pushnil(L);
    return 1;
}

// mu.outputs["DP-1"] finds an output by name.
int
outputsByName(lua_State *L)
{
    MuWM *wm  = wmFrom(L);
    Output *o = nullptr;
    if (lua_type(L, 2) == LUA_TSTRING)
        o = wm->outputByName(lua_tostring(L, 2));
    if (o)
        pushOutput(L, wm->outputInfo(o));
    else
        lua_pushnil(L);
    return 1;
}

int
getOutputs(lua_State *L)
{
    MuWM *wm = wmFrom(L);
    lua_newtable(L);
    lua_Integer n = 0;
    for (Output *o : wm->outputs())
    {
        pushOutput(L, wm->outputInfo(o));
        lua_rawseti(L, -2, ++n);
    }
    luaL_setmetatable(L, "mu.outputs");
    return 1;
}

// ---------------------------------------------------------------------------
// Verbs

// clang-format off
/*lua
  ---Bind keys to actions. An action is a Lua function or a shell command (which
  ---is spawned). Call it with one key and an action, or with a table of
  ---`["Super+Return"] = action` entries. Binding a key again replaces the old
  ---action.
  ---@param keys string|table<string, fun()|string> A key like "Super+Shift+q", or a table of keys to actions
  ---@param action? fun()|string The function to call, or the command to spawn; only when `keys` is a string
  function mu.bind(keys, action) end
*/
// clang-format on
void
bindOne(lua_State *L, int spec, int action)
{
    if (lua_type(L, spec) != LUA_TSTRING)
        luaL_error(L, "mu.bind: keys must be strings like \"Super+q\"");
    if (lua_type(L, action) != LUA_TFUNCTION
        && lua_type(L, action) != LUA_TSTRING)
        luaL_error(L, "mu.bind: the action must be a function or a command");

    const char *s                 = lua_tostring(L, spec);
    std::optional<Binding> parsed = parseBinding(s);
    if (!parsed)
        luaL_error(L, "invalid key binding '%s'", s);

    if (lua_type(L, action) == LUA_TFUNCTION)
    {
        lua_pushvalue(L, action);
        // pops the copy, returns its ref
        parsed->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    else
    {
        MuWM *wm        = wmFrom(L);
        std::string cmd = lua_tostring(L, action);
        parsed->native  = [wm, cmd]
        {
            wm->spawn(cmd);
        };
    }
    wmFrom(L)->addBinding(*parsed);
}

int
luaBind(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TTABLE)
    {
        bindOne(L, 1, 2);
        return 0;
    }

    lua_pushnil(L);
    while (lua_next(L, 1) != 0)
    {
        bindOne(L, lua_gettop(L) - 1, lua_gettop(L)); // key, value
        lua_pop(L, 1);
    }
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
  ---Focus the nearest window in a direction, or the next output if there is
  ---none; or cycle through the windows of the workspace.
  ---@param where "up"|"down"|"left"|"right"|"next"|"prev"
  function mu.focus(where) end
*/
// clang-format on
int
luaFocus(lua_State *L)
{
    const char *d = luaL_checkstring(L, 1);
    MuWM *wm      = wmFrom(L);
    if (!std::strcmp(d, "next"))
        wm->focusNext();
    else if (!std::strcmp(d, "prev"))
        wm->focusPrev();
    else
        wm->focus(checkDirection(L, 1));
    return 0;
}

// clang-format off
/*lua
  ---Swap the focused window with a neighbour in the stack, or with the master.
  ---@param with "next"|"prev"|"master"
  function mu.swap(with) end
*/
// clang-format on
int
luaSwap(lua_State *L)
{
    const char *w = luaL_checkstring(L, 1);
    MuWM *wm      = wmFrom(L);
    if (!std::strcmp(w, "next"))
        wm->swapNext();
    else if (!std::strcmp(w, "prev"))
        wm->swapPrev();
    else if (!std::strcmp(w, "master"))
        wm->swapMaster();
    else
        return luaL_error(L,
                          "mu.swap expects \"next\", \"prev\" or \"master\"");
    return 0;
}

// clang-format off
/*lua
  ---Focus the output in the given direction
  ---@param dir "up"|"down"|"left"|"right"
  function mu.focus_output(dir) end

  ---Move the focused window to the output in the given direction
  ---@param dir "up"|"down"|"left"|"right"
  function mu.move_to_output(dir) end
*/
// clang-format on
int
luaFocusOutput(lua_State *L)
{
    wmFrom(L)->focusOutput(checkDirection(L, 1));
    return 0;
}

int
luaMoveToOutput(lua_State *L)
{
    wmFrom(L)->moveToOutput(checkDirection(L, 1));
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

// clang-format off
/*lua
  ---Write a message to the compositor log
  ---@param message string
  function mu.log(message) end
*/
// clang-format on
int
luaLog(lua_State *L)
{
    wlr_log(WLR_INFO, "lua: %s", luaL_checkstring(L, 1));
    return 0;
}

// ---------------------------------------------------------------------------
// Events

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
  ---| "quit" # The compositor is quitting (fires once, not on reload).
  ---| "config.reload" # The config was reloaded successfully.
  ---| "window.open" # A window appeared; its handler gets the window.
  ---| "window.close" # A window went away; its handler gets the (now invalid) window.
  ---| "window.fullscreen" # A window changed its fullscreen state; its handler gets the window.
  ---| "workspace.change" # The focused workspace changed; its handler gets the new workspace


  ---A window. A handle: it stays usable for as long as the window exists, and
  ---afterwards reads give the last known values (see valid) while writes do
  ---nothing. Windows compare equal when they are the same window.
  ---@class mu.Window
  ---@field id integer Stable id of the window.
  ---@field title string Window title; may still be empty when it opens.
  ---@field app_id string Application id (the class), e.g. "foot".
  ---@field workspace mu.Workspace|nil The workspace it is on. Assign a workspace, its index or its name to move the window there.
  ---@field output mu.Output|nil The output it is on. Assign an output or its name to move the window there.
  ---@field floating boolean Whether the window floats. Assignable.
  ---@field fullscreen boolean Whether the window is fullscreen. Assignable.
  ---@field focused boolean Whether the window has focus. Assign true to focus it.
  ---@field x integer Left edge in layout coordinates.
  ---@field y integer Top edge in layout coordinates.
  ---@field width integer Current surface width in pixels.
  ---@field height integer Current surface height in pixels.
  ---@field valid boolean Whether the window still exists.
  ---@field close fun(self: mu.Window) Ask the window to close; does nothing if it is gone.
  ---@field focus fun(self: mu.Window) Focus the window, switching to its workspace first.
  ---@field move_to_workspace fun(self: mu.Window, workspace: mu.Workspace|integer|string, focus?: boolean) Move the window to a workspace. With focus, switch to that workspace and focus the window; without it the window stays where it is on screen. Defaults to mu.config.focus_moved_window.
  ---@field tag string The window's tag, or "" if none.
  ---@field tag_description string The window's tag description, or "" if none.

  ---A workspace of one output, e.g. "3". A handle like mu.Window.
  ---@class mu.Workspace
  ---@field id integer Unique id; names are only unique per output.
  ---@field name string The workspace name.
  ---@field index integer Its 1-based position in mu.config.workspaces.
  ---@field output mu.Output|nil The output it belongs to.
  ---@field windows mu.Window[] The windows on it.
  ---@field visible boolean Whether it is the one shown on its output.
  ---@field urgent boolean Whether a window on it asked for attention.
  ---@field master_ratio number Share of the screen the master area takes, 0 to 1. Assignable.
  ---@field valid boolean Whether the workspace still exists.
  ---@field show fun(self: mu.Workspace) Show it on its output and focus that output.

  ---A monitor. A handle like mu.Window; outputs are identified by name.
  ---@class mu.Output
  ---@field name string The connector name, e.g. "DP-1".
  ---@field description string Make and model, when known.
  ---@field x integer Left edge in layout coordinates.
  ---@field y integer Top edge in layout coordinates.
  ---@field width integer Width in layout pixels.
  ---@field height integer Height in layout pixels.
  ---@field scale number Output scale.
  ---@field focused boolean Whether it is the focused output.
  ---@field workspace mu.Workspace|nil The workspace it shows. Assign a workspace of this output, its index or its name to show it.
  ---@field valid boolean Whether the output is still connected.
  ---@field focus fun(self: mu.Output) Focus the output and move the pointer to it.
  ---@field neighbor fun(self: mu.Output, dir: "up"|"down"|"left"|"right"): mu.Output|nil The output next to this one in a direction.

  ---Returned by mu.on and mu.once.
  ---@class mu.Listener
  ---@field id integer
  ---@field off fun(self: mu.Listener): boolean Remove the handler; returns whether it was still registered.

  ---Returned by mu.after and mu.every.
  ---@class mu.Timer
  ---@field id integer
  ---@field cancel fun(self: mu.Timer): boolean Stop the timer; returns whether it was still pending.
*/
// clang-format on
struct EventName
{
    const char *name;
    EventType type;
};

constexpr EventName kEventNames[] = {
    {"startup", EventType::Startup},
    {"quit", EventType::Quit},
    {"config.reload", EventType::ConfigReload},
    {"window.open", EventType::WindowOpen},
    {"window.close", EventType::WindowClose},
    {"window.fullscreen", EventType::WindowFullscreen},
    {"workspace.change", EventType::WorkspaceChange},
    // TODO: Add the rest
    // {"window.focus", EventType::WindowFocus},
    // {"window.unfocus", EventType::WindowUnfocus},
    // {"window.move", EventType::WindowMove},
    // {"window.resize", EventType::WindowResize},
    // {"window.minimize", EventType::WindowMinimize},
    // {"window.maximize", EventType::WindowMaximize},
    // {"workspace.add", EventType::WorkspaceAdd},
    // {"workspace.remove", EventType::WorkspaceRemove},
    // {"workspace.rename", EventType::WorkspaceRename},
    // {"workspace.reorder", EventType::WorkspaceReorder},
    // {"workspace.move_window", EventType::WorkspaceMoveWindow},
};

EventType
checkEventType(lua_State *L, int arg)
{
    const char *name = luaL_checkstring(L, arg);
    for (const EventName &e : kEventNames)
        if (std::strcmp(name, e.name) == 0)
            return e.type;
    luaL_error(L, "unknown event '%s'", name);
    return EventType::Startup; // not reached
}

// Pushes the handler arguments for an event; returns how many.
int
pushEventData(lua_State *L, const EventData &data)
{
    if (const auto *w = std::get_if<WindowInfo>(&data))
    {
        pushWindow(L, *w);
        return 1;
    }
    return 0;
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
        wlr_log(WLR_ERROR, "lua handler: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // the traceback function
}

// clang-format off
/*lua
  ---Register an event handler. Window events pass the window; startup and
  ---config.reload pass nothing.
  ---@param event mu.Event
  ---@param handler fun(window?: mu.Window)
  ---@return mu.Listener listener Call listener:off() to remove the handler
  function mu.on(event, handler) end

  ---Register a handler that runs once, on the next occurrence of the event
  ---@param event mu.Event
  ---@param handler fun(window?: mu.Window)
  ---@return mu.Listener listener Call listener:off() to remove the handler
  function mu.once(event, handler) end
*/
// clang-format on
template <bool Once>
int
luaOn(lua_State *L)
{
    const EventType type = checkEventType(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    auto ref = std::make_shared<LuaRef>(mainThread(L),
                                        luaL_ref(L, LUA_REGISTRYINDEX));

    const int id = wmFrom(L)->eventBus().add(type, [ref](const EventData &data)
    { callRef(ref->L, ref->ref, data); }, Once);

    lua_createtable(L, 0, 1);
    lua_pushinteger(L, id);
    lua_setfield(L, -2, "id");
    luaL_setmetatable(L, "mu.listener");
    return 1;
}

int
listenerOff(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    const int id = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pushboolean(L, wmFrom(L)->eventBus().remove(id));
    return 1;
}

// ---------------------------------------------------------------------------
// Rules: properties applied to every new window that matches.

char kRulesKey; // registry: array of {match = table, effect = table|function}
char kRuleApplyKey; // registry: the function below, compiled once

// Strings match exactly; use a function for anything fancier. A workspace or
// output is compared by name.
const char *const kRuleApplySource = R"lua(
return function(rules, w)
  for _, rule in ipairs(rules) do
    local ok = true
    for key, want in pairs(rule.match) do
      local have = w[key]
      if type(have) == "userdata" and type(want) == "string" then
        have = have.name
      end
      if type(want) == "function" then
        ok = want(have, w)
      else
        ok = (have == want)
      end
      if not ok then break end
    end
    if ok then
      if type(rule.effect) == "function" then
        rule.effect(w)
      else
        for key, value in pairs(rule.effect) do w[key] = value end
      end
    end
  end
end
)lua";

void
runRules(lua_State *L, const EventData &data)
{
    const auto *info = std::get_if<WindowInfo>(&data);
    if (!info)
        return;

    lua_pushcfunction(L, traceback);
    const int msgh = lua_gettop(L);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRuleApplyKey);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRulesKey);
    pushWindow(L, *info);
    if (lua_pcall(L, 2, 0, msgh) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "window rule: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // the traceback function
}

// clang-format off
/*lua
  ---Apply properties to every new window that matches. All keys of `match`
  ---must match: a string must equal the window's property (workspaces and
  ---outputs are compared by name), a function is called as fn(value, window)
  ---and returns whether it matches. `apply` is a table of properties to
  ---assign (floating, fullscreen, workspace, output), or a function called
  ---with the window; use a function when the order of changes matters.
  ---@param match table<string, string|boolean|number|fun(value: any, window: mu.Window): boolean>
  ---@param apply table<string, any>|fun(window: mu.Window)
  function mu.rule(match, apply) end
*/
// clang-format on
int
luaRule(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_type(L, 2) != LUA_TTABLE && lua_type(L, 2) != LUA_TFUNCTION)
        return luaL_error(
            L, "mu.rule expects a table or a function as its second argument");

    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRulesKey);
    if (lua_isnil(L, -1))
    {
        // The first rule: create the list and start watching new windows.
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &kRulesKey);

        lua_State *mainL = mainThread(L);
        wmFrom(L)->eventBus().add(EventType::WindowOpen,
                                  [mainL](const EventData &data)
        { runRules(mainL, data); });
    }

    lua_createtable(L, 0, 2);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "match");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "effect");
    lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2)) + 1);
    return 0;
}

// ---------------------------------------------------------------------------
// Timers

struct Timer
{
    uint64_t id          = 0;
    int ms               = 0;
    bool repeat          = false;
    wl_event_source *src = nullptr;
    std::shared_ptr<LuaRef> fn;

    Timer()              = default;
    Timer(const Timer &) = delete;
    ~Timer()
    {
        if (src)
            wl_event_source_remove(src);
    }
};

std::vector<std::unique_ptr<Timer>> g_timers;
uint64_t g_nextTimerId = 1;

std::vector<std::unique_ptr<Timer>>::iterator
findTimer(uint64_t id)
{
    return std::ranges::find_if(g_timers,
                                [id](const auto &t) { return t->id == id; });
}

int
onTimer(void *data)
{
    const uint64_t id                = static_cast<Timer *>(data)->id;
    // Keep the handler alive while it runs: it may cancel its own timer.
    const std::shared_ptr<LuaRef> fn = static_cast<Timer *>(data)->fn;
    callRef(fn->L, fn->ref, EventData{});

    auto it = findTimer(id);
    if (it == g_timers.end())
        return 0; // cancelled by the handler
    if ((*it)->repeat)
        wl_event_source_timer_update((*it)->src, (*it)->ms);
    else
        g_timers.erase(it);
    return 0;
}

int
addTimer(lua_State *L, bool repeat)
{
    const lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 1 || ms > INT_MAX)
        return luaL_error(L, "the interval must be between 1 and %d ms",
                          INT_MAX);

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    uint64_t id = 0;
    {
        auto timer    = std::make_unique<Timer>();
        timer->id     = g_nextTimerId++;
        timer->ms     = static_cast<int>(ms);
        timer->repeat = repeat;
        timer->fn     = std::make_shared<LuaRef>(mainThread(L), ref);
        timer->src    = wl_event_loop_add_timer(wmFrom(L)->eventLoop(), onTimer,
                                                timer.get());
        if (timer->src)
        {
            wl_event_source_timer_update(timer->src, timer->ms);
            id = timer->id;
            g_timers.push_back(std::move(timer));
        }
    }
    if (!id)
        return luaL_error(L, "could not create a timer");

    lua_createtable(L, 0, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    lua_setfield(L, -2, "id");
    luaL_setmetatable(L, "mu.timer");
    return 1;
}

// clang-format off
/*lua
  ---Call a function once after a delay
  ---@param ms integer Delay in milliseconds, at least 1
  ---@param fn fun()
  ---@return mu.Timer timer
  function mu.after(ms, fn) end

  ---Call a function repeatedly
  ---@param ms integer Interval in milliseconds, at least 1
  ---@param fn fun()
  ---@return mu.Timer timer
  function mu.every(ms, fn) end
*/
// clang-format on
int
luaAfter(lua_State *L)
{
    return addTimer(L, false);
}

int
luaEvery(lua_State *L)
{
    return addTimer(L, true);
}

int
timerCancel(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "id");
    const auto id    = static_cast<uint64_t>(luaL_checkinteger(L, -1));
    auto it          = findTimer(id);
    const bool found = it != g_timers.end();
    if (found)
        g_timers.erase(it);
    lua_pushboolean(L, found);
    return 1;
}

// A metatable whose __index holds a single method, for the small token tables
// returned by mu.on and mu.after.
void
registerToken(lua_State *L, const char *mt, const char *method,
              lua_CFunction fn)
{
    luaL_newmetatable(L, mt);
    lua_newtable(L);
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, method);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

// The lookup tables returned by mu.workspaces and mu.outputs: an array, plus
// lookup by name.
void
registerLookup(lua_State *L, const char *mt, lua_CFunction byName)
{
    luaL_newmetatable(L, mt);
    lua_pushcfunction(L, byName);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
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

void
MuWM::clearLuaTimers()
{
    g_timers.clear();
}

// Runs the user's config. Must happen after the socket exists and
// WAYLAND_DISPLAY is set, so that mu.spawn() clients can connect.
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

    // On reload, everything the old config registered must go away.
    clearBindings();
    installDefaultBindings();
    m_eventBus.clear();
    clearLuaTimers();
    lua_pushnil(m_lua);
    lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kRulesKey);

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
    static Group rootGroup, configGroup, borderGroup, keyboardGroup,
        touchpadGroup, gapsGroup, layoutGroup, cursorGroup, dimGroup,
        opacityGroup;

    registerHandle<WindowKind>(m_lua);
    registerHandle<WorkspaceKind>(m_lua);
    registerHandle<OutputKind>(m_lua);
    registerLookup(m_lua, "mu.workspaces", workspacesByName);
    registerLookup(m_lua, "mu.outputs", outputsByName);
    registerToken(m_lua, "mu.listener", "off", listenerOff);
    registerToken(m_lua, "mu.timer", "cancel", timerCancel);

    if (luaL_dostring(m_lua, kRuleApplySource) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "window rules are unavailable: %s",
                lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
    }
    else
        lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kRuleApplyKey);

    auto &kb = m_config.keyboard;
    auto &bd = m_config.border;

    // lua ---@class mu.config.cursor
    cursorGroup.fields = {

        // lua ---@field size? integer Cursor size in pixels. Must be between 1
        // and 1000.
        intField("size", &m_config.cursor.size, 1, 1000, MuWM::ChangeCursor),

        // lua ---@field theme? string Cursor theme name.
        stringField("theme", &m_config.cursor.theme, MuWM::ChangeCursor),
    };

    // lua ---@class mu.config.border
    borderGroup.fields = {
        // lua ---@field width? integer Border width in pixels. Must be between
        // 0 and 1000.
        intField("width", &bd.width, 0, 1000, MuWM::ChangeLayout),

        // lua ---@field focused? mu.Color Border color for the focused window.
        colorField("focused", bd.focused_color, MuWM::ChangeRecolor),

        // lua ---@field unfocused? mu.Color Border color for unfocused windows.
        colorField("unfocused", bd.unfocused_color, MuWM::ChangeRecolor),

        // lua ---@field smart? boolean Whether to hide borders when only one
        // window is visible.
        boolField("smart", &bd.smart, MuWM::ChangeLayout),

        // lua ---@field fullscreen? boolean Whether to draw a border around
        // fullscreen windows.
        boolField("fullscreen", &bd.fullscreen, MuWM::ChangeLayout),
    };

    // lua ---@class mu.config.layout
    layoutGroup.fields = {
        // lua ---@field master_ratio? number The ratio of the master area to
        // the whole screen. Must be between 0 and 1.
        floatField("master_ratio", &m_config.layout.master_ratio,
                   MuWM::ChangeLayout, 0.0f, 1.0f),

    };

    // lua ---@class mu.config.keyboard
    keyboardGroup.fields = {
        // lua ---@field rules? string XKB rules file name.
        stringField("rules", &kb.rules, MuWM::ChangeKeyboard),

        // lua ---@field model? string XKB model name.
        stringField("model", &kb.model, MuWM::ChangeKeyboard),

        // lua ---@field layout? string XKB layout name.
        stringField("layout", &kb.layout, MuWM::ChangeKeyboard),

        // lua ---@field variant? string XKB variant name.
        stringField("variant", &kb.variant, MuWM::ChangeKeyboard),

        // lua ---@field xkb_options? string XKB options string, e.g.
        // "ctrl:nocaps".
        stringField("xkb_options", &kb.xkb_options, MuWM::ChangeKeyboard),

        // lua ---@field repeat_rate? integer Keyboard repeat rate in characters
        // per second.
        intField("repeat_rate", &kb.repeat_rate, INT_MIN, INT_MAX,
                 MuWM::ChangeKeyboard),

        // lua ---@field repeat_delay? integer Keyboard repeat delay in
        // milliseconds.
        intField("repeat_delay", &kb.repeat_delay, INT_MIN, INT_MAX,
                 MuWM::ChangeKeyboard),
    };

    // lua ---@class mu.config.gaps
    gapsGroup.fields = {
        // lua ---@field size? integer Window gap in pixels.
        intField("size", &m_config.gaps.value, 0, 1000, MuWM::ChangeLayout),

        // lua ---@field smart? boolean Whether to hide gaps when only one
        // window is visible.
        boolField("smart", &m_config.gaps.smart, MuWM::ChangeLayout),
    };

    // lua ---@class mu.config.touchpad
    touchpadGroup.fields = {
        // lua ---@field natural_scroll? boolean Whether to enable natural
        // scrolling on the touchpad.
        boolField("natural_scroll", &m_config.touchpad.natural_scroll,
                  MuWM::ChangePointer),

        // lua ---@field tap_to_click? boolean Whether to enable tap-to-click on
        // the touchpad.
        boolField("tap_to_click", &m_config.touchpad.tap_to_click,
                  MuWM::ChangePointer),

        // lua ---@field disable_while_typing? boolean Whether to disable the
        // touchpad while typing.
        boolField("disable_while_typing",
                  &m_config.touchpad.disable_while_typing, MuWM::ChangePointer),

        // lua ---@field middle_emulation? boolean Whether to enable middle
        // button emulation on the touchpad.
        boolField("middle_emulation", &m_config.touchpad.middle_emulation,
                  MuWM::ChangePointer),
    };

    // lua ---@class mu.config.dim
    dimGroup.fields = {
        // lua ---@field unfocused? boolean Whether to dim inactive windows.
        boolField("unfocused", &m_config.dim.unfocused, MuWM::ChangeRecolor),

        // lua ---@field modal? boolean Whether to dim inactive windows only
        // when a modal window is open.
        boolField("modal", &m_config.dim.modal, MuWM::ChangeRecolor),

        // lua ---@field factor? number The dimming factor for inactive windows,
        // between 0 and 1.
        floatField("factor", &m_config.dim.factor, MuWM::ChangeRecolor, 0.0f,
                   1.0f),
    };

    // lua ---@class mu.config.opacity
    opacityGroup.fields = {
        // lua ---@field unfocused? boolean Whether to make inactive windows
        // transparent.
        boolField("unfocused", &m_config.opacity.unfocused,
                  MuWM::ChangeRecolor),

        // lua ---@field modal? boolean Whether to make inactive windows
        // transparent only when a modal window is open.
        boolField("modal", &m_config.opacity.modal, MuWM::ChangeRecolor),

        // lua ---@field factor? number The opacity factor for inactive windows,
        // between 0 and 1.
        floatField("factor", &m_config.opacity.factor, MuWM::ChangeRecolor,
                   0.0f, 1.0f),
    };

    // clang-format off
    // lua ---@class mu.config
    configGroup.fields = {
        // lua ---@field keyboard mu.config.keyboard Keyboard layout options.
        subField("keyboard", &keyboardGroup),

        // lua ---@field touchpad mu.config.touchpad Touchpad options.
        subField("touchpad", &touchpadGroup),

        // lua ---@field gaps mu.config.gaps Window gap options.
        subField("gaps", &gapsGroup),

        // lua ---@field border mu.config.border Window border options.
        subField("border", &borderGroup),

        // lua ---@field cursor mu.config.cursor Cursor options.
        subField("cursor", &cursorGroup),

        // lua ---@field layout mu.config.layout Layout options.
        subField("layout", &layoutGroup),

        // lua ---@field new_client_position mu.NewClientPosition Where a new
        // window is inserted in the layout.
        enumField("new_client_position", &m_config.new_client_position,
                  &kNewClientPositionNames, MuWM::ChangeLayout),

        // lua ---@field dim mu.config.dim Dimming options.
        subField("dim", &dimGroup),

        // lua ---@field opacity mu.config.opacity Opacity options.
        subField("opacity", &opacityGroup),

        // lua ---@field workspaces string[] Workspace names; their number is the
        // workspace count.
        stringListField("workspaces", &m_config.workspaces,
                        MuWM::ChangeWorkspace),

        // lua ---@field dynamic_workspaces boolean Whether to create and destroy
        // workspaces on demand, instead of using the fixed list in mu.config.workspaces.
        boolField("dynamic_workspaces", &m_config.dynamic_workspaces,
                  MuWM::ChangeWorkspace),

        // lua ---@field focus_moved_window boolean Whether moving a window to
        // another workspace also switches to that workspace and focuses the
        // window. Calls can override it per move.
        boolField("focus_moved_window", &m_config.focus_moved_window),

        // lua ---@field focus_follows_mouse boolean Whether to focus a window
        // when the mouse enters it.
        boolField("focus_follows_mouse", &m_config.focus_follows_mouse),

        // lua ---@field raise_on_focus boolean Whether to raise a window when it
        // is focused.
        boolField("raise_on_focus", &m_config.raise_on_focus),

        // lua ---@field apply_tearing boolean Whether to apply tearing to
        // windows that request it.
        boolField("apply_tearing", &m_config.apply_tearing),
    };
    // clang-format on

    // lua ---@class mu
    rootGroup.fields = {

        // lua ---@field config mu.config Options. Assign them one by one
        // (mu.config.gaps.size = 8) or call it with a table: mu.config { ... }.
        subField("config", &configGroup),

        // lua ---@field window mu.Window|nil The focused window, if any.
        getterField("window", getWindow),

        // lua ---@field windows mu.Window[] All windows.
        getterField("windows", getWindows),

        // lua ---@field workspace mu.Workspace|nil The workspace shown on the
        // focused output.
        getterField("workspace", getWorkspace),

        // lua ---@field workspaces mu.Workspace[] The workspaces of the focused
        // output, in order. Also indexable by name: mu.workspaces.web.
        getterField("workspaces", getWorkspaces),

        // lua ---@field output mu.Output|nil The focused output, if any.
        getterField("output", getOutput),

        // lua ---@field outputs mu.Output[] All outputs. Also indexable by
        // name:
        // mu.outputs["DP-1"].
        getterField("outputs", getOutputs),

        // lua ---@field toggle_fullscreen fun() Toggle fullscreen for the
        // focused window.
        funcField("toggle_fullscreen", luaCall<&MuWM::toggleFullscreen>),

        // lua ---@field toggle_floating fun() Toggle floating for the focused
        // window.
        funcField("toggle_floating", luaCall<&MuWM::toggleFloating>),

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

        funcField("focus", luaFocus),
        funcField("swap", luaSwap),
        funcField("focus_output", luaFocusOutput),
        funcField("move_to_output", luaMoveToOutput),
        funcField("spawn", luaSpawn),
        funcField("bind", luaBind),
        funcField("rule", luaRule),
        funcField("on", luaOn<false>),
        funcField("once", luaOn<true>),
        funcField("after", luaAfter),
        funcField("every", luaEvery),
        funcField("log", luaLog),
    };

    lua_pushlightuserdata(m_lua, this);
    lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kWmKey);

    pushProxy(m_lua, &rootGroup);
    lua_setglobal(m_lua, "mu");
}
