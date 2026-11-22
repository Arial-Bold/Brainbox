/* bbmath.h - sinf/cosf that stay fast for large angles.
 *
 * newlib's sinf/cosf switch to a slow, exact reduction once the angle is
 * past about 200 radians, and the animation passes angles like t * 18
 * (t in seconds), so a few minutes in, every one of those calls took the
 * slow path. This brings the angle back near zero first (in double, which
 * costs the same as float on the GameCube's FPU), then calls the real one.
 * Include after <math.h>. */
#pragma once
#include <math.h>

static inline float bb_wrap_angle(float x) {
    if (x > 200.0f || x < -200.0f) {
        double d = x;
        d -= 6.283185307179586 * floor(d * 0.15915494309189535 + 0.5);
        x = (float)d;
    }
    return x;
}
static inline float bb_sinf(float x) { return (sinf)(bb_wrap_angle(x)); }
static inline float bb_cosf(float x) { return (cosf)(bb_wrap_angle(x)); }

#define sinf(x) bb_sinf(x)
#define cosf(x) bb_cosf(x)

/* (1.4) A quick square root. The GameCube's processor has no square root
 * instruction, so sqrtf is worked out bit by bit in software, and shaping
 * and lighting the hologram calls it thousands of times a frame. This uses
 * the processor's reciprocal square root estimate, sharpened twice
 * (Newton's method): as good as sqrtf for drawing, and far quicker. */
#ifdef GEKKO
static inline float bb_sqrtf(float x) {
    if (!(x > 0)) return 0;
    double e;
    __asm__("frsqrte %0,%1" : "=f"(e) : "f"((double)x));
    double y = e;
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    y = y * (1.5 - 0.5 * x * y * y);
    return (float)(x * y);
}
#define sqrtf(x) bb_sqrtf(x)
#endif
