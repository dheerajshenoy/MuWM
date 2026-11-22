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
        String,
        Int,
        Color,
        Sub,
        Func
    } kind;
    const char *name;
    void *ptr        = nullptr; // std::string*, int*, float[4], or Group*
    lua_CFunction fn = nullptr;
    int changes      = 0; // MuWM::Change flags to apply after a write
    int min          = INT_MIN;
    int max          = INT_MAX;
};

struct Group
{
    std::vector<Field> fields;
};

Field
stringField(const char *name, std::string *p)
{
    return {Field::String, name, p};
}

Field
intField(const char *name, int *p, int min = INT_MIN, int max = INT_MAX,
         int changes = 0)
{
    return {Field::Int, name, p, nullptr, changes, min, max};
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

int
luaBind(lua_State *L)
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

int
luaSpawn(lua_State *L)
{
    wmFrom(L)->spawn(luaL_checkstring(L, 1));
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

    // Compile first: a syntax error must not cost the user their old bindings.
    if (luaL_loadfile(m_lua, path.c_str()) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "Failed to load Lua config: %s",
                lua_tostring(m_lua, -1));
        lua_pop(m_lua, 1);
        return;
    }

    // On reload, bindings the new config no longer sets must go away.
    clearBindings();

    if (lua_pcall(m_lua, 0, 0, 0) != LUA_OK)
    {
        wlr_log(WLR_ERROR, "Failed to run Lua config: %s",
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
    static Group rootGroup, optsGroup, borderGroup, keymapGroup, keyboardGroup;

    auto &kb = m_config.keyboard;
    auto &bd = m_config.border;

    borderGroup.fields = {
        intField("width", &bd.width, 0, 1000, MuWM::ChangeLayout),
        colorField("focused", bd.focused_color, MuWM::ChangeRecolor),
        colorField("unfocused", bd.unfocused_color, MuWM::ChangeRecolor),
    };

    keyboardGroup.fields = {
        stringField("rules", &kb.rules),
        stringField("model", &kb.model),
        stringField("layout", &kb.layout),
        stringField("variant", &kb.variant),
        stringField("xkb_options", &kb.xkb_options),
        intField("repeat_rate", &kb.repeat_rate),
        intField("repeat_delay", &kb.repeat_delay),
    };

    optsGroup.fields = {
        subField("keyboard", &keyboardGroup),
        intField("gap", &m_config.gap, 0, 1000, MuWM::ChangeLayout),
        subField("border", &borderGroup),
    };

    keymapGroup.fields = {funcField("set", luaBind)};

    rootGroup.fields = {
        subField("keymap", &keymapGroup),
        subField("opts", &optsGroup),
        funcField("focus_next", luaCall<&MuWM::focusNext>),
        funcField("focus_prev", luaCall<&MuWM::focusPrev>),
        funcField("swap_next", luaCall<&MuWM::swapNext>),
        funcField("swap_prev", luaCall<&MuWM::swapPrev>),
        funcField("close", luaCall<&MuWM::close>),
        funcField("quit", luaCall<&MuWM::quit>),
        funcField("reload", luaCall<&MuWM::loadConfig>),
        funcField("spawn", luaSpawn),
    };

    lua_pushlightuserdata(m_lua, this);
    lua_rawsetp(m_lua, LUA_REGISTRYINDEX, &kWmKey);

    pushProxy(m_lua, &rootGroup);
    lua_setglobal(m_lua, "mu");
}
