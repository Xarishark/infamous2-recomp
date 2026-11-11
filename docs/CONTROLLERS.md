# Controllers

inFamous 2 on PC takes physical controllers and keyboard/mouse at the same time. Connect or disconnect controllers over USB or Bluetooth at any point, including mid-game.

## Supported controllers

| Controller | Connection | Pressure-sensitive buttons | Motion (SIXAXIS) | Rumble | Lights | Battery in the log |
|---|---|---|---|---|---|---|
| Xbox 360 / One / Series and other XInput pads | USB, Bluetooth, Xbox wireless adapter | triggers (analog) | – | yes | – | wireless pads |
| DualShock 4 (both models), DualShock 4 USB wireless adapter | USB, Bluetooth | L2/R2 (analog) | yes | yes | lightbar = port colour | yes |
| DualSense, DualSense Edge | USB, Bluetooth | L2/R2 (analog) | yes | yes | lightbar + player LEDs | yes |
| DualShock 3 | USB with DsHidMini or Sony's sixaxis driver; Bluetooth with BthPS3 + DsHidMini | all 12 (SDF and SXS modes) | SXS mode / sixaxis driver | SXS mode / sixaxis driver | port LED | yes |
| Switch Pro Controller and pads in "Switch mode" (8BitDo, PowerA, Hori, ...) | USB, Bluetooth | ZL/ZR read as fully pressed | yes | yes | player lights + HOME ring | yes |
| Other HID gamepads, arcade sticks, flight sticks, wheels (DirectInput class) | USB, Bluetooth | analog axes can feed L2/R2 | – | – | – | – |

## Players and ports
- inFamous 2 is single-player and reads **port 0 (player 1)** only. Ports go up to 7.
- **At startup**, ports go by type:
  1. Xbox (XInput) pads;
  2. PlayStation and Switch pads;
  3. other HID gamepads;
  4. last, joystick-type devices (wheels, flight sticks, button boxes).

  So a wheel or flight stick left plugged in does not become player 1.
- **Remapper copies (DS4Windows, DualSenseX, BetterJoy, x360ce, SCP Toolkit, Steam Input):** these add a virtual Xbox (or DUALSHOCK 4) pad that copies the physical one. The game recognises the copy from its place in Windows' device tree (the ViGEm or SCP bus, a software bus, or Steam's virtual-pad IDs) and, while the physical pad is connected, **hides it**: the physical pad is player 1, with motion, lightbar, pressure and the PS button, and the copy gets no port.
  - Hidden, not moved to port 1: it is the same controller, and on a port of its own it would be a phantom second player mirroring the first.
  - The copy is a standby. If the physical pad goes (unplugged, or hidden from the game by HidHide) while the tool still feeds its copy, the copy takes over player 1; when the physical pad comes back, it takes player 1 back.
  - As many copies are hidden as there are physical PlayStation, Switch and HID gamepads. A real Xbox pad is on none of these buses, so an Xbox pad and a DualSense both keep their ports. Steam Input can also copy an Xbox pad, so its copies count Xbox pads too.
  - Starting or stopping the tool, and unplugging or replugging the pad, are handled while the game runs.
  - `prefer_physical = 0` (env `PAD_PREFER_PHYSICAL=0`) keeps copies on ports like any other pad, the old behaviour: XInput pads come first, so the copy is then player 1.
- **After startup**, a new controller takes the lowest free port. A controller keeps its port while connected, and one that comes back gets its old port if it is still free. That also holds when a PlayStation or Switch pad moves between USB and Bluetooth.
- **One pad over two links:** a PlayStation pad connected over Bluetooth and USB at once counts once. The second link waits and takes over when the first one drops.
- **If a joystick-type device already holds port 0** when your gamepad arrives, the gamepad gets port 1, and the log prints a note saying so. Either add `map.VID:PID = ignore` for that device under [controller] (the note shows the line), or restart the game.
- **Port 0 always exists.** With no controller, the keyboard and mouse are the pad. With a controller on port 0, they work together: buttons combine, and each stick follows the device used last.
  - **Camera (right stick):** moving the controller's right stick or pressing any controller button gives the camera to the controller. A resting or jittering mouse then never moves it. The mouse takes the camera back once it has really moved (`mouse_takeover` counts at `mouse_sensitivity` 1, default 40, within about half a second), or at once on a mouse-button or camera-key press. Mouse aiming with a controller in hand still works.
  - **Movement (left stick):** whichever was pressed or moved last, the move keys or the controller's stick, wins. Held keys still move Cole while the stick is centred.
  - `last_device_wins = 0` (input.ini `[options]`) is the old merge: any keyboard or mouse stick input overrides the controller's stick for that moment.
