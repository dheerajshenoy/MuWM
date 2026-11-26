#include "MuWM.hpp"
#include "Logging.hpp"

void
initArgs(argparse::ArgumentParser &parser)
{
    parser.add_argument("-s", "--spawn")
        .help("Command to spawn on startup")
        .metavar("CMD");
    parser.add_argument("--check-config")
        .help("Load the Lua config, report any errors and exit")
        .flag();
}

int
main(int argc, char **argv)
{
    argparse::ArgumentParser parser(PROJECT_NAME, PROJECT_VERSION);
    initArgs(parser);

    try
    {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error &err)
    {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    const bool checkOnly = parser.get<bool>("--check-config");
    if (checkOnly)
        wlr_log_init(WLR_INFO, nullptr); // to stderr, not the log file
    else
        initLogging(WLR_INFO); // MUWM_LOG=debug for more

    MuWM wm(parser);
    if (checkOnly)
        return wm.checkConfig() ? 0 : 1;

    if (!wm.ok())
    {
        wlr_log(WLR_ERROR, "Failed to initialize compositor");
        return 1;
    }

    std::string startupCmd;
    if (auto cmd = parser.present("--spawn"))
        startupCmd = *cmd;

    return wm.run(startupCmd) ? 0 : 1;
}
