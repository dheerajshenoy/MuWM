// Logging.hpp

#pragma once
#include "wlroots.hpp"

// Opens the log file (keeping the previous one as .old) and installs the
// wlroots log callback. `defaultLevel` is used unless the MUWM_LOG environment
// variable ("silent", "error", "info", "debug" or 0-3) sets another one.
void initLogging(wlr_log_importance defaultLevel);
