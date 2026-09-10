# Mario Party 7 nearby multiplayer

Tap Mario Party 7 in the iPhone library and choose **Nearby Multiplayer**.
One person chooses **Host Nearby Room**; the other phones select that room.
Everyone chooses **Ready**, then the host chooses **Start Game**.

Sessions assign one GameCube controller slot per phone, for two to four players.
Use the touch controls or a paired physical controller. Each phone needs its
own imported copy of the same disc revision and the same compiled app/module.
The handshake checks the game, assets, REL modules, runtime and code-generation
identity before allowing a session. A build without MP7 minigame modules is
rejected.

Keep Wi-Fi enabled and allow Local Network access when iOS asks. Network.framework
enables Apple peer-to-peer Wi-Fi as well as ordinary local Wi-Fi; an internet
connection is unnecessary. This does not implement a Bluetooth-only transport.
Router-free operation must be validated on physical phones, not a simulator or
the local transport test.

The host's memory card is synchronized before boot. Normal game saves persist
on the host; guests use Dolphin's separate netplay card paths. Turn off microphone
minigames in MP7; microphone audio is not shared. Eight-player controller sharing
is outside this version's one-player-per-phone interface.

The game continues while the nearby game menu is open. Leaving the game or
backgrounding the app ends the session for everyone. A disconnect or detected
desync stops play; create a new room to restart. There is no mid-game reconnect,
host migration or rollback.

A stop requested while the runtime is being created cancels the pending boot.
Once the runtime is published, its shutdown request remains pending through boot.
After a lobby fails or closes, queued verification, connection and polling
callbacks cannot reopen it or present a game. A transport error during play is
retained until runtime shutdown finishes, so the alert explains why play ended.

## Implementation

- `App/DBNearbyViewController.mm`: room browser, player names, ready/start state,
  full-screen game handoff, foreground lifecycle and error presentation.
- `bridge/DBNetplaySession.mm`: Dolphin session lifecycle, attached native-module
  compatibility, GameCube mapping and synchronized `BootSessionData` handoff.
- `bridge/DBNearbyTransport.mm`: Bonjour `_dbmp7._udp` discovery and
  Network.framework UDP with `includePeerToPeer`. A separate loopback UDP socket
  for each peer carries ENet datagrams unchanged. The ENet host binds only to
  loopback. ENet remains responsible for reliability, ordering and congestion
  control. Datagram and pending-send limits bound transport buffering.
  Established joining tunnels report a host that has stopped responding after
  eight seconds without incoming traffic. This deadline starts after the first
  reply, so game verification before connection does not trigger it.
- RecompCore's netplay protocol now negotiates GameCube/Wii controller mode.
  Existing desktop callers keep Wii mode by default. Incompatible old protocol
  versions cannot connect. GameCube input waits feed adaptive-buffer telemetry.
  Nearby sessions request a five-second ENet peer timeout; other callers retain
  the existing thirty-second default.

Each phone runs the full recompiled game locally. GameCube rooms start with six
buffered controller polls and adapt up to twelve. Link latency can raise the
target, and actual input waits can raise it beyond what low ping alone suggests.
A wait of at least 16 ms, or recurring shorter waits, requests two to four extra
polls, at most once per second per client. Isolated waits shorter than 16 ms do
not immediately add latency. The host broadcasts one shared target to every peer.

The buffer never automatically shrinks while the game is running, including
between minigames. This avoids repeatedly losing learned headroom during quiet
sections. In the lobby it can decrease one poll per thirty seconds; a new room
starts fresh. The twelve-poll limit bounds added button delay. Polls are not
necessarily video frames, so this is not a fixed millisecond latency guarantee.
Existing desktop Wii buffering retains its previous behavior.

Missing inputs still block simulation, so every phone must sustain the game's
speed. Buffering can absorb brief interruptions; it cannot hide a device that
consistently runs below full speed or an arbitrarily long network outage. This
is delayed input synchronization, with no video streaming or speculative replay.

