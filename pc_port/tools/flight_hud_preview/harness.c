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
    return 0;
}
