/* head3d.h - Professor Scotty himself: his head in 3D, posed, lit and drawn
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
} HeadPose;

typedef struct { float x0, y0, x1, y1; } HeadBox;   /* where he was drawn */

/* Makes him in this pose (shape and lighting); once a frame. */
void head3d_build(const HeadPose *pose);
/* The visible part of the 640x480 layout (as the 2D drawing's projection
 * has it), and whether the depth test is turned round (display.depth_flip). */
void head3d_view(float l, float t, float r, float b, bool depth_flip);
/* Draws him with the GPU: his middle at x, y in the 640x480 layout, scale 1
 * as in the mock-ups. mirror_y > -100: upside down about that height in his
 * own space (the floor reflection). It changes the projection, the vertex
 * description (GX_VTXFMT2), the TEV, the matrix and the depth test: the
 * caller sets its own back up afterwards. */
void head3d_draw(float x, float y, float scale, float alpha, float mirror_y, HeadBox *box);
int  head3d_triangles(void);
/* his colours are multiplied by this when drawn (white: as they are; a pale blue for a ghostly hologram) */
void head3d_tint(u8 r, u8 g, u8 b);
