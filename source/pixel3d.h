/* pixel3d.h - Professor Pixel himself: his head in 3D, posed, lit and drawn
 * with the GPU (called from the drawing thread, in ui.c). */
#pragma once
#include <stdbool.h>
#include <gccore.h>

typedef struct {
    float t;          /* seconds (for the gentle movements: bobbing, the bow tie, the props) */
    float talk;       /* how open his mouth is, 0..1 (from his voice) */
    float blink;      /* 0 open .. 1 shut */
    float think;      /* 0 .. 1: spectacles down, one brow up, a glance up and aside */
    float brows;      /* extra eyebrow lift (surprise, delight) */
    float chuckle;    /* 0..1: a little shake and the bow tie bouncing */
    float look_x;     /* eyes left (-1) .. right (1) */
    float scribble;   /* how busily the pencil scribbles, 0..1 */
    bool props;       /* the globe and the pencil */
} PixelPose;

typedef struct { float x0, y0, x1, y1; } PixelBox;   /* where he was drawn */

/* Makes him in this pose (shape and lighting); once a frame. */
void pixel3d_build(const PixelPose *pose);
/* Draws him with GX_VTXFMT1 (position x, y, z; colour; texture coordinate),
 * which the caller sets up, with the depth test on: his middle at x, y in
 * the 640x480 layout, scale 1 as in the mock-ups. mirror_y > -100: upside
 * down about that height in his own space (the floor reflection). */
void pixel3d_draw(float x, float y, float scale, float alpha, float mirror_y, PixelBox *box);
int  pixel3d_triangles(void);
