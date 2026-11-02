// QuakeMove64 - movimentação estilo Quake/GoldSrc para GTA SA Android 2.10 (arm64)
// Plugin AML. Usa as structs/símbolos do aml-psdk (que suporta a 2.10 64-bit).
#include <float.h>
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/ai/PedIntelligence.h>
#include <aml-psdk/game_sa/engine/Pad.h>
#include <aml-psdk/game_sa/base/Timer.h>
#include <aml-psdk/game_sa/entity/Camera.h>

#include "quake_move.h"

MYMODCFG(net.quakemove.gtasa64, Quake Movement 64, 1.0.0, QuakeMove)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- configuração

struct Settings
{
    bool enabled      = true;
    bool autoHop      = true;   // segurar pulo = bunny hop automático
    bool invertFwd    = false;  // se "frente" estiver invertida
    bool invertStrafe = false;  // se esquerda/direita estiverem invertidas
    bool debug        = false;
    qm::Params p;
} g;

static float CfgF(const char* key, float def)
{
    float v = cfg->Bind(key, def, "QuakeMove")->GetFloat();
    delete Config::pLastEntry;
    return v;
}
static bool CfgB(const char* key, bool def)
{
    bool v = cfg->Bind(key, def, "QuakeMove")->GetBool();
    delete Config::pLastEntry;
    return v;
}

// ---------------------------------------------------------------- estado

struct State
{
    CPlayerPed* ped      = nullptr;
    bool        active   = false;   // estamos controlando a velocidade?
    bool        wasOnGround = true;
    bool        canAutoHop  = false; // usado pelo hook de JumpJustDown
    qm::V2      vel      = { 0, 0 };  // m/s
    qm::V2      lastWritten = { 0, 0 };
    unsigned    frame    = 0;

    void Reset()
    {
        active = false;
        canAutoHop = false;
        vel = lastWritten = { 0, 0 };
        wasOnGround = true;
    }
} st;

static bool g_hasTaskChecks = false;

// ---------------------------------------------------------------- elegibilidade

static bool IsBadPedState(unsigned s)
{
    switch (s)
    {
        case PEDSTATE_DIE: case PEDSTATE_DEAD: case PEDSTATE_DIE_BY_STEALTH:
        case PEDSTATE_DRIVING: case PEDSTATE_PASSENGER: case PEDSTATE_TAXI_PASSENGER:
        case PEDSTATE_ENTER_CAR: case PEDSTATE_EXIT_CAR: case PEDSTATE_OPEN_DOOR:
        case PEDSTATE_CARJACK: case PEDSTATE_DRAGGED_FROM_CAR: case PEDSTATE_STEAL_CAR:
        case PEDSTATE_ARRESTED: case PEDSTATE_ARREST_PLAYER: case PEDSTATE_HANDS_UP:
        case PEDSTATE_GETUP: case PEDSTATE_STAGGER: case PEDSTATE_SIT:
        case PEDSTATE_ENTER_TRAIN: case PEDSTATE_EXIT_TRAIN: case PEDSTATE_ABSEIL_FROM_HELI:
        case PEDSTATE_ON_FIRE: case PEDSTATE_CPR:
            return true;
    }
    return false;
}

