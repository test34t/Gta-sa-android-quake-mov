// quake_move.h  (v2)
// Física de movimento estilo Quake / GoldSrc (Half-Life) / CS:GO, sem nenhuma dependência do jogo.
// Tudo aqui trabalha em METROS e SEGUNDOS, no plano horizontal (XY), exceto o pulo (eixo Z).
// Baseado em PM_Friction / PM_Accelerate / PM_AirAccelerate do GoldSrc.
#pragma once
#include <math.h>
#include <string.h>
#include <ctype.h>

namespace qm
{
    struct V2
    {
        float x, y;
    };

    inline float Dot(V2 a, V2 b)  { return a.x * b.x + a.y * b.y; }
    inline float Len(V2 a)        { return sqrtf(a.x * a.x + a.y * a.y); }
    inline V2    Norm(V2 a)       { float l = Len(a); return l > 1e-6f ? V2{ a.x / l, a.y / l } : V2{ 0.0f, 0.0f }; }

    struct Params
    {
        // Valores = cvars do Half-Life convertidos de "units" para metros (1 unit = 0.0254 m)
        float maxSpeed      = 8.13f;  // sv_maxspeed 320
        float accelerate    = 10.0f;  // sv_accelerate
        float airAccelerate = 10.0f;  // sv_airaccelerate
        float airWishCap    = 0.762f; // o limite de 30 ups no ar (é ele que permite o air-strafe)
        float friction      = 4.0f;   // sv_friction
        float stopSpeed     = 2.54f;  // sv_stopspeed (100 ups)
        float maxVelocity   = 25.0f;  // trava de segurança (evita atravessar paredes)
        float bhopCap       = 1.7f;   // 0 = sem limite (Quake/CS:GO). 1.7 = GoldSrc (PM_PreventMegaBunnyJumping)
        float jumpHeight    = 1.14f;  // altura do pulo em metros (HL = 45 units)
    };

    // Presets: "hl" (Half-Life 1), "csgo", "quake" (Quake 3 / VQ3-like)
    inline bool ApplyPreset(Params& p, const char* name)
    {
        char n[16] = {0};
        for (int i = 0; i < 15 && name && name[i]; i++) n[i] = (char)tolower((unsigned char)name[i]);

        if (!strcmp(n, "hl") || !strcmp(n, "halflife") || !strcmp(n, "goldsrc"))
        {
            p = Params();
            return true;
        }
        if (!strcmp(n, "csgo") || !strcmp(n, "cs"))
        {
            p = Params();
            p.maxSpeed = 6.35f;        // 250 ups
            p.accelerate = 5.5f;
            p.airAccelerate = 12.0f;
            p.friction = 5.2f;
            p.stopSpeed = 2.03f;       // 80 ups
            p.bhopCap = 0.0f;
            p.jumpHeight = 1.45f;      // 57 units
            return true;
        }
        if (!strcmp(n, "quake") || !strcmp(n, "q3"))
        {
            p = Params();
            p.maxSpeed = 8.13f;        // 320 ups
            p.accelerate = 10.0f;
            p.airAccelerate = 1.0f;
            p.friction = 6.0f;
            p.stopSpeed = 2.54f;
            p.bhopCap = 0.0f;
            p.jumpHeight = 1.20f;
            return true;
        }
        return false;
    }

    // PM_Friction (chão)
    inline void Friction(V2& v, float dt, const Params& p)
    {
        float speed = Len(v);
        if (speed < 0.003f)
        {
            v = { 0.0f, 0.0f };
            return;
        }
        float control = (speed < p.stopSpeed) ? p.stopSpeed : speed;
        float drop = control * p.friction * dt;
        float news = speed - drop;
        if (news < 0.0f) news = 0.0f;
        float k = news / speed;
        v.x *= k;
        v.y *= k;
    }

    // PM_Accelerate (chão)
    inline void Accelerate(V2& v, V2 wishdir, float wishspeed, float accel, float dt)
    {
        float current = Dot(v, wishdir);
        float add = wishspeed - current;
        if (add <= 0.0f) return;
        float as = accel * dt * wishspeed;
        if (as > add) as = add;
        v.x += as * wishdir.x;
        v.y += as * wishdir.y;
    }

    // PM_AirAccelerate (ar) - a diferença para o chão é o "cap" em wishspd
    inline void AirAccelerate(V2& v, V2 wishdir, float wishspeed, float accel, float cap, float dt)
    {
        float wishspd = (wishspeed > cap) ? cap : wishspeed;
        float current = Dot(v, wishdir);
        float add = wishspd - current;
        if (add <= 0.0f) return;
        float as = accel * wishspeed * dt;
        if (as > add) as = add;
        v.x += as * wishdir.x;
        v.y += as * wishdir.y;
    }

    // Entrada normalizada: wishdir (unitário) e wishspeed em m/s.
    struct Wish
    {
        V2 dir;
        float speed;
    };

    inline Wish MakeWish(V2 camFwd, V2 camRight, float fwdInput, float rightInput, float maxSpeed)
    {
        V2 w = { camFwd.x * fwdInput + camRight.x * rightInput,
                 camFwd.y * fwdInput + camRight.y * rightInput };
        float mag = Len(w);
        Wish out;
        if (mag < 0.001f)
        {
            out.dir = { 0.0f, 0.0f };
            out.speed = 0.0f;
            return out;
        }
        out.dir = { w.x / mag, w.y / mag };
        if (mag > 1.0f) mag = 1.0f;
        out.speed = mag * maxSpeed;
        return out;
    }

