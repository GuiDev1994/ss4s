#include <assert.h>

#include "ss4s.h"

int main(int argc, char *argv[]) {
    SS4S_Config config = {.audioDriver = NULL, .videoDriver = "empty"};
    SS4S_Init(argc, argv, &config);
    SS4S_PostInit(argc, argv);

    SS4S_Player *player = SS4S_PlayerOpen();
    assert(player != NULL);
    assert(!SS4S_PlayerGetPanelPhaseLoosen(player));

    SS4S_PlayerSetPanelPhaseLoosen(player, true);
    assert(SS4S_PlayerGetPanelPhaseLoosen(player));

    SS4S_PlayerSetPanelPhaseLoosen(player, false);
    assert(!SS4S_PlayerGetPanelPhaseLoosen(player));

    SS4S_PlayerClose(player);
    SS4S_Quit();
    return 0;
}