## Verification

Run the portable GameCube protocol tests with loopback networking available:

```sh
cmake --build ModernGekko/build --target moderngekko_netplay_protocol_test moderngekko_netplay_buffer_test -j 4
ctest --test-dir ModernGekko/build -R 'moderngekko.netplay_(protocol|buffer)' --output-on-failure --timeout 40
```

They exercise four GameCube players, identical input delivery to remote ports,
ready gating, a full room, incompatible controller modes/builds, disconnect slot
reuse, and rejection of another player's inputs. Existing Wii tests run too.
The protocol test also checks four-client agreement on the six-poll starting
buffer, a stall-driven increase beyond the old low-ping cap, retention beyond
the old four-second expiry, and enforcement of the twelve-poll cap.

The buffer policy test uses a controlled clock to exercise recurring short
stalls, substantial waits, feedback cooldowns, stale requests, thirty minutes
of active play, gradual lobby recovery and a fresh room. It needs no game data,
network access or simulator.

Run the actual Apple transport on macOS:

```sh
bash ios/tests/run-nearby-transport-test.sh
```

It advertises a room, discovers it through Bonjour and connects three independent
peers. Ninety variable-size datagrams must return intact through three distinct
host-side ports. It needs local networking and Bonjour access.
It also establishes a connection, abruptly stops the host, and requires a loss
notification within twelve seconds while the client continues sending.

Run the UIKit lifecycle regression on a booted, dedicated iOS simulator:

```sh
bash ios/tests/run-nearby-lifecycle-test.sh <simulator-uuid>
```

This compiles the actual lobby controller into a small test app with controlled
session and transport fakes. It delays verification and boot replies until after
a connection failure, checks that the failed lobby stays closed, checks error
retention through runtime shutdown, and verifies normal successful startup. The
runner removes its separate test app afterward; no game data is required.

