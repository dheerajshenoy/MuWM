#include "MuWM.hpp"
#include "Logging.hpp"

void
initArgs(argparse::ArgumentParser &parser)
{
    parser.add_argument("-s", "--spawn")
        .help("Command to spawn on startup")
        .metavar("CMD");
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

    initLogging(WLR_INFO); // MUWM_LOG=debug for more

    MuWM wm(parser);
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
