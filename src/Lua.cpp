// Lua API: the `muwm` global table, its `muwm.opts` option proxies, and
// loading of the user's init.lua.

#include "MuWM.hpp"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace
{

struct Group;

struct Field
{
    enum Kind
    {
        Int,
        Color,
        Sub,
        Func
    } kind;
    const char *name;
    void *ptr        = nullptr; // int*, float[4], or Group*
    lua_CFunction fn = nullptr;
    int changes      = 0; // MuWM::Change flags to apply after a write
    int min          = INT_MIN;
    int max          = INT_MAX;
};

struct Group
{
    std::vector<Field> fields;
};

const Field *
findField(const Group *g, const char *name)
{
    for (const Field &f : g->fields)
        if (std::strcmp(f.name, name) == 0)
            return &f;
    return nullptr;
}

char kWmKey; // its address is used as a registry key (see spawn below)

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
        case Field::Int:
        {
            if (lua_type(L, val) != LUA_TNUMBER)
                luaL_error(L, "option '%s' expects a number", key);
            const lua_Number n = lua_tonumber(L, val);
            if (!(n == std::floor(n))) // also rejects NaN
                luaL_error(L, "option '%s' expects an integer", key);
            if (n < f->min || n > f->max)
                luaL_error(L, "option '%s' must be between %d and %d", key,
                           f->min, f->max);
            *static_cast<int *>(f->ptr) = static_cast<int>(n);
            break;
        }

        case Field::Color:
        {
            float tmp[4];
            if (lua_type(L, val) == LUA_TSTRING)
            {
                if (!parseHexColor(lua_tostring(L, val), tmp))
                    luaL_error(L,
                               "option '%s': invalid color string (expected "
                               "\"#rrggbb\" or \"#rrggbbaa\")",
                               key);
            }
            else if (lua_type(L, val) == LUA_TTABLE && lua_rawlen(L, val) == 4)
            {
                for (int i = 0; i < 4; i++)
                {
                    lua_rawgeti(L, val, i + 1);
                    if (lua_type(L, -1) != LUA_TNUMBER)
                        luaL_error(L, "option '%s': entry %d is not a number",
                                   key, i + 1);
                    tmp[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
            }
            else
            {
                luaL_error(L,
                           "option '%s' expects a color: \"#rrggbb[aa]\" or "
                           "a table {r, g, b, a}",
                           key);
            }
            std::memcpy(f->ptr, tmp, sizeof tmp); // all-or-nothing
            break;
        }

        case Field::Sub:
            if (lua_type(L, val) != LUA_TTABLE)
                luaL_error(L, "option '%s' expects a table", key);
            lua_pushnil(L);
            while (lua_next(L, val) != 0)
            {
                if (lua_type(L, -2) != LUA_TSTRING)
                    luaL_error(L, "option '%s': keys must be strings", key);
                setField(L, static_cast<const Group *>(f->ptr),
                         lua_tostring(L, -2), lua_gettop(L));
                lua_pop(L, 1);
            }
            break;

        case Field::Func:
            luaL_error(L, "'%s' is read-only", key);
            break;
    }

    if (f->changes != MuWM::ChangeNone)
    {
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kWmKey);
        auto *wm = static_cast<MuWM *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        wm->scheduleApply(f->changes);
    }
}

int
proxyNewIndex(lua_State *L)
{
    auto *g
        = static_cast<const Group *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (lua_type(L, 2) != LUA_TSTRING)
        return luaL_error(L, "option names must be strings");
    setField(L, g, lua_tostring(L, 2), 3);
    return 0;
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

int
proxyIndex(lua_State *L)
{
    auto *g
        = static_cast<const Group *>(lua_touserdata(L, lua_upvalueindex(1)));
    if (lua_type(L, 2) != LUA_TSTRING)
    {
        lua_pushnil(L);
        return 1;
    }
    const Field *f = findField(g, lua_tostring(L, 2));
    if (!f)
    {
        lua_pushnil(L);
        return 1;
    }

    switch (f->kind)
    {
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
    return 1;
}

void
pushProxy(lua_State *L, const Group *g)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_pushlightuserdata(L, const_cast<Group *>(g));
    lua_pushcclosure(L, proxyIndex, 1);
    lua_setfield(L, -2, "__index");
    lua_pushlightuserdata(L, const_cast<Group *>(g));
    lua_pushcclosure(L, proxyNewIndex, 1);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
}

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
        return;

    const auto path = configFilePath();
    if (path.empty() || !std::filesystem::exists(path))
        return;

    if (luaL_dofile(m_lua, path.c_str()) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "Failed to load Lua config: %s",
                lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        return;
    }
    wlr_log(WLR_INFO, "Lua config loaded from %s", path.c_str());
}

void
MuWM::initLuaBindings()
{
    // The proxies keep raw pointers to these, so they must outlive this
    // function (locals would dangle as soon as it returned).
    static Group m_rootGroup, m_optsGroup, m_borderGroup;

    m_borderGroup.fields = {
        {Field::Int, "width", &m_config.border.width, nullptr,
         MuWM::ChangeLayout, 0, 1000},
        {Field::Color, "focused", m_config.border.focused_color, nullptr,
         MuWM::ChangeRecolor},
        {Field::Color, "unfocused", m_config.border.unfocused_color, nullptr,
         MuWM::ChangeRecolor},
    };

    m_optsGroup.fields = {
        {Field::Int, "gap", &m_config.gap, nullptr, MuWM::ChangeLayout, 0,
         1000},
        {Field::Sub, "border", &m_borderGroup},
    };

    m_rootGroup.fields = {
        {Field::Sub, "opts", &m_optsGroup},
        {Field::Func, "spawn", nullptr,
         [](lua_State *L) -> int
    {
        const char *cmd = luaL_checkstring(L, 1); // before touching the stack
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kWmKey);
        auto *wm = static_cast<MuWM *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        wm->spawn(cmd);
        return 0;
    }},
    };

    lua_pushlightuserdata(m_lua, this);
    lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kWmKey);

    pushProxy(m_lua, &m_rootGroup);
    lua_setglobal(m_lua, "muwm");
}
