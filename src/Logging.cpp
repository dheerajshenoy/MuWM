#include "Logging.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <strings.h>
#include <unistd.h>

namespace
{

FILE *g_file = nullptr;

// $XDG_STATE_HOME/<project>/<project>.log, else ~/.local/state/...
std::filesystem::path
logPath()
{
    namespace fs = std::filesystem;
    fs::path base;
    if (const char *xdg = getenv("XDG_STATE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = getenv("HOME"); home && *home)
        base = fs::path(home) / ".local" / "state";
    else
        return {};
    return base / PROJECT_NAME / (std::string(PROJECT_NAME) + ".log");
}

// "silent", "error", "info", "debug" (any case) or 0-3.
bool
parseLevel(const char *text, wlr_log_importance &out)
{
    static const struct
    {
        const char *name;
        wlr_log_importance level;
    } levels[] = {
        {"silent", WLR_SILENT},
        {"error", WLR_ERROR},
        {"info", WLR_INFO},
        {"debug", WLR_DEBUG},
    };

    for (const auto &l : levels)
        if (strcasecmp(text, l.name) == 0)
        {
            out = l.level;
            return true;
        }

    char *end        = nullptr;
    const long value = strtol(text, &end, 10);
    if (end != text && *end == '\0' && value >= WLR_SILENT
        && value <= WLR_DEBUG)
    {
        out = static_cast<wlr_log_importance>(value);
        return true;
    }
    return false;
}

void
callback(wlr_log_importance level, const char *fmt, va_list args)
{
    static const char *const names[] = {"", "ERROR", "INFO", "DEBUG"};

    // wlroots hands a custom callback every message, whatever the configured
    // verbosity; filtering is up to us.
    if (level > wlr_log_get_verbosity())
        return;

    char stamp[32];
    const time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof stamp, "%F %T", &local);

    // A va_list can only be consumed once, so each sink gets its own copy.
    if (g_file)
    {
        va_list copy;
        va_copy(copy, args);
        fprintf(g_file, "%s %-5s ", stamp, names[level]);
        vfprintf(g_file, fmt, copy);
        fputc('\n', g_file);
        fflush(g_file); // a crash must not lose the last lines
        va_end(copy);
    }

    // Also show it on the terminal, or anywhere if there is no file.
    if (!g_file || isatty(STDERR_FILENO))
    {
        va_list copy;
        va_copy(copy, args);
        fprintf(stderr, "%s %-5s ", stamp, names[level]);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
}

} // namespace

void
initLogging(wlr_log_importance defaultLevel)
{
    namespace fs        = std::filesystem;
    const fs::path path = logPath();

    // Read before any config runs, so it also covers the first messages.
    wlr_log_importance level = defaultLevel;
    const char *env          = getenv("MUWM_LOG");
    const bool badLevel      = env && *env && !parseLevel(env, level);
    if (badLevel)
        level = defaultLevel;

    if (!path.empty())
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        // Keep the previous session's log: it's the one you need after a crash.
        fs::path old = path;
        old += ".old";
        fs::rename(path, old, ec); // fine if there was no previous log

        g_file = fopen(path.c_str(), "w");
    }

    wlr_log_init(level, callback);

    if (badLevel)
        wlr_log(WLR_ERROR,
                "Ignoring invalid MUWM_LOG='%s' (use silent, error, info or "
                "debug)",
                env);

    if (g_file)
        wlr_log(WLR_INFO, "Logging to %s", path.c_str());
    else
        wlr_log(WLR_ERROR, "Could not open a log file; logging to stderr only");
}
