// QuakeMove64 v2 - movimentacao estilo Half-Life / CS:GO / Quake para GTA SA Android 2.10 (arm64)
// Plugin AML. Usa as structs/simbolos do aml-psdk (que suporta a 2.10 64-bit).
//
// O que faz:
//  * fisica de chao/ar do GoldSrc (atrito, accelerate, air-strafe), presets "hl", "csgo", "quake"
//  * pulo PROPRIO e deterministico (altura fixa, autohop, buffer de pulo) - nao depende da animacao do CJ
//  * camera em primeira pessoa enquanto o plugin esta ativo e o CJ esta a pe
//  * direcao do analogico calibrada sozinha contra o que o proprio jogo quer fazer
#include <float.h>
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <aml-psdk/game_sa/entity/PlayerPed.h>
#include <aml-psdk/game_sa/ai/PedIntelligence.h>
#include <aml-psdk/game_sa/engine/Pad.h>
#include <aml-psdk/game_sa/base/Timer.h>
#include <aml-psdk/game_sa/entity/Camera.h>
#include <aml-psdk/game_sa/engine/Draw.h>

#include "quake_move.h"

MYMODCFG(net.quakemove.gtasa64, Quake Movement 64, 2.0.1, QuakeMove)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- configuracao

struct Settings
{
    bool  enabled       = true;
    bool  autoHop       = true;   // segurar pulo = bunny hop automatico
    bool  ownJump       = true;   // pulo proprio (altura fixa). false = pulo nativo do jogo
    bool  firstPerson   = true;   // camera em 1a pessoa quando o plugin esta ativo
    bool  firstPersonAim = true;  // tambem em 1a pessoa ao mirar com arma (camera de mira do jogo)
    bool  autoCalibrate = true;   // acerta sozinho frente/tras e esquerda/direita
    bool  flipFwd       = false;  // frente/tras invertidos
    bool  flipStrafe    = false;  // esquerda/direita invertidos
    bool  debug         = false;
    float fov           = 85.0f;  // 0 = nao mexe no FOV
    float nearClip      = 0.2f;   // 0 = nao mexe no near clip
    float eyeHeight     = 0.62f;  // altura dos olhos acima do centro do CJ
    float eyeForward    = 0.12f;  // quanto a camera avanca a frente da cabeca (evita ver o rosto)
    float pitchSens     = 3.0f;   // velocidade de olhar para cima/baixo (rad/s com o analogico/toque no maximo)
    bool  pitchInvert   = false;  // inverte olhar vertical
    int   jumpBufferMs  = 100;    // apertar pulo ate X ms antes de pousar tambem vale
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
static int CfgI(const char* key, int def)
{
    int v = cfg->Bind(key, def, "QuakeMove")->GetInt();
    delete Config::pLastEntry;
    return v;
}

// ---------------------------------------------------------------- estado

struct State
{
    CPlayerPed*  ped         = nullptr;
    bool         active      = false;   // estamos controlando a velocidade?
    bool         wasOnGround = true;
    bool         fpActive    = false;   // 1a pessoa desejada neste frame (lido pelo hook da camera)
    qm::V2       vel         = { 0, 0 };  // m/s
    qm::V2       lastWritten = { 0, 0 };
    float        gEst        = 20.0f;   // gravidade medida (m/s^2)
    float        pitch       = 0.0f;    // inclinacao propria da camera (rad)
    bool         pitchInit   = false;
    float        dirX        = 0.0f, dirY = 1.0f; // direcao horizontal da camera
    bool         launched    = false;   // lancamos um pulo no frame anterior
    float        launchV0    = 0.0f;
    int          launchFails = 0;
    float        rawLook     = 0.0f;
    qm::JumpState js;
    qm::AxisVote calF, calR;
    unsigned     frame       = 0;