    // Um passo de física. onGround = está pisando no chão.
    // noFriction = pulo segurado/agendado: no pouso não aplica atrito (é isso que faz o bunny hop conservar velocidade).
    inline void Step(V2& v, bool onGround, bool noFriction, Wish wish, float dt, const Params& p)
    {
        if (onGround && !noFriction)
        {
            Friction(v, dt, p);
            if (wish.speed > 0.0f)
                Accelerate(v, wish.dir, wish.speed, p.accelerate, dt);
        }
        else
        {
            if (wish.speed > 0.0f)
                AirAccelerate(v, wish.dir, wish.speed, p.airAccelerate, p.airWishCap, dt);
        }

        float s = Len(v);
        if (s > p.maxVelocity)
        {
            float k = p.maxVelocity / s;
            v.x *= k;
            v.y *= k;
        }
    }

    // GoldSrc: PM_PreventMegaBunnyJumping. Chamar no frame em que o jogador sai do chão pulando.
    inline void PreventMegaBhop(V2& v, const Params& p)
    {
        if (p.bhopCap <= 0.0f) return;
        float maxScaled = p.maxSpeed * p.bhopCap;
        float spd = Len(v);
        if (spd > maxScaled && spd > 0.0f)
        {
            float k = (maxScaled / spd) * 0.65f;
            v.x *= k;
            v.y *= k;
        }
    }

    // ------------------------------------------------------------------ pulo (determinístico)

    // v0 = sqrt(2 g h): toda vez o mesmo pulo, independente de animação / frame rate.
    inline float JumpSpeed(float height, float gravity)
    {
        if (gravity < 5.0f || gravity > 60.0f) gravity = 20.0f;
        float v = sqrtf(2.0f * gravity * height);
        if (v < 2.0f) v = 2.0f;
        if (v > 14.0f) v = 14.0f;
        return v;
    }

    // Mede a gravidade real do jogo: queda de velocidade vertical num frame no ar (sem nenhum impulso nosso).
    // zPre/zPost em m/s. Devolve a média suavizada.
    inline float EstimateGravity(float current, float zPre, float zPost, float dt)
    {
        if (dt < 0.004f) return current;
        float g = (zPre - zPost) / dt;
        if (g < 8.0f || g > 60.0f) return current;
        return current * 0.9f + g * 0.1f;
    }

    struct JumpState
    {
        float lastPress  = -100.0f;  // instante do último "apertou"
        float lastLaunch = -100.0f;  // instante do último pulo lançado
        bool  prevHeld   = false;
    };

    // Decide se lança um pulo AGORA.
    //  - autoHop: segurar = pula de novo assim que pousa.
    //  - bufferSec: apertar um pouco ANTES de pousar também vale (pulo consistente, estilo CS).
    //  - minAirSec: trava anti-duplo (o flag de "no chão" pode demorar 1 frame para cair).
    inline bool JumpUpdate(JumpState& js, float now, bool onGround, bool held, bool edge,
                           bool autoHop, float bufferSec, float minAirSec)
    {
        bool pressed = edge || (held && !js.prevHeld);
        if (pressed) js.lastPress = now;
        js.prevHeld = held;

        if (!onGround) return false;
        if (now - js.lastLaunch < minAirSec) return false;

        bool want = (held && autoHop) || (now - js.lastPress <= bufferSec);
        if (want)
        {
            js.lastLaunch = now;
            js.lastPress = -100.0f;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ empurrões externos

    // Diferença entre a velocidade que o motor devolveu e a que nós escrevemos.
    // Arrasto do ar do motor (~1% por tick) NÃO conta: só colisão/empurrão/explosão.
    inline bool ExternalDelta(V2 pre, V2 lastWritten, V2& delta)
    {
        V2 d = { pre.x - lastWritten.x, pre.y - lastWritten.y };
        float thr = 0.25f + 0.03f * Len(lastWritten);
        if (Len(d) <= thr) return false;
        delta = d;
        return true;
    }

    // ------------------------------------------------------------------ calibração de eixos

    // Vota se o sinal de um eixo do analógico está certo, comparando com a direção que o PRÓPRIO jogo quer andar.
    struct AxisVote
    {
        int agree = 0;
        int disagree = 0;
        bool decided = false;

        void Add(bool agrees) { if (agrees) agree++; else disagree++; }

        // +1 = sinal certo, -1 = sinal invertido, 0 = ainda sem certeza
        int Decide(int minVotes, float ratio)
        {
            int n = agree + disagree;
            if (n < minVotes) return 0;
            if ((float)agree >= ratio * (float)n) return +1;
            if ((float)disagree >= ratio * (float)n) return -1;
            return 0;
        }
    };

    // Base de movimento a partir do vetor "front" da câmera (direção do olhar), sem depender de matriz de view.
    struct View
    {
        V2 fwd;    // frente no plano XY (unitário)
        V2 right;  // direita no plano XY (unitário)
        float yaw; // heading no padrão do GTA: frente = (-sin, cos)
        bool valid;
    };

    inline View MakeView(float frontX, float frontY)
    {
        View v;
        float l = sqrtf(frontX * frontX + frontY * frontY);
        if (l < 0.001f)
        {
            v.fwd = { 0.0f, 1.0f }; v.right = { 1.0f, 0.0f }; v.yaw = 0.0f; v.valid = false;
            return v;
        }
        v.fwd = { frontX / l, frontY / l };
        v.right = { v.fwd.y, -v.fwd.x };
        v.yaw = atan2f(-v.fwd.x, v.fwd.y);
        v.valid = true;
        return v;
    }
}
