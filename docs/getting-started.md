# Getting started

From nothing to playing, on Windows, macOS or Linux. Setup installs what it
needs, downloads the libraries, recompiles the game from your ROM set and
builds it. You run one command; it takes a while the first time.

## What you need

1. **Your own `daytona93` ROM set.** This is *Daytona USA Deluxe '93*, the
   set MAME calls `daytona93`, as a `.zip` or `.7z`. No game data comes
   with this project: the game is built from your copy.

   It must be that exact set. Other Daytona USA sets (`daytona`,
   `daytonas`, `daytonat` and so on) have different program ROMs and are
   rejected, even if you rename the file. You can tell `daytona93` apart:
   it contains `epr-16530a.12`, `epr-16531a.13`, `epr-16534a.6` and
   `epr-16535a.7`. A set that has `epr-16722a.12` instead is a different
   version.

2. **A 64-bit computer** running Windows 10 or 11, macOS, or a common Linux
   distribution (Debian/Ubuntu, Fedora, Arch or openSUSE), with an internet
   connection for the first setup and a few GB of free disk space.

3. **Git**, to download the project. Setup installs everything else.

   - Windows: `winget install Git.Git` in PowerShell, or
     <https://git-scm.com>.
   - macOS: type `git` in Terminal and accept the offer to install the
     command line tools.
   - Linux: `sudo apt install git` (or your distribution's equivalent).

## 1. Download the project

Open a terminal (PowerShell on Windows, Terminal on macOS) and run:

    git clone https://github.com/alphanu1/daytona-arcade-recomp.git
    cd daytona-arcade-recomp

Stay in this folder for every command below.

## 2. Put your ROM set in place

Make a folder called `roms` inside the project and copy your set into it,
named **exactly** `daytona93.zip` (or `daytona93.7z`):

    daytona-arcade-recomp/
        roms/
            daytona93.zip

macOS and Linux:

    mkdir -p roms
    cp /path/to/your/daytona93.zip roms/daytona93.zip

Windows (PowerShell):

    mkdir roms
    copy C:\path\to\your\daytona93.zip roms\daytona93.zip

Do this before setup. Without it, setup builds only the tools and there is
no game to run. The `roms` folder is never uploaded anywhere: git ignores
it.

## 3. Run setup

**macOS and Linux:**

    ./setup.sh

The name ends in `.sh`: `./setup` on its own is "no such file".

- macOS: needs Homebrew (<https://brew.sh>); setup tells you how to install
  it if it is missing. The first time, macOS may open a window offering the
  Xcode command line tools: install them, then run `./setup.sh` again.
- Linux: setup installs packages with `sudo`, so it asks for your password.

**Windows** (PowerShell):

    powershell -ExecutionPolicy Bypass -File setup.ps1

It installs Git, CMake, Ninja, Python and the Visual Studio 2022 Build
Tools, with their Clang compiler, using winget. If you already have Visual
Studio or the Build Tools, it adds the C++ and Clang tools to them: Windows
asks for permission for that (the Visual Studio Installer needs administrator
rights). The game is built with Clang (`Compiler: Clang` in the output). If
the Clang tools cannot be added, setup stops and says what to add by hand;
`setup.ps1 --msvc` builds with Microsoft's compiler instead. The Build Tools
are a large download. If it says Python "is
not on PATH yet", close PowerShell, open a new one and run the same command
again.

Setup prints each step. It is done when you see:

    == Done

    The game is built. Start it with:

        build/daytona

If it stops before that, see [Troubleshooting](#troubleshooting).

## 4. Play

macOS and Linux:

    build/daytona

Windows:

    build\Release\daytona.exe

(Use the exact path setup printed: it is `build\daytona.exe` if setup
used Ninja.) On Windows the command prompt comes back straight away, as for
any windowed program; the game's messages still appear in that window. To
have the window wait until the game closes, use
`start /wait build\Release\daytona.exe`. Started from Explorer or a
shortcut, the game writes its messages to `daytona.log` in its settings
folder (see Starting again from scratch for where that is).

A launcher opens first. On the **Game** tab, click **Browse...**, choose
the same `roms/daytona93.zip`, and wait for the line under it to say "All
30 files verified." Then click **Start**. The launcher remembers the file
next time. **Controls** sets your keys and gamepad. In the game, **Esc**
brings the launcher back.

**Widescreen** (optional): in the launcher, under **Enhancements**, set
**Widescreen** to 16:10, 16:9 or 21:9. You see more of the scene at the
sides, nothing is stretched, and the HUD stays 4:3 in the centre; tick **HUD
at the screen edges (Experimental)** to move the lap times, position and maps out to the
sides. In a race the sky at the sides is plain blue; tick **Stretch tile
background (Experimental)** to stretch the game's own sky picture across
the whole screen instead. All apply straight away, even mid-race.

**Draw mode** (on the Game tab): **Double buffered** draws every frame, as
the arcade game does. **Single buffered** draws every second frame and
**Every third frame** every third: much less work for slower machines. The
game itself still runs at full speed; only the picture updates less often.

**Draw distance** (optional): the slider under **Enhancements** sets how far
ahead trees, rocks and buildings are drawn. **Default** is the game's own.
**Shorter** and **Shortest** draw less and run faster, which helps slower
machines; **Further** and **Furthest** draw more. The road itself is not
affected yet.

Default keys: arrows to steer, accelerate and brake; 5 inserts a coin,
Enter is start; A S D F are the view buttons; 1-4 or Q/W change gear. The
full table is in the [README](../README.md#playing).

To skip the launcher, tick **Skip launcher** on the Game tab: from then on
the game starts straight away (Esc still brings the launcher back, where you
can untick it). If the ROM set is missing or wrong, the launcher shows
anyway, with the reason. For one run only:

    build/daytona --rom roms/daytona93.zip --autostart

## Updating

To get a newer version, from the project folder:

    git pull
    ./setup.sh

(`setup.ps1` on Windows.) Always run setup after pulling: updates can
change how the game is recompiled, and setup redoes that from your ROM
set. It is much quicker the second time.

## Troubleshooting

**`zsh: no such file or directory: ./setup`**
The script is `./setup.sh`.

**`No ROM set: the tools are built, the game is not`**, or
**`build/daytona: no such file or directory`**
Setup did not find `roms/daytona93.zip` or `roms/daytona93.7z`. Check the
folder is called `roms`, is inside the project folder, and the file has
exactly that name (not `daytona.zip`, not `daytona93.zip.zip`: Windows can
hide the extension). Setup lists any archives it found in `roms/`. Fix it
and run setup again.

**`m2import: missing epr-16530a.12`** (or another file), then
**`your ROM set was rejected`**
The file is not the `daytona93` set, or is incomplete. See
[What you need](#what-you-need). Renaming a different set does not help:
its program is different.

**`your ROM set was accepted, but recompiling or building the game
failed`**
Your ROM set is fine; the build has a problem. First run `git pull` and
setup again: it may already be fixed. If not, search the output for lines
containing `error` and report them (an issue on GitHub), with the ten or so
lines around them. To retry from a clean state, delete the `build` folder
first (see Starting again from scratch).

**Windows: `unresolved external symbol WinMain`** (error LNK2019)
An older version of the project. Run `git pull`, then `setup.ps1` again.

**Windows: `error C4235: nonstandard extension used: '__int128'`**, or
**`'__builtin_clz' undefined`**
An older version of the project, built with Microsoft's compiler. Run
`git pull`, then `setup.ps1` again: it installs the Clang tools and
switches the build to Clang (both compilers work now).

**Windows: `error C2099: initializer is not a constant` in SDL's
`yuv_rgb_internal.h`**
An older version of the project, built with Microsoft's compiler. Run
`git pull`, then `setup.ps1` again.

**Windows: "The Clang tools for Visual Studio are not installed"**
Open the Visual Studio Installer, choose Modify on your Visual Studio or
Build Tools, and under Individual components tick "C++ Clang Compiler for
Windows" and "MSBuild support for LLVM (clang-cl) toolset". Then run
`setup.ps1` again. Or run `setup.ps1 --msvc` to use Microsoft's compiler.

**Start is greyed out in the launcher**
The line under the ROM path says why. If it says files are missing or
wrong, the file you chose is not the `daytona93` set. If it cannot open the
file, click **Browse...** and choose it again.

**The game closes straight away, mentioning `SDL_CreateGPUDevice`**
The graphics option in the launcher is set to one your computer lacks
(Vulkan on a Mac, for instance). Current versions fall back to automatic;
on an older one, set **Graphics API** to **Automatic**, or delete the
settings file below.

**`no recompiled code at 00xxxxxx: add it to the seeds`**
The game reached code this version does not have yet. Run `git pull` and
setup again; if it still happens, open an issue on GitHub with the address
and what you were doing in the game.

**Reporting a problem with the game itself**
Include the game's messages: the lines starting `daytona:` in the window you
started it from, or on Windows the file `daytona.log` in the settings folder
below. They say which graphics driver and renderer are in use (for example
`daytona: renderer hardware (GPU)`), and why the hardware renderer could not
start if it could not.

**Starting again from scratch**
Delete the `build` folder and run setup again. Your ROM set in `roms/` is
kept. Launcher settings, the game's settings EEPROM and backup RAM are in:

- macOS: `~/Library/Application Support/daytona-recomp/daytona93/`
- Windows: `%APPDATA%\daytona-recomp\daytona93\`
- Linux: `~/.local/share/daytona-recomp/daytona93/`

Delete that folder to reset them.

## Status

The game builds and plays on macOS (Apple silicon, Metal). Every change is
built and tested by GitHub Actions on Windows (Clang and MSVC), macOS and
Linux (GCC and Clang), without a ROM set; the Windows and Linux games
themselves have had less testing.
Problems on any platform are worth an issue on GitHub, with the full
output of setup.