static bool Eligible(CPlayerPed* ped, CPad* pad)
{
    if (!g.enabled || !pad) return false;
    if (pad->DisablePlayerControls) return false;           // cutscene / script tirou o controle
    if (ped->bInVehicle) return false;
    if (ped->m_fHealth <= 0.0f) return false;
    if (ped->bIsAimingGun || ped->bIsDucking) return false; // mirando: o jogo move relativo à mira
    if (ped->bKnockedUpIntoAir) return false;
    if (ped->bIsInWater || ped->bSubmergedInWater) return false;
    if (IsBadPedState((unsigned)ped->m_ePedState)) return false;

    if (g_hasTaskChecks && ped->m_pIntelligence)
    {
        CPedIntelligence* in = ped->m_pIntelligence;
        if (in->GetTaskSwim() || in->GetTaskJetPack() || in->GetTaskClimb() || in->GetTaskDuck(false))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- hooks

DECL_HOOKv(CPlayerPed_ProcessControl, CPlayerPed* self)
{
    // 1) velocidade que o jogo deixou depois da física do frame anterior
    qm::V2 pre = { self->m_vecMoveSpeed.x * 50.0f, self->m_vecMoveSpeed.y * 50.0f };

    CPlayerPed_ProcessControl(self);   // lógica original (tasks, animações, input...)

    if (self != FindPlayerPed(-1)) return;
    if (st.ped != self) { st.Reset(); st.ped = self; }

    CPad* pad = CPad::GetPad(0);
    float ts = CTimer::ms_fTimeStep;               // 1.0 = 20 ms de jogo
    if (!(ts > 0.0001f) || !Eligible(self, pad))
    {
        st.Reset();
        st.ped = self;
        return;
    }
    float dt = ts * 0.02f;
    if (dt > 0.1f) dt = 0.1f;

    bool onGround = self->bIsStanding;

    // 2) mudanças externas (colisão com parede, empurrão, explosão) viram delta na nossa velocidade
    if (st.active && (!st.wasOnGround || self->bPedHitWallLastFrame))
    {
        st.vel.x += pre.x - st.lastWritten.x;
        st.vel.y += pre.y - st.lastWritten.y;
    }

    // 3) direção desejada = direção do analógico relativa à câmera
    float fwd = -(float)pad->GetPedWalkUpDown() / 128.0f;
    float rgt =  (float)pad->GetPedWalkLeftRight() / 128.0f;
    if (g.invertFwd) fwd = -fwd;
    if (g.invertStrafe) rgt = -rgt;
    fwd = fwd > 1.0f ? 1.0f : (fwd < -1.0f ? -1.0f : fwd);
    rgt = rgt > 1.0f ? 1.0f : (rgt < -1.0f ? -1.0f : rgt);

    CVector& cam = TheCamera.m_mCameraMatrix.GetForward();
    qm::V2 cf = { cam.x, cam.y };
    float cl = qm::Len(cf);
    if (cl < 0.001f) return;
    cf.x /= cl; cf.y /= cl;
    qm::V2 cr = { cf.y, -cf.x };
    qm::Wish wish = qm::MakeWish(cf, cr, fwd, rgt, g.p.maxSpeed);

    bool jumpHeld = g.autoHop && onGround && pad->GetJump();

    // 4) parado e sem input: devolve o controle ao jogo (idle normal, scripts, etc.)
    if (onGround && wish.speed <= 0.0f && !jumpHeld && qm::Len(st.vel) < 0.2f && (!st.active || qm::Len(pre) < 0.2f))
    {
        st.active = false;
        st.canAutoHop = g.autoHop;
        st.wasOnGround = onGround;
        return;
    }

    if (!st.active)
    {
        st.vel = pre;            // começa de onde o jogo estava
        st.active = true;
        st.wasOnGround = onGround;
    }

    // 5) saiu do chão pulando: limite estilo GoldSrc (se BhopCap > 0)
    if (st.wasOnGround && !onGround && self->m_vecMoveSpeed.z > 0.01f)
        qm::PreventMegaBhop(st.vel, g.p);

    // 6) um passo de física Quake/GoldSrc
    qm::Step(st.vel, onGround, jumpHeld, wish, dt, g.p);

    // 7) escreve de volta (m/s -> unidades por tick de 20 ms)
    self->m_vecMoveSpeed.x = st.vel.x * 0.02f;
    self->m_vecMoveSpeed.y = st.vel.y * 0.02f;
    st.lastWritten = st.vel;
    st.wasOnGround = onGround;
    st.canAutoHop = g.autoHop && onGround;

    if (g.debug && (st.frame++ % 60) == 0)
        logger->Info("vel=%.2f m/s ground=%d wish=%.2f state=%u", qm::Len(st.vel), (int)onGround, wish.speed, (unsigned)self->m_ePedState);
}

// Autohop: enquanto o botão de pulo está segurado e o jogador está no chão, o jogo recebe um "acabou de apertar".
DECL_HOOKb(CPad_JumpJustDown, CPad* self)
{
    bool r = CPad_JumpJustDown(self);
    if (!r && st.canAutoHop && self == CPad::GetPad(0) && self->GetJump())
        return true;
    return r;
}

// ---------------------------------------------------------------- init

ON_MOD_LOAD()
{
    logger->SetTag("QuakeMove");

    void* hGTASA = aml->GetLibHandle("libGTASA.so");
    if (!hGTASA)
    {
        logger->Error("libGTASA.so nao encontrada - plugin desativado");
        return;
    }

    // Se algum símbolo essencial não existir nessa versão do jogo, não instala nada (o jogo roda normal).
    const char* required[] = {
        "_ZTV10CPlayerPed", "_Z13FindPlayerPedi", "_ZN4CPad6GetPadEi",
        "_ZN4CPad19GetPedWalkLeftRightEv", "_ZN4CPad16GetPedWalkUpDownEv",
        "_ZN4CPad7GetJumpEv", "_ZN4CPad12JumpJustDownEv"
    };
    for (const char* s : required)
    {
        if (!aml->GetSym(hGTASA, s))
        {
            logger->Error("simbolo ausente: %s - plugin desativado (versao do jogo diferente da 2.10 arm64?)", s);
            return;
        }
    }
    g_hasTaskChecks =
        aml->GetSym(hGTASA, "_ZNK16CPedIntelligence11GetTaskSwimEv") &&
        aml->GetSym(hGTASA, "_ZNK16CPedIntelligence14GetTaskJetPackEv") &&
        aml->GetSym(hGTASA, "_ZNK16CPedIntelligence12GetTaskClimbEv") &&
        aml->GetSym(hGTASA, "_ZNK16CPedIntelligence11GetTaskDuckEb");

    // Config (arquivo .ini criado na primeira execução, pode editar sem recompilar)
    g.enabled      = CfgB("Enabled", true);
    g.autoHop      = CfgB("AutoHop", true);
    g.invertFwd    = CfgB("InvertForward", false);
    g.invertStrafe = CfgB("InvertStrafe", false);
    g.debug        = CfgB("Debug", false);
    g.p.maxSpeed      = CfgF("MaxSpeed", g.p.maxSpeed);           // m/s
    g.p.accelerate    = CfgF("Accelerate", g.p.accelerate);
    g.p.airAccelerate = CfgF("AirAccelerate", g.p.airAccelerate);
    g.p.airWishCap    = CfgF("AirWishCap", g.p.airWishCap);       // m/s (30 ups = 0.762)
    g.p.friction      = CfgF("Friction", g.p.friction);
    g.p.stopSpeed     = CfgF("StopSpeed", g.p.stopSpeed);         // m/s
    g.p.maxVelocity   = CfgF("MaxVelocity", g.p.maxVelocity);     // m/s
    g.p.bhopCap       = CfgF("BhopCap", g.p.bhopCap);             // 0 = Quake, 1.7 = GoldSrc
    cfg->Save();

    HOOK(CPlayerPed_ProcessControl, GetNativeVtable<CPlayerPed>("_ZTV10CPlayerPed")[11]);
    HOOKSYM(CPad_JumpJustDown, hGTASA, "_ZN4CPad12JumpJustDownEv");

    logger->Info("QuakeMove carregado (maxspeed=%.2f accel=%.1f airaccel=%.1f autohop=%d)",
                 g.p.maxSpeed, g.p.accelerate, g.p.airAccelerate, (int)g.autoHop);
}