    void Reset()
    {
        active = false;
        vel = lastWritten = { 0, 0 };
        wasOnGround = true;
    }
} st;

static bool g_hasTaskChecks = false;
static bool g_hasCamHook    = false;
static int  g_jumpField     = -1;     // indice (em i16) do botao de pulo dentro de CControllerState
static int  g_jumpFindTries = 0;

static float g_origNear       = 0.0f;
static bool  g_nearOverridden = false;

static const int kPadFields = (int)(sizeof(CControllerState) / sizeof(i16));

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

// Condicoes minimas: o jogador esta a pe, vivo e com controle (sem cutscene).
static bool BaseOk(CPlayerPed* ped, CPad* pad)
{
    if (!g.enabled || !pad) return false;
    if (pad->DisablePlayerControls) return false;
    if (ped->bInVehicle) return false;
    if (ped->m_fHealth <= 0.0f) return false;
    return true;
}

// Condicoes para a fisica Quake assumir a velocidade.
static bool MoveOk(CPlayerPed* ped)
{
    if (ped->bIsAimingGun || ped->bIsDucking) return false; // mirando/agachado: o jogo move relativo a mira
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

static bool IsFpCamMode(int mode)
{
    if (mode == MODE_FOLLOWPED || mode == MODE_FOLLOW_PED_WITH_BIND) return true;
    // mirando com arma a pe: a camera de mira do jogo tambem vira 1a pessoa (a direcao da mira e mantida)
    return g.firstPersonAim && mode == MODE_AIMWEAPON;
}

// ---------------------------------------------------------------- botao de pulo

// Descobre qual campo do CControllerState o jogo le como "pulo": zera um campo por vez e ve se GetJump() cai.
// Assim nao dependemos de saber se e Square/Cross/etc. neste port Android.
static void FindJumpField(CPad* pad)
{
    i16* f = (i16*)&pad->NewState;
    int found = -1, count = 0;
    for (int k = 0; k < kPadFields; k++)
    {
        if (f[k] == 0) continue;
        i16 saved = f[k];
        f[k] = 0;
        bool still = pad->GetJump();
        f[k] = saved;
        if (!still)
        {
            if (found < 0) found = k;
            count++;
        }
    }
    g_jumpFindTries++;
    if (found >= 0)
    {
        g_jumpField = found;
        logger->Info("campo do pulo encontrado: indice %d (candidatos: %d)", found, count);
    }
    else if (g_jumpFindTries == 5)
    {
        logger->Error("nao achei o campo do pulo no pad - pulo proprio desativado (usa o pulo do jogo)");
    }
}

// ---------------------------------------------------------------- helpers de camera / corpo

static void ForceHeading(CPlayerPed* p, float yaw)
{
    p->m_fCurrentRotation = yaw;
    p->m_fAimingRotation  = yaw;
    if (p->m_matrix)
    {
        float c = cosf(yaw), s = sinf(yaw);
        p->m_matrix->right = { c, s, 0.0f };
        p->m_matrix->up    = { -s, c, 0.0f };
        p->m_matrix->at    = { 0.0f, 0.0f, 1.0f };
    }
}

static void SetNearClip(float v)
{
    if (v <= 0.0f) return;
    if (!g_nearOverridden)
    {
        g_origNear = CDraw::ms_fNearClipZ;
        g_nearOverridden = true;
    }
    CDraw::ms_fNearClipZ = v;
}

static void RestoreNearClip()
{
    if (g_nearOverridden)
    {
        CDraw::ms_fNearClipZ = g_origNear;
        g_nearOverridden = false;
    }
}

// ---------------------------------------------------------------- hook: camera (1a pessoa)

DECL_HOOKv(CCam_Process, CCam* self)
{
    CCam_Process(self);

    if (self != &TheCamera.GetActiveCam()) return;

    int mode = (int)self->m_nMode;
    if (!g.firstPerson || !st.fpActive || !st.ped || !IsFpCamMode(mode))
    {
        RestoreNearClip();
        return;
    }

    float fx = self->m_vecFront.x, fy = self->m_vecFront.y, fz = self->m_vecFront.z;
    float len = sqrtf(fx * fx + fy * fy + fz * fz);
    if (len < 0.001f) return;
    fx /= len; fy /= len; fz /= len;

    // olhar vertical PROPRIO: a camera de 3a pessoa do jogo limita muito o olhar para cima/baixo.
    float hz = sqrtf(fx * fx + fy * fy);
    float gamePitch = atan2f(fz, hz);
    if (hz > 0.05f) { st.dirX = fx / hz; st.dirY = fy / hz; }
    if (!st.pitchInit || mode == MODE_AIMWEAPON)
    {
        st.pitch = gamePitch;       // mirando: vale a inclinacao do jogo
        st.pitchInit = true;
    }
    else
    {
        CPad* lp = CPad::GetPad(0);
        float raw = lp ? (float)lp->LookAroundUpDown() : 0.0f;
        if (raw == 0.0f && lp) raw = (float)lp->NewState.RightStickY;
        st.rawLook = raw;
        float r = raw / 128.0f;
        if (r > 4.0f) r = 4.0f;
        if (r < -4.0f) r = -4.0f;
        float ts = CTimer::ms_fTimeStep;
        if (!(ts > 0.0f) || ts > 5.0f) ts = 1.0f;
        float sgn = g.pitchInvert ? 1.0f : -1.0f;
        st.pitch += sgn * r * g.pitchSens * ts * 0.02f;
        if (st.pitch > 1.5f) st.pitch = 1.5f;
        if (st.pitch < -1.5f) st.pitch = -1.5f;
    }
    {
        float ch = cosf(st.pitch);
        fx = st.dirX * ch;
        fy = st.dirY * ch;
        fz = sinf(st.pitch);
        self->m_vecFront = { fx, fy, fz };
    }

    CVector pos = st.ped->GetPosition();
    float eye = g.eyeHeight - (st.ped->bIsDucking ? 0.30f : 0.0f);
    self->m_vecSource = { pos.x + fx * g.eyeForward, pos.y + fy * g.eyeForward, pos.z + eye + fz * g.eyeForward };
    self->m_vecUp     = { 0.0f, 0.0f, 1.0f };
    if (g.fov > 1.0f) self->m_fFOV = g.fov;
    SetNearClip(g.nearClip);
}

// ---------------------------------------------------------------- hook: jogador

DECL_HOOKv(CPlayerPed_ProcessControl, CPlayerPed* self)
{
    if (self != FindPlayerPed(-1))
    {
        CPlayerPed_ProcessControl(self);
        return;
    }
    if (st.ped != self) { st.Reset(); st.ped = self; }

    CPad* pad = CPad::GetPad(0);
    bool base = BaseOk(self, pad);
    bool move = base && MoveOk(self);
    float now = (float)((unsigned)CTimer::GetTimeMS()) * 0.001f;

    // velocidade que o motor deixou depois da colisao do frame anterior
    qm::V2 pre = { self->m_vecMoveSpeed.x * 50.0f, self->m_vecMoveSpeed.y * 50.0f };
    float preZ = self->m_vecMoveSpeed.z * 50.0f;

    // --- entrada de pulo (lida ANTES de mascarar) e mascara do botao para o jogo
    bool down = false, edge = false;
    bool maskNew = false, maskOld = false;
    i16 savedNew = 0, savedOld = 0;
    if (base)
    {
        down = pad->GetJump();
        edge = pad->JumpJustDown();
        if (g_jumpField < 0 && down && g_jumpFindTries < 5) FindJumpField(pad);

        if (move && g_jumpField >= 0)
        {
            if (g.ownJump)
            {
                // o pulo e nosso: o jogo nao pode ver o botao (senao pula duas vezes / trava no pouso)
                i16* nf = (i16*)&pad->NewState;
                savedNew = nf[g_jumpField];
                nf[g_jumpField] = 0;
                maskNew = true;
            }
            else if (g.autoHop && down && self->bIsStanding)
            {
                // pulo nativo + autohop: finge que o botao acabou de ser apertado
                i16* of = (i16*)&pad->OldState;
                savedOld = of[g_jumpField];
                of[g_jumpField] = 0;
                maskOld = true;
            }
        }
    }

    CPlayerPed_ProcessControl(self);   // logica original (tasks, animacoes, input...)

    if (maskNew) ((i16*)&pad->NewState)[g_jumpField] = savedNew;
    if (maskOld) ((i16*)&pad->OldState)[g_jumpField] = savedOld;

    float ts = CTimer::ms_fTimeStep;               // 1.0 = 20 ms de jogo
    if (!base || !(ts > 0.0001f))
    {
        st.Reset();
        st.fpActive = false;
        return;
    }
    float dt = ts * 0.02f;
    if (dt > 0.1f) dt = 0.1f;

    bool onGround = self->bIsStanding;

    // --- o pulo proprio funcionou? (o motor deve ter mantido a velocidade vertical para cima)
    if (st.launched)
    {
        st.launched = false;
        if (preZ < st.launchV0 * 0.4f)
        {
            if (++st.launchFails >= 3 && g.ownJump)
            {
                g.ownJump = false;
                logger->Error("pulo proprio nao pegou (vz=%.2f de %.2f) - voltando ao pulo nativo com autohop", preZ, st.launchV0);
            }
        }
        else st.launchFails = 0;
    }

    // --- gravidade real do jogo (para a altura do pulo ficar exata)
    if (!onGround)
        st.gEst = qm::EstimateGravity(st.gEst, preZ, self->m_vecMoveSpeed.z * 50.0f, dt);

    // --- base de movimento: vetor "front" da camera ativa (direcao do olhar)
    CCam& acam = TheCamera.GetActiveCam();
    qm::View view = qm::MakeView(acam.m_vecFront.x, acam.m_vecFront.y);
    int camMode = (int)acam.m_nMode;

    // --- entrada do analogico (sinais canonicos do SA: cima = negativo)
    float rawF = -(float)pad->GetPedWalkUpDown() / 128.0f;
    float rawR =  (float)pad->GetPedWalkLeftRight() / 128.0f;
    rawF = rawF > 1.0f ? 1.0f : (rawF < -1.0f ? -1.0f : rawF);
    rawR = rawR > 1.0f ? 1.0f : (rawR < -1.0f ? -1.0f : rawR);
    float smag = sqrtf(rawF * rawF + rawR * rawR);

    // --- calibracao: compara com a direcao que o PROPRIO jogo quer andar (m_fAimingRotation)
    if (g.autoCalibrate && view.valid && smag > 0.5f && onGround && !self->bIsAimingGun)
    {
        float th = self->m_fAimingRotation;
        if (isfinite(th))
        {
            qm::V2 dg = { -sinf(th), cosf(th) };
            float pf = qm::Dot(dg, view.fwd);
            float pr = qm::Dot(dg, view.right);
            if (!st.calF.decided && fabsf(rawF) > 0.4f && fabsf(pf) > 0.35f)
                st.calF.Add((pf > 0.0f) == (rawF > 0.0f));
            if (!st.calR.decided && fabsf(rawR) > 0.4f && fabsf(pr) > 0.35f)
                st.calR.Add((pr > 0.0f) == (rawR > 0.0f));

            int d = st.calF.decided ? 0 : st.calF.Decide(40, 0.85f);
            if (d != 0)
            {
                st.calF.decided = true;
                g.flipFwd = (d < 0);
                logger->Info("calibracao: frente/tras %s", d < 0 ? "INVERTIDO (corrigido)" : "ok");
            }
            d = st.calR.decided ? 0 : st.calR.Decide(40, 0.85f);
            if (d != 0)
            {
                st.calR.decided = true;
                g.flipStrafe = (d < 0);
                logger->Info("calibracao: esquerda/direita %s", d < 0 ? "INVERTIDO (corrigido)" : "ok");
            }
        }
    }

    float inF = g.flipFwd ? -rawF : rawF;
    float inR = g.flipStrafe ? -rawR : rawR;

    // --- 1a pessoa: o corpo olha para onde a camera olha (e a camera nao "foge" para tras do CJ)
    st.fpActive = g.firstPerson && g_hasCamHook;
    if (st.fpActive && view.valid && IsFpCamMode(camMode) && !self->bIsAimingGun)
        ForceHeading(self, view.yaw);

    if (!move)
    {
        // sem fisica Quake agora (mirando, agachado, nadando...)
        st.Reset();
        if (g.ownJump && g_jumpField >= 0) qm::JumpUpdate(st.js, now, false, down, edge, g.autoHop, 0.0f, 0.0f);
        return;
    }

    // --- empurroes externos (colisao com parede, explosao); o arrasto do motor e ignorado
    if (st.active)
    {
        qm::V2 d;
        if (qm::ExternalDelta(pre, st.lastWritten, d) && (!st.wasOnGround || self->bPedHitWallLastFrame))
        {
            st.vel.x += d.x;
            st.vel.y += d.y;
        }
    qm::Wish wish = qm::MakeWish(view.fwd, view.right, inF, inR, g.p.maxSpeed);

    // --- pulo proprio
    bool ownJumpOn = g.ownJump && g_jumpField >= 0;
    bool wantJump = false;
    if (ownJumpOn)
        wantJump = qm::JumpUpdate(st.js, now, onGround, down, edge, g.autoHop, g.jumpBufferMs * 0.001f, 0.12f);

    // parado, sem input e sem pulo: devolve o controle ao jogo (idle normal, scripts, etc.)
    bool holdHop = !ownJumpOn && g.autoHop && down && onGround;
    if (onGround && !wantJump && !holdHop && wish.speed <= 0.0f && qm::Len(st.vel) < 0.2f && (!st.active || qm::Len(pre) < 0.2f))
    {
        st.active = false;
        st.wasOnGround = onGround;
        return;
    }

    if (!st.active)
    {
        st.vel = pre;            // comeca de onde o jogo estava
        st.active = true;
        st.wasOnGround = onGround;
    }

    // --- decolagem
    if (wantJump)
    {
        qm::PreventMegaBhop(st.vel, g.p);      // so faz algo se BhopCap > 0 (GoldSrc)
        float v0 = qm::JumpSpeed(g.p.jumpHeight, st.gEst);
        self->m_vecMoveSpeed.z = v0 / 50.0f;
        self->bIsStanding = false;
        st.launched = true;
        st.launchV0 = v0;
        if (g.debug) logger->Info("PULO v0=%.2f m/s g=%.1f vel=%.2f", v0, st.gEst, qm::Len(st.vel));
    }

    // --- um passo de fisica Quake/GoldSrc.
    // Frame do pulo conta como "ar": nao tem atrito (e isso que conserva a velocidade no bhop).
    bool groundPhys = onGround && !wantJump;
    bool noFric = holdHop;
    qm::Step(st.vel, groundPhys, noFric, wish, dt, g.p);

    // --- escreve de volta (m/s -> unidades por tick de 20 ms)
    self->m_vecMoveSpeed.x = st.vel.x * 0.02f;
    self->m_vecMoveSpeed.y = st.vel.y * 0.02f;
    st.lastWritten = st.vel;
    st.wasOnGround = onGround && !wantJump;

    if (g.debug && (st.frame++ % 60) == 0)
    {
        logger->Info("look=%.0f pitch=%.2f vel=%.2f m/s ground=%d wish=%.2f jumpDown=%d field=%d g=%.1f cam=%d fp=%d flipF=%d flipR=%d votesF=%d/%d votesR=%d/%d state=%u",
                     st.rawLook, st.pitch, qm::Len(st.vel), (int)onGround, wish.speed, (int)down, g_jumpField, st.gEst, camMode, (int)st.fpActive,
                     (int)g.flipFwd, (int)g.flipStrafe, st.calF.agree, st.calF.disagree, st.calR.agree, st.calR.disagree,
                     (unsigned)self->m_ePedState);
    }
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

    // Se algum simbolo essencial nao existir nessa versao do jogo, nao instala nada (o jogo roda normal).
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
    g_hasCamHook = (aml->GetSym(hGTASA, "_ZN4CCam7ProcessEv") != 0);

    // ---- Config (arquivo .ini criado na primeira execucao, pode editar sem recompilar)
    char preset[16] = {0};
    cfg->Bind("Preset", "hl", "QuakeMove")->GetString(preset, sizeof(preset));   // hl | csgo | quake
    delete Config::pLastEntry;
    if (!qm::ApplyPreset(g.p, preset))
    {
        logger->Error("Preset '%s' desconhecido (use hl, csgo ou quake) - usando hl", preset);
        qm::ApplyPreset(g.p, "hl");
    }

    g.enabled       = CfgB("Enabled", true);
    g.autoHop       = CfgB("AutoHop", true);
    g.ownJump       = CfgB("OwnJump", true);
    g.firstPerson   = CfgB("FirstPerson", true);
    g.firstPersonAim = CfgB("FirstPersonAim", true);
    g.autoCalibrate = CfgB("AutoCalibrate", true);
    g.flipFwd       = CfgB("FlipForward", false);
    g.flipStrafe    = CfgB("FlipStrafe", false);
    g.debug         = CfgB("Debug", false);
    g.fov           = CfgF("FOV", g.fov);
    g.nearClip      = CfgF("NearClip", g.nearClip);
    g.eyeHeight     = CfgF("EyeHeight", g.eyeHeight);
    g.eyeForward    = CfgF("EyeForward", g.eyeForward);
    g.pitchSens     = CfgF("PitchSens", g.pitchSens);
    g.pitchInvert   = CfgB("PitchInvert", false);
    g.jumpBufferMs  = CfgI("JumpBufferMs", g.jumpBufferMs);

    g.p.maxSpeed      = CfgF("MaxSpeed", g.p.maxSpeed);           // m/s
    g.p.accelerate    = CfgF("Accelerate", g.p.accelerate);
    g.p.airAccelerate = CfgF("AirAccelerate", g.p.airAccelerate);
    g.p.airWishCap    = CfgF("AirWishCap", g.p.airWishCap);       // m/s (30 ups = 0.762)
    g.p.friction      = CfgF("Friction", g.p.friction);
    g.p.stopSpeed     = CfgF("StopSpeed", g.p.stopSpeed);         // m/s
    g.p.maxVelocity   = CfgF("MaxVelocity", g.p.maxVelocity);     // m/s
    g.p.bhopCap       = CfgF("BhopCap", g.p.bhopCap);             // 0 = sem limite, 1.7 = GoldSrc
    g.p.jumpHeight    = CfgF("JumpHeight", g.p.jumpHeight);       // metros
    cfg->Save();

    HOOK(CPlayerPed_ProcessControl, GetNativeVtable<CPlayerPed>("_ZTV10CPlayerPed")[11]);
    if (g_hasCamHook)
    {
        HOOKSYM(CCam_Process, hGTASA, "_ZN4CCam7ProcessEv");   // a macro ja termina com ';'
    }
    else
    {
        logger->Error("CCam::Process nao encontrado - primeira pessoa desativada");
    }

    logger->Info("QuakeMove v2 carregado (preset=%s maxspeed=%.2f accel=%.1f airaccel=%.1f jump=%.2fm autohop=%d ownjump=%d fp=%d)",
                 preset, g.p.maxSpeed, g.p.accelerate, g.p.airAccelerate, g.p.jumpHeight,
                 (int)g.autoHop, (int)g.ownJump, (int)(g.firstPerson && g_hasCamHook));
}
