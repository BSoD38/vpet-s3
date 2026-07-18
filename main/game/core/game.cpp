#include "game.hpp"
#include "app.hpp"

static App app;   // the one composition-root instance (owns everything)

void game_run(void)
{
    app.init();
    app.runLoop();   // never returns
}
