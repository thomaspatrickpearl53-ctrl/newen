# newen

A from-scratch, GoldSrc-compatible Half-Life engine in C (32-bit, Windows, SDL2, OpenGL) with Lua built into the
engine. **Read `HANDOFF.md` before changing anything.** It has the decisions, code map, status, known issues and
next steps. `README.md` covers commands, cvars and the Lua API.

- Build: `build.bat` (MSVC x86 + bundled CMake/Ninja). Output: `bin\newen.exe`.
- Test without a human: `bin\newen.exe +map c1a0 +wait 30 +screenshot +wait 1 +quit`, then read
  `bin\userdata\valve\console.log` and convert `bin\userdata\valve\screenshots\*.tga` with ffmpeg.
- Clean-room: never copy Xash3D (GPL) code. Never write into the Half-Life folder.
- Match the surrounding style: tabs, C11, `Mem_*` allocators, growable arrays instead of fixed limits, short
  comments that explain why.
- The owner likes to discuss before building. When they say to wait for "go", wait for it. Don't look in their
  other folders unless asked.
