#include "MuWM.hpp"

void
initArgs(argparse::ArgumentParser &parser)
{
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

    wlr_log_init(WLR_DEBUG, NULL);

    MuWM wm(parser);

    return wm.run() ? 0 : 1;
}
