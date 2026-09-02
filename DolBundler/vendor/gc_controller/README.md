# gc_controller

A low-latency macOS driver for the **Nintendo Switch Online GameCube controller**
(`057E:2073`), written in Rust. It reads the controller directly, applies the
factory calibration stored in its flash, and feeds Dolphin through a named pipe.

Measured **250 Hz** sustained over USB on an Apple Silicon Mac — twice the rate
the controller's HID interface advertises, and about 7.5× the 33 Hz the
Bluetooth link was measured delivering.

## Why this exists

The NSO GameCube controller does not present usable input as plain HID. Its
buttons, analog sticks and analog triggers only stream after a proprietary
initialisation handshake, and the raw axis values are meaningless until the
per-unit calibration is read out of the controller's SPI flash.

macOS still enumerates it as a gamepad, and Dolphin still offers it as an SDL
device — it just does not work. See
[Why not just use Dolphin's SDL backend?](#why-not-just-use-dolphins-sdl-backend)

## Quick start

### What you need

| | |
|---|---|
| macOS | Built and verified on 26.5, Apple Silicon |
| Rust | 1.85 or newer — this crate is edition 2024 |
| Xcode command line tools | `xcode-select --install` — supplies `clang`, needed for the Objective-C shim and for libusb |
| Dolphin | Any version with the Pipe input backend |

Nothing needs `sudo`, an Apple developer account, a code signature, or SIP
changes. macOS will ask once for Bluetooth permission the first time you use
`--transport ble`; the USB path prompts for nothing.

### From a fresh clone

```sh
git clone <this repo> && cd gc_controller
cargo build --release

# 1. Confirm the controller is seen and decoded. Prints live input, no Dolphin
#    involved. Plug in over USB, or hold sync to go wireless. Ctrl-C when happy.
./start.sh --dump

# 2. Run for real. This creates the FIFO Dolphin reads.
./start.sh
```

With that running, **now** start Dolphin, and configure it once:

**Controllers → GameCube Port 1 → Standard Controller → Configure**, set
**Device** to `Pipe/0/gcc1`, then bind each input by clicking its field and
pressing the button on the controller. The driver prints `Dolphin attached` when
Dolphin opens the pipe, which is the quickest confirmation the wiring is right.

That order matters only this once — Dolphin scans its `Pipes/` directory at
startup, so the FIFO has to exist by then. It persists on disk afterwards, and
your bindings persist with it.

### Every time after

```sh
./start.sh      # Ctrl-C to stop, or ./stop.sh from another terminal
```

Or use the menu bar app, if you would rather not keep a terminal open:

```sh
./ui/build.sh                                     # builds it
open "ui/build/NSO GameCube Controller.app"
```

A game controller icon appears in the menu bar — filled while the driver is
running, hollow when it is not. The menu has Start/Stop, a transport picker and
a window with the driver's live log. See [`ui/`](ui/) for how it works.

Start Dolphin whenever you like. Neither script launches or closes Dolphin. The
driver does have to be running the whole time you play, though — it is what
reads the controller and feeds the pipe, so without it Dolphin sees a controller
that is configured but silent.

`start.sh` passes its arguments through, so `./start.sh --transport ble` and
every flag below work through it.

## Usage

```
--transport <auto|usb|ble> How to reach the controller [default: auto]
--pipe-name <NAME>         FIFO name; Dolphin shows it as Pipe/0/<NAME> [default: gcc1]
--pipe-dir <PATH>          Override Dolphin's Pipes directory
--dump                     Print decoded input instead of driving Dolphin
--raw                      Hex-dump every raw report (implies --dump)
--stats                    Report the achieved poll rate once a second
--invert-main-y            Flip the main stick's vertical axis
--invert-c-y               Flip the C stick's vertical axis
--trigger-deadzone <F>     Travel below this reads as released [default: 0.02]
--scan-timeout <SECS>      How long to scan before giving up (BLE) [default: 20]
--ble-latency <CLASS>      Link scheduling to ask macOS for [default: low]
--histogram                Measure the gap between reports; print it on exit
--reconnect                Keep retrying when the controller goes away
```

## How it works

### Picking a transport

`auto` — the default — uses the cable when the controller is plugged in and
falls back to Bluetooth when it is not, so one invocation covers both. The cable
is preferred because it is the lower-latency path, not merely because it is
already connected.

With `--reconnect` the choice is remade on every retry, so unplugging the cable
moves you to wireless without restarting the driver. Pass `--transport usb` or
`--transport ble` to force one.

Note that Bluetooth still needs the sync button held, every time: LTK pairing is
not implemented, so the controller never remembers this Mac.

### USB (recommended)

The controller exposes two USB interfaces, and they are **not** interchangeable:

| Interface | Class | Used for |
|---|---|---|
| 0 | HID (3) | Input reports. macOS binds its own driver, so we read it through IOKit's HID stack. |
| 1 | Vendor (255), bulk `0x02` OUT / `0x82` IN | The SW2 command channel: flash reads, init sequence, LEDs. |

Commands go out on interface 1; input comes back on interface 0. Sending init
on the vendor interface and then reading input from that same endpoint yields
nothing at all — this split is load-bearing.

Neither interface needs an entitlement or elevated privileges. Interface 1 has
no driver attached, so `libusb` can claim it directly.

### Startup sequence

1. Claim the vendor interface and drain any packets a previous run left behind.
2. Read six SPI flash blocks: serial number, factory stick calibration, the
   GameCube trigger zero points, and the user calibration blocks (which override
   the factory values, but only when their `B2 A1` magic is present).
3. Send the ten-frame init sequence. Nothing streams until all of it lands.
4. Light the player-1 LED.
5. Read report `0x05` and translate.

### Calibration

Both matter, and both are per-unit:

- **Sticks** carry a centre and an *asymmetric* range either side of it. On the
  test unit the main stick centres at `(2033, 2022)` — not `2048` — so treating
  the range as symmetric leaves a visible resting drift.
- **Triggers** rest at a non-zero value (`L=0x23 R=0x1f` on the test unit) and
  top out at `0xE8`, not `0xFF`.

A trigger still rests a count or two above its stored zero, which Dolphin would
see as a permanently slightly-pulled trigger, so `--trigger-deadzone` (default
2%) rescales that away while keeping a full pull at exactly 1.0.

### Dolphin output

Dolphin's pipe backend takes a line-oriented text protocol:

```
PRESS A / RELEASE A
SET MAIN 0.5000 0.5000     x, y in 0..1, 0.5 centred, +Y is up
SET C    0.5000 0.5000
SET L 0.0000 / SET R 0.0000
```

Two properties keep this path fast:

- **Only deltas are written.** A steady controller produces no traffic at all.
  Values are compared at the precision they are printed, so sensor jitter below
  the 4th decimal is silent.
- **It never blocks.** The FIFO is opened non-blocking and writes stay within
  `PIPE_BUF` (512 bytes), so each write is atomic and can never tear a line. If
  Dolphin stalls, the update is dropped rather than stalling the read loop;
  since the committed state only advances on a successful write, the next report
  re-sends whatever did not land.

### Bluetooth LE

`--transport ble` implements the same protocol over the controller's two custom
GATT services: commands to `649d4ac9-…-f005` (write **without** response — the
characteristic advertises both modes but only this one works), replies on
`c765a961-…-836a`, input reports on `ab7de9be-…-7fd2`.

**Status: verified on hardware.** Connects, reads the same calibration the cable
reports, and drives Dolphin. Two known gaps:

- Nintendo's LTK pairing exchange (`cmd 0x15`) is **not** implemented, so the
  controller will not remember this Mac — expect to hold sync each time.
- Four init commands (`0x0a/0x08`, `0x01/0x0c`, `0x01/0x01`, `0x08/0x02`) get no
  reply over BLE, though they are answered over USB. Input still streams, so
  these are not fatal — but see below, because they are the most likely
  explanation for the rate.

### Bluetooth latency

**Measured: 33 Hz, a 30 ms gap between reports.** Over 1207 reports:

```
median 30.00 ms   p95 31.85 ms   p99 59.94 ms   max 90.64 ms
~30 ms   30 ms interval          97.8% ███████████████████████████████
```

That is roughly 15 ms of mean added latency against the cable's 2 ms — a quarter
of a frame at 60 fps, and the reason USB is still the recommended transport.

Wireless input latency is normally bounded by the **connection interval**: a
report can only reach the host on a connection event, so the interval *is* the
worst case. That framing is what `--ble-latency` was built to attack, and on
this controller **it does not work**:

| | median | p95 | 30 ms bin |
|---|---|---|---|
| `--ble-latency system` | 30.00 ms | 31.85 ms | 97.8% |
| `--ble-latency low` | 30.04 ms | 32.06 ms | 97.1% |

The SPI call is *accepted* — the driver reports `2 call(s) accepted`, no
exception — and changes nothing measurable. The flag is left in place because
the mechanism is sound and costs nothing, but it currently buys zero.

The likely reason is that 30 ms is **not** the connection interval at all, but
the rate the controller chooses to send at. The evidence points that way: p99 sits
at 60 ms and max at 90 ms, exact multiples of 30, which is what a missed report
looks like rather than a renegotiated link. If the controller is producing at
30 ms, no connection-interval change can help — and the four unanswered init
commands are the obvious suspect for why it might be in a reduced-rate mode.
Chasing those acks is a better lead than chasing the interval.

`--ble-latency` asks for something quicker through
`-[CBCentralManager setDesiredConnectionLatency:forPeripheral:]`, the
central-role counterpart to the public `CBPeripheralManager` method of the same
name. It is SPI, so `src/transport/ble_latency.m` reaches it by selector and
swizzles CoreBluetooth rather than forking btleplug, which does not expose its
`CBPeripheral`.

It is a **request, not a setting** — macOS picks the interval, and does not
report it back in documented units. So measure:

```sh
./target/release/gc_controller --transport ble --dump --histogram --ble-latency system   # baseline
./target/release/gc_controller --transport ble --dump --histogram --ble-latency low      # after
```

The histogram bins gaps on the intervals BLE actually uses — `bluetoothd` bins
its own telemetry the same way (`Interval_Bin_00_7point5ms`,
`..._01_11point25ms`, `..._02_15ms`), which is what confirms 7.5 ms links are
real on macOS and not merely permitted by the spec. It works over USB too, where
it measures poll spacing, which is the comparison worth having:

| Path | Interval | Mean added latency | |
|---|---|---|---|
| USB @ 250 Hz | 4 ms poll | ~2 ms | measured |
| BLE, macOS default | 30 ms | ~15 ms | measured |
| BLE, `--ble-latency low` | 30 ms | ~15 ms | measured — no change |
| BLE, protocol floor | 7.5 ms | ~3.75 ms | not reached |

The USB row is measured, not nominal — `--histogram` over 2916 reports on an
Apple Silicon Mac:

```
median 4.00 ms   p95 4.25 ms   p99 4.59 ms   max 6.43 ms
~4 ms   250 Hz — the vendor bulk path        99.6% ████████████████████████
```

Worth stating plainly because 125 Hz is widely taken to be the macOS ceiling —
macOS gives no user-accessible way to override USB HID polling rates. That
ceiling applies to the *HID* interface. Reading input while driving the vendor
interface gets twice the rate.

**7.5 ms is a hard BLE floor**, so ~3.75 ms mean is the best any wireless path
can do. USB remains lower latency and is still the recommended transport.

## Why not a system-wide virtual gamepad?

Publishing a virtual HID device on macOS means `IOHIDUserDeviceCreateWithProperties`,
which requires the `com.apple.developer.hid.virtual.device` entitlement. That
entitlement is restricted — Apple grants it on request to Developer Program
members. Verified on macOS 26.5:

| Signing | Result |
|---|---|
| Unsigned | `IOHIDUserDeviceCreateWithProperties` returns `NULL` |
| Ad-hoc signed, entitlement claimed | Process killed by AMFI (`SIGKILL`) |
| Apple-granted entitlement | Works |

The Dolphin pipe needs no entitlement, no code signing and no privacy prompt,
and it is a shorter path from wire to emulator.

## Why not just use Dolphin's SDL backend?

Because it looks like it works and then does not. Verified on macOS 26.5 with
Dolphin 2506a, controller freshly replugged so nothing had initialised it:

| | |
|---|---|
| macOS enumerates it | yes — `Nintendo GameCube Controller`, usage page 1, usage 5 (Gamepad) |
| Dolphin lists it | yes — appears as an `SDL/0/…` device |
| Buttons respond | **no** |

SDL does have a Switch 2 driver, but it is USB-only and needs SDL built with
libusb, because the initialisation handshake runs on the vendor interface and the
HID interface alone will not do. Dolphin's bundled SDL never enables it. So the
device enumerates as a gamepad, Dolphin offers it, and nothing streams — which is
a worse failure than not appearing at all, because it looks like a binding
problem.

That is the whole reason this exists.

## Why not set the connection interval directly?

Because the interval is set over HCI, and HCI is closed. The obvious approach —
issue `LE Connection Update` yourself and name the interval instead of asking for
a latency class — does not work in userspace. Verified on macOS 26.5, Apple
Silicon:

| Approach | Result |
|---|---|
| `IOServiceOpen` on `IOBluetoothHCIController` | `kIOReturnUnsupported` (`0xe00002c7`) |
| `-[IOBluetoothHostController BluetoothHCILEConnectionUpdate:…]` | present, but returns 0 and fills nothing |
| Other `IOBluetoothHostController` HCI reads | same — silently swallowed, out-params untouched |
| Intercepting the radio's own HCI transport | radio is **PCIe** (`AppleSunriseBluetooth.dext`), not USB — no transport to sit on |

The gate is `com.apple.bluetooth.iokit-user-access`, which `bluetoothd` carries
and third-party binaries cannot get. It is an entitlement check, not a `uid`
check, so `sudo` does not help. Watch for the false positive here:
`BluetoothHCIReadDeviceAddress:` *does* return the correct adapter address, but
from a framework cache rather than a live round-trip — it is not evidence that
HCI passthrough works.

A USB Bluetooth dongle would sidestep all of this, and macOS 26 ships **no** USB
Bluetooth transport at all (no `bDeviceClass 224` personality in any kext or
dext), so `libusb` could claim one and drive HCI directly. That is the only path
to a guaranteed 7.5 ms interval — at the cost of external hardware.

## Layout

```
src/
  protocol/      Transport-independent: command frames, report 0x05, calibration
  transport/     usb.rs (rusb + hidapi), ble.rs (btleplug),
                 ble_latency.{m,rs} — CoreBluetooth interval request
  output/        pipe.rs — Dolphin FIFO
  latency.rs     Inter-arrival histogram; identifies the negotiated interval
```

`cargo test` covers the wire-format decoding, the calibration maths and the
pipe protocol — everything that can be checked without hardware.

## Protocol notes

The SW2 command framing, the init sequence, the SPI flash addresses and the
report 0x05 layout are all undocumented by Nintendo. They were established from
prior open-source reverse-engineering of the Switch 2 controller family, and
confirmed against this unit — the calibration this driver reads back matches over
both transports, and the init sequence is the minimum that makes it stream.
