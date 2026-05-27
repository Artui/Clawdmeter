#pragma once
#include <stdint.h>

// ============================================================================
// Claude Buddy ASCII art — vendored from upstream MIT projects:
//   https://github.com/1270011/claude-buddy   (server/art.ts, engine.ts)
//   https://github.com/fiorastudio/buddy
// Both are MIT-licensed; tables reproduced here with attribution (see README).
//
// Each species has 3 idle frames. Each frame is a NULL-terminated list of
// monospace lines; "{E}" marks an eye position substituted at render time
// (see buddy.cpp). Rendered with the Mono LVGL font (font_mono_32 / _18),
// which was regenerated to include the eye/star/block glyphs these tables use
// (·, ◉, ✦, °, ×, ★, █, ░, ω, ≈) — see tools/lv_font_patch.py.
// ============================================================================

#define BUDDY_FRAMES 3
#define BUDDY_MAX_LINES 6

struct BuddyFrame { const char* lines[BUDDY_MAX_LINES + 1]; };  // NULL-terminated
struct BuddySpecies { const char* name; BuddyFrame frames[BUDDY_FRAMES]; };

// Species order matches the upstream SPECIES array (engine.ts) — the daemon
// sends an index into this table.
static const BuddySpecies BUDDY_SPECIES_ART[] = {
  { "duck", {
    {{ "            ", "    __      ", "  <({E} )___  ", "   (  ._>   ", "    `--'    ", NULL }},
    {{ "            ", "    __      ", "  <({E} )___  ", "   (  ._>   ", "    `--'~   ", NULL }},
    {{ "            ", "    __      ", "  <({E} )___  ", "   (  .__>  ", "    `--'    ", NULL }},
  }},
  { "goose", {
    {{ "            ", "     ({E}>    ", "     ||     ", "   _(__)_   ", "    ^^^^    ", NULL }},
    {{ "            ", "    ({E}>     ", "     ||     ", "   _(__)_   ", "    ^^^^    ", NULL }},
    {{ "            ", "     ({E}>>   ", "     ||     ", "   _(__)_   ", "    ^^^^    ", NULL }},
  }},
  { "blob", {
    {{ "            ", "   .----.   ", "  ( {E}  {E} )  ", "  (      )  ", "   `----'   ", NULL }},
    {{ "            ", "  .------.  ", " (  {E}  {E}  ) ", " (        ) ", "  `------'  ", NULL }},
    {{ "            ", "    .--.    ", "   ({E}  {E})   ", "   (    )   ", "    `--'    ", NULL }},
  }},
  { "cat", {
    {{ "            ", "   /\\_/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ", "  (\")_(\")   ", NULL }},
    {{ "            ", "   /\\_/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ", "  (\")_(\")~  ", NULL }},
    {{ "            ", "   /\\-/\\    ", "  ( {E}   {E})  ", "  (  ω  )   ", "  (\")_(\")   ", NULL }},
  }},
  { "dragon", {
    {{ "            ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (   ~~   ) ", "  `-vvvv-'  ", NULL }},
    {{ "            ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (        ) ", "  `-vvvv-'  ", NULL }},
    {{ "   ~    ~   ", "  /^\\  /^\\  ", " <  {E}  {E}  > ", " (   ~~   ) ", "  `-vvvv-'  ", NULL }},
  }},
  { "octopus", {
    {{ "            ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ", "  /\\/\\/\\/\\  ", NULL }},
    {{ "            ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ", "  \\/\\/\\/\\/  ", NULL }},
    {{ "     o      ", "   .----.   ", "  ( {E}  {E} )  ", "  (______)  ", "  /\\/\\/\\/\\  ", NULL }},
  }},
  { "owl", {
    {{ "            ", "   /\\  /\\   ", "  (({E})({E}))  ", "  (  ><  )  ", "   `----'   ", NULL }},
    {{ "            ", "   /\\  /\\   ", "  (({E})({E}))  ", "  (  ><  )  ", "   .----.   ", NULL }},
    {{ "            ", "   /\\  /\\   ", "  (({E})(-))  ", "  (  ><  )  ", "   `----'   ", NULL }},
  }},
  { "penguin", {
    {{ "            ", "  .---.     ", "  ({E}>{E})     ", " /(   )\\    ", "  `---'     ", NULL }},
    {{ "            ", "  .---.     ", "  ({E}>{E})     ", " |(   )|    ", "  `---'     ", NULL }},
    {{ "  .---.     ", "  ({E}>{E})     ", " /(   )\\    ", "  `---'     ", "   ~ ~      ", NULL }},
  }},
  { "turtle", {
    {{ "            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[______]\\ ", "  ``    ``  ", NULL }},
    {{ "            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[______]\\ ", "   ``  ``   ", NULL }},
    {{ "            ", "   _,--._   ", "  ( {E}  {E} )  ", " /[======]\\ ", "  ``    ``  ", NULL }},
  }},
  { "snail", {
    {{ "            ", " {E}    .--.  ", "  \\  ( @ )  ", "   \\_`--'   ", "  ~~~~~~~   ", NULL }},
    {{ "            ", "  {E}   .--.  ", "  |  ( @ )  ", "   \\_`--'   ", "  ~~~~~~~   ", NULL }},
    {{ "            ", " {E}    .--.  ", "  \\  ( @  ) ", "   \\_`--'   ", "   ~~~~~~   ", NULL }},
  }},
  { "ghost", {
    {{ "            ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  ~`~``~`~  ", NULL }},
    {{ "            ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  `~`~~`~`  ", NULL }},
    {{ "    ~  ~    ", "   .----.   ", "  / {E}  {E} \\  ", "  |      |  ", "  ~~`~~`~~  ", NULL }},
  }},
  { "axolotl", {
    {{ "            ", "}~(______)~{", "}~({E} .. {E})~{", "  ( .--. )  ", "  (_/  \\_)  ", NULL }},
    {{ "            ", "~}(______){~", "~}({E} .. {E}){~", "  ( .--. )  ", "  (_/  \\_)  ", NULL }},
    {{ "            ", "}~(______)~{", "}~({E} .. {E})~{", "  (  --  )  ", "  ~_/  \\_~  ", NULL }},
  }},
  { "capybara", {
    {{ "            ", "  n______n  ", " ( {E}    {E} ) ", " (   oo   ) ", "  `------'  ", NULL }},
    {{ "            ", "  n______n  ", " ( {E}    {E} ) ", " (   Oo   ) ", "  `------'  ", NULL }},
    {{ "    ~  ~    ", "  u______n  ", " ( {E}    {E} ) ", " (   oo   ) ", "  `------'  ", NULL }},
  }},
  { "cactus", {
    {{ "            ", " n  ____  n ", " | |{E}  {E}| | ", " |_|    |_| ", "   |    |   ", NULL }},
    {{ "            ", "    ____    ", " n |{E}  {E}| n ", " |_|    |_| ", "   |    |   ", NULL }},
    {{ " n        n ", " |  ____  | ", " | |{E}  {E}| | ", " |_|    |_| ", "   |    |   ", NULL }},
  }},
  { "robot", {
    {{ "            ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ ==== ]  ", "  `------'  ", NULL }},
    {{ "            ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ -==- ]  ", "  `------'  ", NULL }},
    {{ "     *      ", "   .[||].   ", "  [ {E}  {E} ]  ", "  [ ==== ]  ", "  `------'  ", NULL }},
  }},
  { "rabbit", {
    {{ "            ", "   (\\__/)   ", "  ( {E}  {E} )  ", " =(  ..  )= ", "  (\")__(\")", NULL }},
    {{ "            ", "   (|__/)   ", "  ( {E}  {E} )  ", " =(  ..  )= ", "  (\")__(\")", NULL }},
    {{ "            ", "   (\\__/)   ", "  ( {E}  {E} )  ", " =( .  . )= ", "  (\")__(\")", NULL }},
  }},
  { "mushroom", {
    {{ "            ", " .-o-OO-o-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   ", NULL }},
    {{ "            ", " .-O-oo-O-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   ", NULL }},
    {{ "   . o  .   ", " .-o-OO-o-. ", "(__________)", "   |{E}  {E}|   ", "   |____|   ", NULL }},
  }},
  { "chonk", {
    {{ "            ", "  /\\    /\\  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------'  ", NULL }},
    {{ "            ", "  /\\    /|  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------'  ", NULL }},
    {{ "            ", "  /\\    /\\  ", " ( {E}    {E} ) ", " (   ..   ) ", "  `------'~ ", NULL }},
  }},
  { "wyvern", {
    {{ "}       {", "|\\^```^/|", "\\ {E}' '{E} /", " \\ } { /", " ≈(° °)≈", "   '-'", NULL }},
    {{ "}       {", "|\\^```^/|", "\\ {E}' '{E} /", " \\ } { /", " ≈(° °)≈", "  //|\\\\", NULL }},
    {{ "}       {", "|\\^```^/|", "\\ {E}' '{E} /", " \\ } { /", " ≈(° °)≈", "   'v'", NULL }},
  }},
  { "pikachu", {
    {{ "            ", "   /\\_/\\   ", "  ({E} {E})  ", "   (  ω )   ", "   (__)    ", NULL }},
    {{ "            ", "   /\\_/\\   ", "   (- -)   ", "   (  ω )   ", "   (__)    ", NULL }},
    {{ "            ", "   /\\_/\\   ", "  ({E} {E})  ", "   (  ~ )   ", "   (__)    ", NULL }},
  }},
};

