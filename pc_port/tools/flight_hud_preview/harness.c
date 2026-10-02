#include "sh_log.h"
#undef SH_DBG
#define SH_DBG(...) ((void)0)
#include "../../src/pc_flight_hud.c"

s_SysWork  g_SysWork;
s_GameWork g_GameWork;
static s_Savegame s_sav;
s_Savegame* const g_SavegamePtr = &s_sav;
static s_ControllerData s_c0;
s_ControllerData* const g_Controller0 = &s_c0;
q19_12 g_DeltaTime = Q12(1.0f / 30.0f);
s_PcConfig g_PcConfig;
MATRIX VbWvsMatrix;
int g_PcConsoleInputActive, g_PcQuickOptionsActive;
static int s_touch;
long ReadGeomScreen(void) { return 256; }
void vcGetNowCamPos(VECTOR3* c) { c->vx = 0; c->vy = Q12(-1.7f); c->vz = Q12(-4.0f); }
s32 Pc_WorldAnchorOfy(void) { return 8; }
int PsyX_RawControllerBindHeld(int b) { return 0; }
int Pc_Touch_IsDrivingInput(void) { return s_touch; }
void SD_Call(u32 c) {}

static void dump(const char* path)
{
    FILE* f = fopen(path, "w");
    int i;
    fprintf(f, "%d %d %d\n", s_fill.n, s_glow.n, s_hud.n);
    for (i = 0; i < s_fill.n * 6; i++) fprintf(f, "%g\n", s_fillV[i]);
    for (i = 0; i < s_glow.n * 6; i++) fprintf(f, "%g\n", s_glowV[i]);
    for (i = 0; i < s_hud.n * 6; i++) fprintf(f, "%g\n", s_hudV[i]);
    fclose(f);
}

static void enemy(int i, int id, float x, float z, float faceDeg)
{
    s_SubCharacter* n = &g_SysWork.npcs[i];
    n->model.charaId = id;
    n->health = Q12(100.0f);
    n->collision.state = 3;
    n->position.vx = Q12(x); n->position.vz = Q12(z); n->position.vy = 0;
    n->collision.box.offsetY = Q12(-0.9f);
    n->rotation.vy = (q3_12)(faceDeg / 360.0f * 4096.0f);
}

static void say(int face, int chara)
{
    int i;
    for (i = 0; i < AH_RADIO_LINES; i++)
        if (s_radioLines[i].face == face && (face == AH_FACE_CYBIL) == (s_radioLines[i].who != NULL))
            break;
    s_radioN = 1; s_radioQ[0] = i; s_radioChara[0] = chara;
    s_radioT = AH_RADIO_TIME - 1.0f;
}

static void frame(float aspect, const char* path, int alert)
{
    int k;
    s_w2 = 240.0f * aspect; s_halfW = 120.0f * aspect; s_camH = 256; vcGetNowCamPos(&s_cam);
    red_setup:
    {
        const float o = 1.0f;
        if (alert) { s_main[0]=1; s_main[1]=0.22f; s_main[2]=0.16f; s_main[3]=0.95f;
                     s_hi[0]=1; s_hi[1]=0.85f; s_hi[2]=0.75f; s_hi[3]=1; }
        else { s_main[0]=0.45f; s_main[1]=1; s_main[2]=0.55f; s_main[3]=0.9f;
               s_hi[0]=1; s_hi[1]=0.9f; s_hi[2]=0.35f; s_hi[3]=1; }
        memcpy(s_dim, s_main, sizeof(s_dim)); s_dim[3] *= 0.55f;
    }
    s_hud.n = s_glow.n = s_fill.n = 0;
    s_cur = &s_glow; Ah_BuildFlares();
    s_cur = &s_hud;  if (g_PcConfig.flightHud == 2) Ah_BuildHudClassic(1920, 1080); else Ah_BuildHud();
    dump(path);
}