For repeatable device checks, launch with `DOLBUNDLER_AUTOPLAY=GP7E01`,
`DOLBUNDLER_NEARBY_TEST=host` on one phone and `join` on the other, and the same
`DOLBUNDLER_NEARBY_ROOM` value. These explicit test hooks use the ordinary lobby,
discovery and boot paths, automatically ready/start, and disable save writes.
They append snapshots, input-wait totals, FPS and speed to
`Documents/moderngekko/nearby-test.jsonl` (under the app's Dolphin user directory).
The same file records explicit `background`, `leave`, `gameFinished` and `failure`
events, so an intentional session exit can be distinguished from a network error.
Snapshots include cumulative `waitCount`, `longWaitCount` (completed waits of at
least 50 ms), and `maximumWaitMilliseconds`, in addition to total input waiting.
Compare counter differences across matching gameplay windows to assess large
stalls; median FPS alone can hide them. Counters reset when a game starts/stops.
`DOLBUNDLER_RUN_SECONDS` stops a run; `DOLBUNDLER_PERF_LOG=1` and
`DOLBUNDLER_SCREENSHOT_AFTER` enable the existing runtime diagnostics.

In that test mode, `Documents/moderngekko/nearby-test-controls.json` can drive the
local pad, for example `{"a":1,"stick_x":0.5,"capture":1}`. Button/axis keys match
`applyTestControls` in the lobby controller. Omitted controls are released; an
empty object releases everything. Increment `capture` to request another Dolphin
screenshot. With no file, touch/physical inputs work normally. This hook is
inactive during ordinary play and never edits guest memory.
For a short menu press, use `{"tap":"a","tapSequence":1}` and increment
`tapSequence` for each new press. The selected digital button releases after
100 ms, avoiding directional repeats from a value held between file polls.

Before claiming game compatibility or latency targets, test on two and four
physical iPhones: matching boot, board navigation, several complete minigames
(including Snow Ride), save synchronization, exit/background/disconnect behavior,
and sustained speed and memory use after the phones warm up. Repeat with Wi-Fi
enabled but no shared access point to establish router-free operation. Mac tests and successful
iPhone linking do not establish any of those gameplay or radio measurements.

### Development verification, September 8, 2026

- The expanded GameCube/Wii netplay protocol test passed; the other 42 tests in
  the existing ModernGekko build passed separately.
- Bonjour discovery and all 90 datagram round trips passed on macOS, including
  packets at Dolphin's 1392-byte ENet MTU and three distinct peer sockets.
- The signed iPhoneOS build passed. The simulator build passed, and its nearby
  room browser was visually inspected in portrait; a clipped footer was fixed.
  The simulator also exercised session initialization and cleanup for an invalid
  game, displaying the expected recoverable error without crashing.
- The development build was installed on the paired iPhone 13 Pro Max and
  iPhone 15 Pro Max. The 13 received the existing extracted MP7 data. Xcode
  generated a development profile covering both devices.
- The iPhone 15 subsequently launched the host lobby successfully. Its real MP7
  files passed compatibility checks, its local player occupied slot 1 and became
  ready, and its advertised room was discovered from the Mac through Bonjour.
  The lobby correctly refused to start with only one player and reported no
  session errors. This verifies device setup and discovery, not a shared game.
- An isolated simulator build then joined the actual iPhone 15. A second run
  reversed their host/client roles. Both passed the normal compatibility checks,
  synchronized boot with separate controller slots, and ran without a reported
  desync. The phone rendered the title and board-setup screens. In the second run,
  diagnostics recorded the phone entering the background immediately before both
  peers stopped cleanly. These runs did not complete a minigame.
- For that diagnostic simulator only, a temporary copy of the phone's 54,936
  generated Mach-O objects had its `LC_BUILD_VERSION` platform changed from iOS
  to iOS Simulator. Every other byte was checked unchanged. The production phone
  module and compatibility checks were unmodified. This is a local test artifact,
  not a supported cross-platform distribution; the simulator also has Metal
  presentation/pipeline errors, so its onscreen rendering is not a phone result.
- Two-phone gameplay remains unverified. iOS refused launches on the 13 with a
  signature/profile-trust error; the user then confirmed that device is currently
  unavailable, so further checks must use another peer. No two-iPhone MP7
  frame-rate, input-delay, desync-free gameplay or router-free radio result is
  claimed by the lobby and transport checks.

### Development verification, September 9, 2026

- Two isolated iOS simulators completed Snow Ride and Bubble Brawl through
  ordinary MP7 menus, with two human slots and two CPU players. The guest
  independently selected Waluigi as player two; the host selected Mario as
  player one. Both peers returned to the free-play selector with no reported
  desync. Bubble Brawl captures show the same Peach victory on both peers.
- During a 28-second active Snow Ride window, 56 half-second samples per peer
  had median 59.94 FPS and approximately 100% emulation speed. The adaptive
  buffer used two or three input polls. This is a Mac simulator result, using
  the diagnostic module described above, and does not measure iPhone speed,
  touch-to-display latency, wireless jitter or thermal behavior.
- The host's 49,216-byte MP7 GCI save and the guest's synchronized temporary
  copy had identical SHA-256 hashes before play. The host's normal card still
  had that hash after these tests, which ran with save writes disabled.
- Abruptly killing the guest app during a running session ended the host's
  game and displayed a recoverable error after 7.4 seconds. Killing the host
  in a separate running session ended the guest's game with a connection-lost
  message after 6.9 seconds.
- The host-loss transport regression failed against the previous transport,
  which could silently stop receiving UDP. It passed after adding the receive
  watchdog; the ninety-datagram transport test and GameCube/Wii protocol suite
  also passed. The current iPhoneOS and simulator builds passed.
- A subsequent UIKit regression reproduced late verification reopening a failed
  lobby, a late poll presenting a game after failure, a late connection reopening
  a failed session, and shutdown discarding a transport error. All five lifecycle
  cases passed after adding terminal-state guards and preserving the error;
  successful lobby startup is included in those cases. Both app builds passed
  again, and the signed lifecycle fix was installed on the iPhone 15.
- The signed build was verified and installed on the iPhone 15 Pro Max. The
  next launch was refused because that phone was locked. No physical minigame
  performance measurement was obtained. The iPhone 13 remains excluded from
  testing at the user's request.

Local captures and diagnostic logs for these runs are archived under
`/tmp/dolbundler-nearby-verification-2026-09-09` on the development Mac. No game
data or generated game code is included in this document.

### iPhone 15 gameplay after unlocking, September 9, 2026

The current signed build subsequently launched on the unlocked iPhone 15 Pro
Max as a guest of the isolated simulator. Normal compatibility and save checks
passed. The phone's own controller confirmed Luigi as player two; the simulator
controlled Mario as player one, with Peach and Yoshi as CPU players.

Two Snow Ride races completed and returned to the free-play menu without a
reported desync. Phone captures show active racing and the finish ceremony.
The first run included device-control/screenshot transfers and noticeable stalls.
The second run made no device transfers during gameplay: 36 half-second samples
from an 18-second active section measured a median **57.71 FPS / 96.52% speed**,
with minima of **55.18 FPS / 91.59% speed**. The buffer remained at four input
polls, and the phone accumulated 1,121 ms of input waiting between the first and
last sample. These measurements do not isolate the cost of networking from
emulation and rendering, and do not establish touch-to-display latency.

Bubble Brawl also completed; the phone capture shows the same Peach victory as
the simulator. Catchy Tunes completed as well, with phone captures of active
play and the round ending. The session ran for about 25 minutes with no reported
desync before the deliberate disconnect. The phone's runtime log reported about
436 MB after twenty minutes and 439 MB near the end. Killing the simulator host
ended the phone's runtime and displayed "Connection to the host was lost" after
6.85 seconds; runtime shutdown returned normally.

This is a development run with automated controls and save writes disabled,
not a two-phone radio or iPhone 13 benchmark. Two- and four-phone sessions,
router-free operation and physical input latency still require validation. The
iPhone 13 remains excluded from testing.

Device captures, logs, input timing and the Snow Ride statistics are under
`/tmp/dolbundler-nearby-phone15-validation` on the development Mac.

### Buffering follow-up, September 9, 2026

Reviewing those recordings after the report of large mid-minigame slowdowns
showed why the median was insufficient. During the quiet Snow Ride window,
two-second phone log samples contained peak frame intervals of 45–65 ms despite
roughly 57.7 FPS overall. The earlier race, which included development transfers,
had severe drops with substantial input waiting while reported pings remained
low. Waiting establishes that inputs were unavailable; it does not identify
whether radio delay, scheduling or a peer's emulation/rendering caused it.

The previous feedback grew by one poll at most every two seconds, was capped
at the ping recommendation plus two, and expired after four seconds. On this
low-ping session that meant repeatedly stalling with a four-poll buffer. The
six-to-twelve-poll policy above gives more initial headroom, responds faster to
completed stalls, and retains it throughout play. This trades additional button
delay for more consistent simulation. The new four-client regression failed
against the previous implementation at its initial-buffer check (exit 44).
After the change, the buffer policy and four-client GameCube/Wii protocol tests
both passed (20.92 seconds combined). The iPhoneOS and simulator builds passed,
and the iPhone app's signature verified. The buffering build was installed on
the iPhone 15 Pro Max. That phone required its passcode when checked, so no
gameplay comparison was run in this follow-up; the iPhone 13 was not used.

These policy changes still need a matching physical gameplay comparison to
measure their effect on large stalls and input responsiveness. The earlier
phone FPS measurements describe the previous buffer policy.