#define BUDDY_SPECIES_COUNT (sizeof(BUDDY_SPECIES_ART) / sizeof(BUDDY_SPECIES_ART[0]))

// Hats sit on the (blank) top line of a species frame. Index = BuddyState.hat.
// Order matches upstream HATS (engine.ts). 12 cols wide to match the art.
static const char* const BUDDY_HAT_ART[] = {
  "",              // none
  "   \\^^^/    ",  // crown
  "   [___]    ",  // tophat
  "    -+-     ",  // propeller
  "   (   )    ",  // halo
  "    /^\\     ",  // wizard
  "   (___)    ",  // beanie
  "    ,>      ",  // tinyduck
};

// The wyvern's top line is "}       {" (horns); these variants tuck the hat
// between the horns instead of above. Index = hat; "" means "no wyvern variant".
static const char* const BUDDY_WYVERN_HAT[] = {
  "",          // none
  "} \\^^^/ {",  // crown
  "} [___] {",  // tophat
  "}  -+-  {",  // propeller
  "} (   ) {",  // halo
  "}  /^\\  {",  // wizard
  "} (___) {",  // beanie
  "}  ,>   {",  // tinyduck
};

// Eye glyph by mood. Upstream keeps the eye as an identity attribute and shows
// mood via a separate emoji; on-device we make the eye itself the cheap
// expressive primitive (per the device plan). Indices match BuddyState.mood:
// happy, focused, excited, tired, melancholy, chaotic. All glyphs are present
// in the regenerated Mono font.
static const char* const BUDDY_MOOD_EYE[] = {
  "◉",  // happy      — wide open
  "·",  // focused    — pinpoint
  "✦",  // excited    — sparkle
  "-",  // tired      — half-closed
  "°",  // melancholy — small/distant
  "@",  // chaotic    — swirl
};
#define BUDDY_MOOD_COUNT (sizeof(BUDDY_MOOD_EYE) / sizeof(BUDDY_MOOD_EYE[0]))

// Blink frame uses a closed eye regardless of mood.
#define BUDDY_BLINK_EYE "-"

// Stat row labels, in BuddyState.stats[] order.
static const char* const BUDDY_STAT_LABELS[] = {
  "DBG", "PAT", "CHA", "WIS", "SNK",
};
#define BUDDY_STAT_COUNT 5