- **If you unplug player 1's controller** while another stays on port 1, the remaining one does not move to port 0. Plug a controller back in and it takes port 0 again.
- **Lightbar colours** show the port: port 0 blue, 1 red, 2 green, 3 pink, 4 orange, 5 cyan, 6 white.
- **Player lights:**
  - DualShock 3 and Switch Pro light LED 1, 2, 3, 4, then 4+1, 4+2, 4+3.
  - The DualSense uses the PS5's own patterns: port 0 lights only the centre LED, port 1 the two inner ones, and so on, out to all five.
- **The game log** lists the controllers at startup and after every connect or disconnect (lines starting `[padhost]`), with battery levels. It ends with the line `Controller: DualSense (USB) on player 1` (or whichever pad it is), and names any hidden copy. The game window shows the same line as a notice once the game reads the pad, and again whenever player 1's controller changes.
- **A Bluetooth controller that drops out** without disconnecting (flat battery, out of range) lets go of its sticks and buttons after one second, so nothing stays held. Its input comes back as soon as it reports again.

## Button mapping

| PS3 button | In inFamous 2 | DualSense / DualShock 4 | Xbox | Switch Pro (default) | Switch Pro (`switch_layout = label`) | Other HID pads (default) |
|---|---|---|---|---|---|---|
| Cross | jump / climb; confirm | Cross | A | B | A | button 2 |
| Circle | drop / cover / dive roll; back | Circle | B | A | B | button 3 |
| Square | melee | Square | X | Y | X | button 1 |
| Triangle | interact / finisher | Triangle | Y | X | Y | button 4 |
| L1 | aim (hold) | L1 | LB | L | L | button 5 |
| R1 | lock-on; bolt while aiming | R1 | RB | R | R | button 6 |
| L2 | electric drain | L2 (analog) | LT (analog) | ZL | ZL | button 7 or the Rx axis |
| R2 | kinetic pulse | R2 (analog) | RT (analog) | ZR | ZR | button 8 or the Ry axis |
| L3 / R3 | stick clicks | stick clicks | stick clicks | stick clicks | stick clicks | buttons 11 / 12 |
| START | pause | Options | Menu | + | + | button 10 |
| SELECT | map | Create / Share, touchpad click | View | − and Capture | − and Capture | button 9 |
| D-pad, sticks | | same | same | same | same | hat 0; X/Y left, Z/Rz (or Rx/Ry) right |
| PS | system button: not passed to the game | PS | Guide | HOME | HOME | button 13 |

- A DualShock 3 maps one to one, with real pressure on every button.
- On the Switch Pro, the default follows button position (the bottom button is Cross, the way a DualShock is held). `switch_layout = label` follows the printed letters instead.
- The DualSense Edge back paddles and Fn buttons, and the DualSense mute button, do nothing unless mapped (see Settings).
- **Other HID devices:**
  - A Slider or Dial (a throttle) never moves a stick.
  - On joystick-type devices an axis only counts once it has been seen at rest. Wheel pedals or a throttle resting at one end therefore hold nothing down; a pedal pushed past halfway counts from then on.
  - Multi-axis devices such as 3D mice are used only with a `map.` line.

## Rumble, lights, motion, pressure
- **Rumble.** The game drives a DualShock 3's two motors: a small on/off motor and a large variable one.
  - Xbox: the large motor is the left (low-frequency) motor; the small one is the right motor at full speed.
  - DualShock 4: real motors.
  - DualSense: "compatible vibration"; both modes on firmware in the range where the references disagree.
  - Switch Pro: HD rumble.
  - Generic HID pads have no rumble.
  - `rumble = 0..200` scales it as a percentage: `0` turns it off, and `1` means 1 %, not "on".
- **Motion.** DualShock 3/4, DualSense and Switch Pro report real tilt and turn when the game turns its motion sensors on (inFamous 2 does). Other controllers, and the keyboard, read as a pad lying still. `motion = 0` ignores the sensors.
- **Pressure.** A DualShock 3 gives real pressure. Analog triggers give their travel. Other buttons read fully pressed.

## Connecting
- **USB:** plug in at any time.
- **Bluetooth:** pair in Windows Settings → Bluetooth & devices → Add device, then turn the controller on.
  - DualSense: hold Create + PS.
  - DualShock 4: hold Share + PS.
  - Switch Pro: hold the sync button.
  - Xbox: hold the pair button. Xbox One pads need the Bluetooth model; Xbox 360 wireless pads need Microsoft's receiver.
- **DualShock 3:** Windows has no working driver for it out of the box. Install Nefarius' DsHidMini and pick a mode:
  - **SDF:** pressure on every button.
  - **SXS:** pressure, motion, rumble and LEDs.
  - DsHidMini's XInput mode turns the pad into an Xbox controller, without pressure or motion.
  - Sony's own sixaxis driver behaves like SXS.
  - For Bluetooth, add BthPS3.