int main(int argc, char** argv)
{
    int i;
    SDL_Init(0);
    g_PcConfig.flightHud = 1; g_PcConfig.flightHudSound = 1; g_PcConfig.flightHudOpacity = 100; g_PcConfig.crosshair = 0;
    g_GameWork.gameState = GameState_InGame;
    g_SysWork.sysState = SysState_Gameplay;
    VbWvsMatrix.m[0][0] = VbWvsMatrix.m[1][1] = VbWvsMatrix.m[2][2] = 4096;
    g_SysWork.playerWork.player.health = Q12(72.0f);
    g_SysWork.playerWork.player.moveSpeed = Q12(4.2f);
    g_SysWork.playerWork.player.rotation.vy = (q3_12)(12.0f / 360.0f * 4096);
    s_sav.equippedWeapon = InvItemId_Handgun;
    g_SysWork.playerCombat.currentWeaponAmmo = 12;
    g_SysWork.playerCombat.totalWeaponAmmo = 40;
    s_sav.gameplayTimer = Q12(3725.0f);
    s_sav.rangedKillCount = 7; s_sav.meleeKillCount = 2;

    enemy(0, Chara_Groaner, 3.0f, 9.0f, 200.0f);
    enemy(1, Chara_GreyChild, -4.0f, 14.0f, 90.0f);
    enemy(2, Chara_AirScreamer, 1.0f, 22.0f, 180.0f);

    /* no lock yet */
    Pc_FlightHud_Update();
    frame(16.0f/9.0f, "normal.txt", 0);

    for (i = 0; i < 40; i++) Pc_FlightHud_Update();
    printf("alert=%d track=%d stock=%d\n", s_alert, s_anyTrack, s_flareStock);
    frame(16.0f/9.0f, "alert.txt", 1);

    Pc_FlightHud_FlareRequest();
    for (i = 0; i < 14; i++) Pc_FlightHud_Update();
    printf("after flare alert=%d stock=%d jam=%f\n", s_alert, s_flareStock, s_jamT);
    frame(16.0f/9.0f, "flare.txt", 0);

    g_SysWork.playerWork.player.health = Q12(40.0f);
    g_SysWork.playerCombat.isAiming = 1;
    enemy(3, Chara_Stalker, 0.3f, 7.0f, 0.0f);
    frame(16.0f/9.0f, "aim.txt", 0);
    g_SysWork.playerCombat.isAiming = 0;
    g_SysWork.playerWork.player.health = Q12(15.0f);
    g_PcConfig.flightHud = 2;
    frame(16.0f/9.0f, "classic.txt", 0);
    g_PcConfig.flightHud = 1;
    s_touch = 1;
    frame(19.5f/9.0f, "touch.txt", 0);
    s_touch = 0;

    /* A kill a moment ago, the radio talking, one enemy off the right edge
     * and one behind the camera, callsigns on. */
    g_SysWork.playerWork.player.health = Q12(72.0f);
    s_sav.mapIdx = MapIdx_MAP2_S00;
    for (i = 0; i < NPC_COUNT_MAX; i++) g_SysWork.npcs[i].model.charaId = 0;
    enemy(0, Chara_Groaner, 3.0f, 9.0f, 0.0f);
    enemy(1, Chara_GreyChild, 14.0f, 6.0f, 0.0f);
    enemy(2, Chara_AirScreamer, -3.0f, -9.0f, 0.0f);
    enemy(3, Chara_Stalker, -1.0f, 12.0f, 0.0f);
    g_SysWork.npcs[3].health = Q12(40.0f);
    for (i = 0; i < 5; i++) Pc_FlightHud_Update();
    g_SysWork.npcs[3].health = Q12(15.0f);
    Pc_FlightHud_Update();
    g_SysWork.npcs[0].health = 0;
    s_sav.rangedKillCount++;
    for (i = 0; i < 9; i++) Pc_FlightHud_Update();
    g_PcConfig.flightHudCallsigns = 2;
    frame(16.0f/9.0f, "events.txt", 0);
    g_PcConfig.flightHudCallsigns = 2;
    g_PcConfig.flightHud = 2;
    frame(16.0f/9.0f, "events_classic.txt", 0);
    g_PcConfig.flightHud = 1;
    g_PcConfig.flightHudCallsigns = 0;

    /* Third alarm level: a Stalker locked on from 2 m. */
    for (i = 0; i < NPC_COUNT_MAX; i++) g_SysWork.npcs[i].model.charaId = 0;
    for (i = 0; i < 40; i++) Pc_FlightHud_Update();
    enemy(4, Chara_Stalker, 0.4f, 2.0f, 180.0f);
    for (i = 0; i < 80; i++) Pc_FlightHud_Update();
    printf("danger=%d alert=%d\n", s_danger, s_alert);
    frame(16.0f/9.0f, "danger.txt", 1);

    /* New zone, then a boss down: banner waits for the debrief. */
    for (i = 0; i < NPC_COUNT_MAX; i++) g_SysWork.npcs[i].model.charaId = 0;
    for (i = 0; i < 200; i++) Pc_FlightHud_Update();
    s_sav.mapIdx = MapIdx_MAP2_S01;
    for (i = 0; i < 20; i++) Pc_FlightHud_Update();
    printf("banner=%d t=%f\n", s_bannerZone, s_bannerT);
    frame(16.0f/9.0f, "banner.txt", 0);
    for (i = 0; i < 200; i++) Pc_FlightHud_Update();
    enemy(5, Chara_SplitHead, 2.0f, 15.0f, 0.0f);
    Pc_FlightHud_Update();
    g_SysWork.npcs[5].health = 0;
    s_sav.gameplayTimer += Q12(600.0f);
    s_sav.rangedKillCount += 3;
    s_sav.firedShotCount = 40;
    s_sav.midRangeShotCount = 31;
    for (i = 0; i < 20; i++) Pc_FlightHud_Update();
    printf("debrief=%f rank=%c\n", s_debriefT, s_debrief.rank);
    frame(16.0f/9.0f, "debrief.txt", 0);

    /* Comm portraits: Cybil, then a few monsters on the open channel. */
    for (i = 0; i < 400; i++) Pc_FlightHud_Update();
    for (i = 0; i < NPC_COUNT_MAX; i++) g_SysWork.npcs[i].model.charaId = 0;
    say(AH_FACE_CYBIL, 0);       frame(16.0f/9.0f, "comm_cybil.txt", 0);
    say(AH_FACE_DOG, Chara_Groaner);      frame(16.0f/9.0f, "comm_dog.txt", 0);
    say(AH_FACE_BIRD, Chara_AirScreamer); frame(16.0f/9.0f, "comm_bird.txt", 0);
    say(AH_FACE_BEAST, Chara_Creeper);    frame(16.0f/9.0f, "comm_beast.txt", 0);
    say(AH_FACE_CHILD, Chara_GreyChild);  frame(16.0f/9.0f, "comm_child.txt", 0);
    return 0;
}
