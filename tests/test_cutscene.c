// SPDX-License-Identifier: GPL-2.0-or-later
/* Cutscene hold (#38 lift, #43 drowning struggle): when the native 30 FPS is held and when
 * the chosen rate comes back. */
#include "../src/runtime_cutscene.c"
#include <stdlib.h>

static int verified=1, holds, releases;
int ps4gpu_hold_native_frame_rate(int on) {
    if (!verified) return 0;
    if (on) ++holds; else ++releases;
    return 1;
}

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++failures; } } while (0)

#define DIALOGUE "/app0/_data/1408209885/ps4/primary/3bf280d756aa4f4e/v1411677946/00000000268ec257"
#define MIX "/app0/_data/1408209885/ps4/primary/3bf280d756aa4f4e/v1411677946/000000003989d3f9"
#define OTHER "/app0/_data/1408209885/ps4/primary/3bf280d756aa4f4e/v1411677946/000000003b9483be"
#define DROWN "/app0/_data/1408209885/ps4/primary/3bf280d756aa4f4e/v1411677946/000000001a473007"
#define SAVE "/savedata0/SYSTEM.DAT"

int main(void) {
    /* Nothing is held before the scene, and saves alone do nothing. */
    cutscene_open_at(OTHER,0); cutscene_open_at(SAVE,1000);
    CHECK(holds==0 && releases==0);

    /* The scene: held from its dialogue; the save written as the lift starts does not end it;
     * the next checkpoint does. */
    cutscene_open_at(DIALOGUE,10000);
    CHECK(holds==1 && held);
    cutscene_open_at(SAVE,10200);
    cutscene_open_at(OTHER,30000);
    CHECK(releases==0 && held);
    cutscene_open_at(SAVE,46000);
    CHECK(releases==1 && !held);
    cutscene_open_at(SAVE,47000);
    CHECK(releases==1);

    /* The scene's sound mix starts it 16 s before the dialogue, which then keeps one hold;
     * the save written as the second part starts (10 s rule from the dialogue) does not end it. */
    cutscene_open_at(MIX,60000);
    CHECK(holds==2 && held);
    cutscene_open_at(DIALOGUE,76000);
    cutscene_open_at(SAVE,76800);
    CHECK(holds==2 && held && releases==1);
    cutscene_open_at(SAVE,112000);
    CHECK(releases==2 && !held);

    /* A checkpoint reload replays it: the dialogue again restarts the 10 s window, one hold. */
    cutscene_open_at(DIALOGUE,100000);
    cutscene_open_at(DIALOGUE,120000);
    CHECK(holds==3);
    cutscene_open_at(SAVE,125000);
    CHECK(releases==2 && held);
    cutscene_open_at(SAVE,131000);
    CHECK(releases==3 && !held);

    /* No checkpoint (quit to the menu): the time limit ends it on the next open. */
    cutscene_open_at(DIALOGUE,200000);
    cutscene_open_at(OTHER,319000);
    CHECK(held);
    cutscene_open_at(OTHER,320000);
    CHECK(releases==4 && !held);

    /* The drowning struggle after the lift (#43): its bed opens after the checkpoint that
     * ended the lift hold, holds again, re-arms on each fail-restart open, and ends at the
     * next checkpoint (10 s rule). */
    cutscene_open_at(DROWN,340000);
    CHECK(holds==5 && held);
    cutscene_open_at(SAVE,341000);
    CHECK(releases==4 && held);
    cutscene_open_at(DROWN,350000);
    CHECK(holds==5);
    cutscene_open_at(SAVE,361000);
    CHECK(releases==5 && !held);

    /* Similar names elsewhere are not the scene. */
    cutscene_open_at("/app0/other/00000000268ec257",400000);
    cutscene_open_at("/app0/_data/1408209885/ps4/primary/x/00000000268ec2570",400000);
    cutscene_open_at("/savedata0/00000000268ec257/SYSTEM.DAT.bak",400000);
    CHECK(holds==5 && !held);

    /* An executable the menu cannot drive: no hold, so no release either. */
    verified=0;
    cutscene_open_at(DIALOGUE,500000);
    CHECK(!held);
    cutscene_open_at(SAVE,520000);
    CHECK(holds==5 && releases==5);

    cutscene_open_at(NULL,600000);
    if (failures) return 1;
    printf("Cutscene hold checks passed\n");
    return 0;
}
