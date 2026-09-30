# ps3pad

Use any PC gamepad (USB or Bluetooth: DualShock 4, DualSense, Xbox, Switch Pro, 8BitDo…) as a PS3 controller, through **webMAN MOD's virtual pad**.

```
[gamepad] --USB/BT--> [PC / Raspberry Pi] --HTTP--> [PS3 running webMAN MOD]
```

Written in C with SDL2. GitHub Actions builds it automatically for Windows x64, Linux x86_64, Linux arm64 (Raspberry Pi) and macOS.

## Usage

```sh
ps3pad --list                      # list detected gamepads
ps3pad 192.168.1.50                # go
ps3pad 192.168.1.50 --stick analog -v
ps3pad --dry-run                   # test the mapping without a PS3
```

| Option | What it does |
|---|---|
| `--port N` | webMAN web server port (default 80) |
| `--pad N` | Which gamepad to use (index from `--list`) |
| `--stick dpad` | *(default)* left stick = D-pad, right stick = right analog |
| `--stick analog` | Both sticks as analog sticks |
| `--stick off` | Ignore the sticks |
| `--deadzone N` | Stick deadzone (0-32767, default 16000) |
| `--trigger N` | L2/R2 threshold (0-32767, default 12000) |
| `--keep` | Don't send `off` on exit |
| `-v` | Print every request and its latency |

## Mapping

Positional, like on a PS3:

| Gamepad | PS3 |
|---|---|
| Bottom face button (A / ✕) | ✕ |
| Right face button (B / ○) | ○ |
| Left face button (X / □) | □ |
| Top face button (Y / △) | △ |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 |
| Stick clicks | L3 / R3 |
| Back / Share / View | SELECT |
| Start / Options / Menu | START |
| Guide / PS, Capture or touchpad | PS |

The Guide button is often hijacked by the OS (Game Bar on Windows, Steam…). That's why the **touchpad** (DS4/DualSense), the **Share** button on Xbox Series pads, the DualSense **mic** button and the Switch **Capture** button also send PS.

## Requirements

- A PS3 running **webMAN MOD** (built with `VIRTUAL_PAD`, regular builds include it) with the virtual pad not disabled in setup.
- PS3 and PC on the same network. If you set an admin password in webMAN, ps3pad doesn't send it (yet).
- **Linux**: `sudo apt install libsdl2-2.0-0`. To read the gamepad without root you may need udev rules for your controller (installing Steam usually takes care of that).
- **macOS**: `brew install sdl2`.
- **Windows**: nothing, the `.exe` has SDL2 built in.

## Limitations (webMAN's fault, not mine)

ps3pad uses `/pad.ps3?hold_...` over HTTP, so:

- **Latency**: every state change is one HTTP request. XMB, menus, Movian, turn-based games: great. A shooter: you will suffer.
- **All-or-nothing sticks**: webMAN only accepts full deflection in 8 directions. No fine analog control.
- **One stick per request**, and if the D-pad is pressed, the D-pad wins.
- **L2/R2 are digital**.
- The virtual pad takes an extra port on the PS3: if another controller is connected, the virtual one may end up as player 2.

## Building manually

```sh
# Linux / macOS / MSYS2
make
# Windows (MSYS2 UCRT64), single exe:
make STATIC=1
```
