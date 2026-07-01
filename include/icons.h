#pragma once
#include <stdint.h>

// 8×10 monochrome icon masks (1 bit per pixel, MSB = leftmost pixel of each row).
// Rendered by DisplayManager::_drawIcon() in the row's foreground color on CLR_BG.
//
// Bit ordering: bit7=col0 (left), bit0=col7 (right)
// Each array has 10 entries — one byte per pixel row.

// Map pin — Lat / Lon rows
//  ...**...
//  ..*..*.
//  .*....*.
//  ..*..*.
//  ...**...
//  ...**...
//  ....*...   ← tip
//  ....*...
static const uint8_t ICON_GPS[10] = {
    0x18,   // ...**...
    0x24,   // ..*..*.
    0x42,   // .*....*.
    0x24,   // ..*..*.
    0x18,   // ...**...
    0x18,   // ...**...
    0x08,   // ....*...
    0x08,   // ....*...
    0x00,
    0x00,
};

// Arrow right — speed row when moving
//  ........
//  ........
//  ....*...
//  .....*..
//  xxxxxxx.   ← full shaft
//  .....*..
//  ....*...
//  ........
static const uint8_t ICON_ARROW[10] = {
    0x00,
    0x00,
    0x08,   // ....*...
    0x04,   // .....*..
    0xFE,   // xxxxxxx.
    0x04,   // .....*..
    0x08,   // ....*...
    0x00,
    0x00,
    0x00,
};

// Pause bars — speed row when idle
//  ........
//  .**..**. ← two 2-pixel-wide bars
//  .**..**. (rows 1–8)
//  ........
static const uint8_t ICON_PAUSE[10] = {
    0x00,
    0x66,   // .**..**
    0x66,
    0x66,
    0x66,
    0x66,
    0x66,
    0x66,
    0x66,
    0x00,
};

// Antenna + arc — TX status row
//  ........
//  .*....*.   ← outer arc
//  ..*..*.    ← inner arc
//  ...**...   ← peak (2px)
//  ....*...   ← mast
//  ....*...
//  ....*...
//  ....*...
//  .*****.    ← base
static const uint8_t ICON_ANTENNA[10] = {
    0x00,
    0x42,   // .*....*.
    0x24,   // ..*..*.
    0x18,   // ...**...
    0x08,   // ....*...
    0x08,
    0x08,
    0x08,
    0x7C,   // .*****..
    0x00,
};

// Analog clock — countdown row
//  .******.   ← top arc
//  *......*
//  *..*...*   ← 12-o'clock mark at col3
//  *..*...*
//  *..***.*   ← 3-o'clock hand (cols 3-5)
//  *......*
//  *......*
//  *......*
//  .******.   ← bottom arc
static const uint8_t ICON_CLOCK[10] = {
    0x7E,   // .******.
    0x81,   // *......*
    0x91,   // *..*...*
    0x91,   // *..*...*
    0x9D,   // *..***.*
    0x81,   // *......*
    0x81,   // *......*
    0x81,   // *......*
    0x7E,   // .******.
    0x00,
};
