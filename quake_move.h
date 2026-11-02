// quake_move.h
// Física de movimento estilo Quake / GoldSrc (Half-Life), sem nenhuma dependência do jogo.
// Tudo aqui trabalha em METROS e SEGUNDOS, no plano horizontal (XY).
// Baseado em PM_Friction / PM_Accelerate / PM_AirAccelerate do GoldSrc.
#pragma once
#include <math.h>

namespace qm
{
    struct V2
    {
        float x, y;
    };

    inline float Dot(V2 a, V2 b)  { return a.x * b.x + a.y * b.y; }
    inline float Len(V2 a)        { return sqrtf(a.x * a.x + a.y * a.y); }

    struct Params
    {
        // Valores padrão = cvars do Half-Life convertidos de "units" para metros (1 unit = 0.0254 m)
        float maxSpeed      = 7.0f;   // sv_maxspeed (HL: 320 ups = 8.1 m/s; 7.0 combina melhor com o sprint do CJ)
        float accelerate    = 10.0f;  // sv_accelerate
        float airAccelerate = 10.0f;  // sv_airaccelerate
        float airWishCap    = 0.762f; // o famoso limite de 30 ups no ar (é ele que permite o air-strafe)
        float friction      = 4.0f;   // sv_friction
        float stopSpeed     = 2.54f;  // sv_stopspeed (100 ups)
        float maxVelocity   = 20.0f;  // trava de segurança (evita atravessar paredes)
        float bhopCap       = 0.0f;   // 0 = Quake (sem limite). 1.7 = GoldSrc (PM_PreventMegaBunnyJumping)
    };

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
    // inputMag vai de 0 a 1 (analógico do joystick).
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
    // jumpHeld = jogador segurando pulo com autohop (não aplica atrito, como no Quake ao pular no frame do pouso).
    inline void Step(V2& v, bool onGround, bool jumpHeld, Wish wish, float dt, const Params& p)
    {
        if (onGround && !jumpHeld)
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
}
