/* earth_map.h - where the land is (see earth_map.c) */
#pragma once
#define EARTH_W 128
#define EARTH_H 64
extern const unsigned char earth_land[EARTH_H][EARTH_W / 2];
/* how much of the spot at column x (0..127), row y (0..63) is land, 0..15 */
static inline int earth_at(int x, int y) {
    x = ((x % EARTH_W) + EARTH_W) % EARTH_W;
    y = y < 0 ? 0 : y >= EARTH_H ? EARTH_H - 1 : y;
    unsigned char b = earth_land[y][x >> 1];
    return (x & 1) ? (b & 15) : (b >> 4);
}
