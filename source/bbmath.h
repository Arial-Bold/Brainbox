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