- **Driver changes while the game runs:** you can disable or update a controller's driver, or switch DsHidMini modes, while the game is running. The game lets go of the controller instead of blocking it, and picks it up again when it comes back.
- **Steam Input, DS4Windows, DualSenseX, BetterJoy, x360ce:** these add a virtual Xbox pad next to the real one, so one controller shows up twice. The game hides the copy and uses the real pad (see Players and ports). Nothing needs to be turned off. The tool may still send its own lightbar colour and rumble to the pad.
  - **DS4Windows "exclusive mode"** holds the real pad, so the log says "in use (exclusive)" and the game uses the Xbox emulation only (no motion, lightbar or pressure). It looks again every 30 s, so closing DS4Windows brings the real pad back.
  - **HidHide** hides the real pad completely, so the copy is player 1. Allow infamous2.exe in HidHide to use the real pad directly.

## Settings
Put settings in `config\input.ini` under `[controller]`. Any section whose name starts with "controller" or "gamepad" works. Every setting is listed there, commented out, with its default. Environment variables win over the file.

| Setting | Default | Meaning |
|---|---|---|
| `controllers` | 1 | 0 = ignore every physical controller (env `PAD_NO_HOST=1`). Use it for benchmarks and scripted runs: a connected pad otherwise feeds live sensor jitter into SENSOR_ON. |
| `backends` | all | xinput, ds3, ds4, dualsense, switchpro, generic, all, none; `-name` removes one (env `PAD_BACKENDS`) |
| `deadzone` / `deadzone_left` / `deadzone_right` | auto | radial stick deadzone in percent; auto = 24 for Xbox pads, 8 for the others; 0 = raw (env `PAD_DEADZONE`). The game adds its own on top; raise it if a worn stick drifts. |
| `rumble` | 100 | vibration strength in percent, 0 to 200 |
| `motion` | 1 | 0 = ignore the motion sensors |
| `trigger_threshold` | 12 | Xbox triggers: percent of travel before L2/R2 count as pressed |
| `focus_only` | 0 | 1 = ignore controllers while the game window is not in front (env `PAD_FOCUS_ONLY`) |
| `prefer_physical` | 1 | 1 = a remapper's virtual copy of a physical pad (DS4Windows, DualSenseX, BetterJoy, x360ce, SCP, Steam Input) is hidden while that pad is connected; 0 = copies are ordinary pads (env `PAD_PREFER_PHYSICAL`) |
| `log` | 0 | 1 = per-controller detail in the log (env `PAD_LOG`) |
| `readonly` | 0 | 1 = never send anything to a controller (no lights, no rumble) |
| `ds4_touchpad`, `dualsense_touchpad` | select | what a touchpad click presses: names joined by `+` |
| `dualsense_mute`, `dualsense_edge_left_paddle`, `dualsense_edge_right_paddle`, `dualsense_edge_left_fn`, `dualsense_edge_right_fn` | none | button lists, as above |
| `ds4_lightbar`, `dualsense_lightbar` | port | `port`, `off`, or `RRGGBB` (no `#`: it starts a comment) |
| `dualsense_player_leds` | port | `port` or `off` |
| `dualsense_vibration` | auto | auto, v1, v2, or both |
| `switch_layout` | position | `position` or `label` |
| `switch_home_led` | 40 | HOME ring brightness, 0 to 100 |
| `map.VID:PID` | – | button map for one HID device, or `ignore` (also keeps a wheel or flight stick off player 1) |

Names in the button lists: select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross square ps none.

`PAD_HOST="name=value|name=value"` sets any of these from the environment.

**Mapping another HID device.** When one is opened, the log prints its active mapping as a ready-made line. Copy it under [controller] and edit it, for example:
```
map.0079:0006 = Triangle=b1 Circle=b2 Cross=b3 Square=b4 RX=a3 RY=a2
```
- **Targets:** Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select PS Up Down Left Right Dpad LX LY RX RY.
- **Sources:**
  - `bN`: button N;
  - `aN`: axis N (a0 X, a1 Y, a2 Z, a3 Rx, a4 Ry, a5 Rz, a6 Slider, a7 Dial, a8 Wheel);
  - `aN~`: the same axis reversed;
  - `aN+` / `aN-`: one half of the axis;
  - `hN.up` / `.down` / `.left` / `.right`, or `Dpad=hN`: a hat.
- `L2=b7/a3` means the stronger of the two sources wins.

**Checking a controller.** `padprobe.exe` (built from libs/input/tests/padprobe.c) shows every port's state exactly as the game reads it. Add `--readonly` to make sure nothing is sent to the controller.
