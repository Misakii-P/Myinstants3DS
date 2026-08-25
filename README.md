# Myinstants3DS

![logo](https://github.com/Misakii-P/Myinstants3DS/blob/master/gfx/myinstantsbanner.png)

A homebrew client for [Myinstants](https://www.myinstants.com) for the Nintendo 3DS. Browse, search, and play sound effects directly on your 3DS for the funsies :P

## Features

- **Browse trending sounds** on the HOME section
- **Search** any sounds from keywords
- **Play sounds** streamed via MP3 decoding with double-buffered NDSP playback
- **Bookmark system** — save your favorite sound links to SD card, easily revisit them on the BOOKMARKS section
- **Intuitive visuals** — a selection of 10 buttons on the top screen for each page, large play button and hints for each
- **Circle pad & D-pad** navigation with page wrapping
- **Touch support** — tap buttons, nav bar, and star bookmark

## Controls

| Button | Action |
|--------|--------|
| A | Play selected sound |
| B | Close info panel |
| X | Toggle bookmark |
| Y | Search |
| SELECT | Sound info |
| L / R | Previous / next page |
| D-pad | Navigate grid (wraps across pages) |
| Circle pad | Navigate grid (analog, with deadzone) |
| START | Exit |


## Building

### Requirements

- [devkitPro](https://devkitpro.org/) with devkitARM
- Portlibs: `libcurl`, `mbedtls`, `citro2d`, `citro3d`

### Build

```bash
# Set up environment (adjust paths for your install)
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitARM/bin:$PATH

# Install portlibs if you haven't
dkp-pacman -S 3ds-curl 3ds-mbedtls 3ds-citro2d 3ds-citro3d

# Build
make
```

Output: `myinstants3ds.3dsx` (for Homebrew Launcher) and `myinstants3ds.cia` (for custom firmware).

### Install

1. Copy `myinstants3ds.3dsx` to the `/3ds/` folder on your SD card
2. Launch via the Homebrew Launcher

## Project Structure

```
myinstants3ds/
├── source/
│   ├── main.c          # UI, input, dual-screen rendering
│   ├── api.c / api.h   # HTTP client (libcurl), JSON parsing, Myinstants API
│   ├── player.c / player.h  # Async MP3 download + double-buffered NDSP playback
│   └── dr_mp3.h        # Vendored single-header MP3 decoder
├── gfx/
│   ├── button.t3s      # tex3ds spritesheet spec
│   ├── button.png       # normal button
│   ├── buttonpressed.png # pressed button
│   ├── bookmarkempty.png  # empty star
│   ├── bookmarkfilled.png # filled star
│   └── ahint.png         # A button hint
├── romfs/gfx/button.t3x  # Pre-built spritesheet
├── icon.png              # App icon for SMDH
└── Makefile
```

## Technical Details

- **Networking**: Uses libcurl with portlibs (not native httpc) to bypass Cloudflare's JA3 fingerprinting. IPv4-only, HTTP/1.1, browser User-Agent string.
- **Audio**: dr_mp3 decodes MP3 frames from a 2MB in-memory buffer into 64KB ndsp wave buffers. Audio download runs on a background thread; ndsp init happens on the main thread after download completes.
- **API**: Talks to `myinstants-api.vercel.app`. Returns up to 36 results per query. No pagination support — the API returns all available results.
- **Bookmarks**: Persisted to `bookmarks.txt` on SD card (pipe-delimited). Max 36 bookmarks. Saved on view switch or app exit.

## Credits

- [Myinstants API](https://github.com/abdipr/myinstants-api)
- [dr_mp3](https://github.com/mackron/dr_libs) — single-header MP3 decoder
- [devkitPro](https://devkitpro.org/) — 3DS homebrew toolchain
