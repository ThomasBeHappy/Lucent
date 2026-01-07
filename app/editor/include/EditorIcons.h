#pragma once

// Icons are UTF-8 strings for ImGui labels.
// Using plain char strings with hex escape sequences - works in both C++17 and C++20.
// These assume you merged an icon font that maps these codepoints, e.g. Font Awesome (free).
//
// Recommended file names (placed in `Assets/Fonts/` at runtime):
// - UI font: `Inter-Regular.ttf` (optional)
// - Icon font: `fa-solid-900.ttf` (optional)
//
// If the icon font isn't loaded, these will typically render as missing glyphs.

// Font Awesome (Solid) codepoints (U+F000..).
// UTF-8 encoding: U+F0xx -> EF 80..83 xx (3 bytes)
// Common UI actions:
#define LUCENT_ICON_SEARCH   "\xef\x80\x82"  // U+F002
#define LUCENT_ICON_FOLDER   "\xef\x81\xbb"  // U+F07B
#define LUCENT_ICON_FILE     "\xef\x85\x9b"  // U+F15B
#define LUCENT_ICON_IMPORT   "\xef\x82\x93"  // U+F093
#define LUCENT_ICON_SAVE     "\xef\x83\x87"  // U+F0C7
#define LUCENT_ICON_OPEN     "\xef\x81\xbc"  // U+F07C folder-open
#define LUCENT_ICON_NEW      "\xef\x85\x9b"  // U+F15B fallback: file
#define LUCENT_ICON_CUT      "\xef\x83\x84"  // U+F0C4
#define LUCENT_ICON_COPY     "\xef\x83\x85"  // U+F0C5
#define LUCENT_ICON_PASTE    "\xef\x83\xaa"  // U+F0EA
#define LUCENT_ICON_DUPLICATE "\xef\x89\x8d" // U+F24D
#define LUCENT_ICON_TRASH    "\xef\x87\xb8"  // U+F1F8
#define LUCENT_ICON_UNDO     "\xef\x83\xa2"  // U+F0E2
#define LUCENT_ICON_REDO     "\xef\x80\x9e"  // U+F01E
#define LUCENT_ICON_SETTINGS "\xef\x80\x93"  // U+F013
#define LUCENT_ICON_HELP     "\xef\x81\x99"  // U+F059
#define LUCENT_ICON_INFO     "\xef\x81\x9a"  // U+F05A
#define LUCENT_ICON_WARN     "\xef\x81\xb1"  // U+F071
#define LUCENT_ICON_ERROR    "\xef\x81\x97"  // U+F057
#define LUCENT_ICON_CHECK    "\xef\x80\x8c"  // U+F00C
#define LUCENT_ICON_CLOSE    "\xef\x80\x8d"  // U+F00D
#define LUCENT_ICON_PLUS     "\xef\x81\xa7"  // U+F067
#define LUCENT_ICON_EDIT     "\xef\x81\x84"  // U+F044

// Editor-ish:
#define LUCENT_ICON_CAMERA   "\xef\x80\xb0"  // U+F030
#define LUCENT_ICON_LIGHT    "\xef\x83\xab"  // U+F0EB
#define LUCENT_ICON_CUBE     "\xef\x86\xb2"  // U+F1B2
#define LUCENT_ICON_PLAY     "\xef\x81\x8b"  // U+F04B
#define LUCENT_ICON_PAUSE    "\xef\x81\x8c"  // U+F04C
#define LUCENT_ICON_STOP     "\xef\x81\x8d"  // U+F04D
#define LUCENT_ICON_CONSOLE  "\xef\x84\xa0"  // U+F120
