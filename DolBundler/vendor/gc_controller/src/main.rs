//! A low-latency macOS driver for the Nintendo Switch Online GameCube controller.
//!
//! Reads the controller over USB or Bluetooth LE, applies its factory
//! calibration, and feeds Dolphin through a named pipe.

mod latency;
mod output;
mod protocol;
mod transport;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use clap::{Parser, ValueEnum};
use futures::StreamExt;

use latency::{Histogram, Link};
use output::DolphinPipe;
use transport::ble_latency::{self, Latency};
use transport::usb::Poll;
use protocol::calibration::Calibration;
use protocol::state::{GcState, RawState};

#[derive(Copy, Clone, PartialEq, Eq, Debug, ValueEnum)]
enum Transport {
    /// Use the cable if the controller is plugged in, otherwise go wireless.
    Auto,
    /// Wired. Claims the vendor interface; reports arrive at the controller's
    /// own rate rather than the HID interface's 125 Hz.
    Usb,
    /// Wireless. Convenient, but the link caps out around 70 Hz.
    Ble,
}

#[derive(Parser, Debug)]
#[command(
    name = "gc_controller",
    about = "Low-latency NSO GameCube controller driver for macOS",
    long_about = None,
)]
struct Args {
    /// How to reach the controller. `auto` prefers the cable, since it is the
    /// lower-latency path, and falls back to Bluetooth when nothing is plugged
    /// in. With --reconnect the choice is remade on every retry, so unplugging
    /// moves you to wireless without restarting.
    #[arg(short, long, value_enum, default_value_t = Transport::Auto)]
    transport: Transport,

    /// FIFO name inside Dolphin's Pipes directory. Dolphin shows it as
    /// `Pipe/0/<name>` once it exists.
    #[arg(long, default_value = "gcc1")]
    pipe_name: String,

    /// Override Dolphin's Pipes directory.
    #[arg(long)]
    pipe_dir: Option<std::path::PathBuf>,

    /// Print decoded input instead of driving Dolphin. Use this to check the
    /// button mapping and stick calibration.
    #[arg(long)]
    dump: bool,

    /// Report the achieved poll rate once a second.
    #[arg(long)]
    stats: bool,

    /// Hex-dump every raw report. Implies --dump.
    #[arg(long)]
    raw: bool,

    /// Flip the main stick's vertical axis.
    #[arg(long)]
    invert_main_y: bool,

    /// Flip the C stick's vertical axis.
    #[arg(long)]
    invert_c_y: bool,

    /// Trigger travel below this fraction reads as released. Removes the ~1%
    /// the hardware rests at. Set to 0 for the raw value.
    #[arg(long, default_value_t = 0.02)]
    trigger_deadzone: f32,

    /// How long to scan for a controller before giving up, in seconds (BLE only).
    #[arg(long, default_value_t = 20)]
    scan_timeout: u64,

    /// How hard to ask macOS to schedule the BLE link. `low` is fastest and is
    /// what you want for a controller; `system` asks for nothing, which is the
    /// baseline worth comparing a histogram against.
    ///
    /// This is a request, not a setting — macOS picks the interval. Pair it with
    /// `--histogram` to see what you actually got.
    #[arg(long, value_enum, default_value_t = Latency::Low)]
    ble_latency: Latency,

    /// Measure the gap between consecutive reports and print the distribution
    /// on exit. Over BLE that gap is the connection interval.
    #[arg(long)]
    histogram: bool,

    /// Keep retrying when the controller is missing or goes away.
    #[arg(long)]
    reconnect: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let running = Arc::new(AtomicBool::new(true));
    {
        let running = running.clone();
        ctrlc_handler(move || running.store(false, Ordering::Relaxed))?;
    }

    loop {
        let result = match args.transport {
            Transport::Usb => run_usb(&args, &running),
            Transport::Ble => run_ble(&args, &running),
            Transport::Auto if transport::usb::is_present() => run_usb(&args, &running),
            Transport::Auto => {
                println!("nothing on USB — trying Bluetooth instead");
                run_ble(&args, &running)
            }
        };

        match result {
            Ok(()) => return Ok(()),
            // A signal during startup aborts whatever we were in the middle of.
            // That is a shutdown, not a failure, and must not trigger a retry.
            Err(_) if !running.load(Ordering::Relaxed) => return Ok(()),
            Err(e) if args.reconnect && running.load(Ordering::Relaxed) => {
                eprintln!("error: {e:#}");
                eprintln!("retrying in 2s…");
                std::thread::sleep(Duration::from_secs(2));
            }
            Err(e) => return Err(e),
        }
    }
}

/// Builds the output sink, or `None` in `--dump` mode.
fn make_sink(args: &Args) -> Result<Option<DolphinPipe>> {
    if args.dump {
        return Ok(None);
    }
    let dir = match &args.pipe_dir {
        Some(d) => d.clone(),
        None => output::pipe::default_dir()
            .context("cannot locate your home directory; pass --pipe-dir")?,
    };
    let pipe = DolphinPipe::create(&dir, &args.pipe_name)?;
    println!("pipe ready: {}", pipe.path().display());
    println!(
        "  in Dolphin: Controllers → GameCube Port 1 → Configure → Device = Pipe/0/{}",
        args.pipe_name
    );
    println!("  (Dolphin only scans this directory at startup — create the pipe first, then launch it.)");
    Ok(Some(pipe))
}

fn run_usb(args: &Args, running: &AtomicBool) -> Result<()> {
    let mut usb = transport::usb::UsbTransport::open()?;
    println!("connected over USB (vendor interface)");

    let mut cal = usb.initialise()?;
    cal.invert_left_y = args.invert_main_y;
    cal.invert_right_y = args.invert_c_y;
    cal.trigger_deadzone = args.trigger_deadzone;
    report_calibration(&cal);
    let _ = usb.set_player_led(0x01);

    let mut sink = make_sink(args)?;
    let mut meter = Meter::new(args.stats);
    let mut hist = Histogram::new(args.histogram, Link::Wired);
    let mut attached = false;
    let mut unknown = 0u64;

    while running.load(Ordering::Relaxed) {
        let poll = match usb.poll() {
            Ok(poll) => poll,
            // Ctrl-C interrupts the blocking read; that is a shutdown, not a
            // disconnected controller.
            Err(_) if !running.load(Ordering::Relaxed) => break,
            Err(e) => return Err(e),
        };
        let raw = match poll {
            Poll::Idle => continue,
            Poll::Unknown(n) => {
                // Worth surfacing once: a controller sending a report shape we
                // do not decode would otherwise look like a dead driver.
                unknown += 1;
                if unknown == 1 {
                    eprintln!(
                        "note: ignoring report id {:#04x} ({n} bytes)",
                        usb.last_frame().first().copied().unwrap_or(0)
                    );
                }
                continue;
            }
            Poll::Report(raw) => raw,
        };
        meter.tick();
        hist.tick();
        if args.raw {
            println!("{:02x?}", usb.last_frame());
        }
        if args.dump || args.raw {
            dump(&raw, &cal);
        } else {
            push(&mut sink, &mut attached, &cal.apply(&raw))?;
        }
    }

    finish(sink.as_ref(), &meter, &hist);
    Ok(())
}

fn run_ble(args: &Args, running: &AtomicBool) -> Result<()> {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .context("starting async runtime")?;

    // Must happen before CoreBluetooth builds a central, so the hooks are in
    // place for the very first connect.
    ble_latency::install(args.ble_latency);
    if args.ble_latency != Latency::System {
        if !ble_latency::supported() {
            eprintln!(
                "note: this macOS build does not expose the connection-latency SPI; \
                 taking the system default"
            );
        } else if !ble_latency::hooked() {
            eprintln!("note: could not hook CoreBluetooth; taking the system default");
        }
    }

    rt.block_on(async {
        println!("scanning for a controller — hold sync until the player LEDs chase…");
        let ble =
            transport::ble::BleTransport::discover(Duration::from_secs(args.scan_timeout), running).await?;
        println!("connected over BLE to {}", ble.name().await);
        report_link(args.ble_latency);

        let mut cal = ble.initialise().await?;
        cal.invert_left_y = args.invert_main_y;
        cal.invert_right_y = args.invert_c_y;
        cal.trigger_deadzone = args.trigger_deadzone;
        report_calibration(&cal);

        let mut sink = make_sink(args)?;
        let mut meter = Meter::new(args.stats);
        let mut hist = Histogram::new(args.histogram, Link::Wireless);
        let mut attached = false;
        let mut reports = Box::pin(ble.reports().await?);

        while running.load(Ordering::Relaxed) {
            // Wake periodically even when idle so Ctrl-C is honoured and a
            // silent link is noticed.
            let next = tokio::time::timeout(Duration::from_millis(500), reports.next()).await;
            let raw = match next {
                Ok(Some(raw)) => raw,
                Ok(None) => anyhow::bail!("controller disconnected"),
                Err(_) => {
                    if !ble.is_connected().await {
                        anyhow::bail!("controller disconnected");
                    }
                    continue;
                }
            };
            meter.tick();
            hist.tick();
            if args.dump || args.raw {
                dump(&raw, &cal);
            } else {
                push(&mut sink, &mut attached, &cal.apply(&raw))?;
            }
        }

        ble.disconnect().await;
        finish(sink.as_ref(), &meter, &hist);
        if let Some(params) = ble_latency::reported_params() {
            println!("last connection parameters reported by CoreBluetooth:\n  {params}");
        }
        Ok(())
    })
}

/// Says whether the faster-interval request actually reached CoreBluetooth.
///
/// Worth printing: the request is silent when it works, so without this there is
/// no way to tell a granted request from one that never left the process.
fn report_link(latency: Latency) {
    if latency == Latency::System {
        println!("link scheduling: system default (no request made)");
        return;
    }
    let (applied, failed) = ble_latency::attempts();
    if applied > 0 {
        println!("link scheduling: requested {latency:?} latency ({applied} call(s) accepted)");
        println!("  macOS chooses the interval — run with --histogram to measure what it gave you");
    } else {
        println!("link scheduling: request did not reach CoreBluetooth ({failed} failure(s))");
    }
}

/// Pushes state to Dolphin and announces the reader coming and going, so it is
/// obvious whether Dolphin has actually picked the pipe up.
fn push(sink: &mut Option<DolphinPipe>, attached: &mut bool, state: &GcState) -> Result<()> {
    let Some(pipe) = sink else {
        return Ok(());
    };
    pipe.update(state)?;
    let now = pipe.is_connected();
    if now != *attached {
        *attached = now;
        println!("Dolphin {}", if now { "attached" } else { "detached" });
    }
    Ok(())
}

fn finish(sink: Option<&DolphinPipe>, meter: &Meter, hist: &Histogram) {
    println!();
    if let Some(pipe) = sink
        && pipe.dropped() > 0
    {
        println!(
            "{} update(s) dropped (Dolphin was not draining the pipe)",
            pipe.dropped()
        );
    }
    if let Some(peak) = meter.peak {
        println!("peak report rate: {peak} Hz");
    }
    hist.report();
    println!("stopped.");
}

fn report_calibration(cal: &Calibration) {
    match &cal.serial {
        Some(s) => println!("controller serial: {s}"),
        None => println!("controller serial: unavailable"),
    }
    println!(
        "stick calibration: main centre ({}, {}), C centre ({}, {})",
        cal.left.x.neutral, cal.left.y.neutral, cal.right.x.neutral, cal.right.y.neutral
    );
    println!(
        "trigger zero points: L={:#04x} R={:#04x}",
        cal.trigger_l_zero, cal.trigger_r_zero
    );
}

/// Prints decoded state, for verifying the mapping and calibration.
fn dump(raw: &RawState, cal: &Calibration) {
    let s: GcState = cal.apply(raw);
    let mut held = String::new();
    for (name, on) in [
        ("A", s.a),
        ("B", s.b),
        ("X", s.x),
        ("Y", s.y),
        ("Z", s.z),
        ("START", s.start),
        ("L", s.l),
        ("R", s.r),
        ("Up", s.d_up),
        ("Down", s.d_down),
        ("Left", s.d_left),
        ("Right", s.d_right),
        ("HOME", s.home),
        ("CAPTURE", s.capture),
        ("C", s.c_button),
    ] {
        if on {
            if !held.is_empty() {
                held.push(' ');
            }
            held.push_str(name);
        }
    }

    print!(
        "\rbtn {:08x} main({:+.3},{:+.3}) c({:+.3},{:+.3}) L{:.2} R{:.2}  [{held}]\x1b[K",
        raw.buttons, s.main.0, s.main.1, s.c_stick.0, s.c_stick.1, s.trigger_l, s.trigger_r,
    );
    use std::io::Write as _;
    let _ = std::io::stdout().flush();
}

/// Counts reports and prints the achieved rate once a second.
struct Meter {
    enabled: bool,
    count: u32,
    window: Instant,
    peak: Option<u32>,
}

impl Meter {
    fn new(enabled: bool) -> Self {
        Self { enabled, count: 0, window: Instant::now(), peak: None }
    }

    fn tick(&mut self) {
        if !self.enabled {
            return;
        }
        self.count += 1;
        let elapsed = self.window.elapsed();
        if elapsed >= Duration::from_secs(1) {
            let hz = (f64::from(self.count) / elapsed.as_secs_f64()).round() as u32;
            self.peak = Some(self.peak.map_or(hz, |p| p.max(hz)));
            println!("{hz} Hz");
            self.count = 0;
            self.window = Instant::now();
        }
    }
}

/// Installs a SIGINT/SIGTERM handler that flips a flag so the read loop can exit.
fn ctrlc_handler(f: impl Fn() + Send + Sync + 'static) -> Result<()> {
    static HANDLER: std::sync::OnceLock<Box<dyn Fn() + Send + Sync>> = std::sync::OnceLock::new();
    HANDLER
        .set(Box::new(f))
        .map_err(|_| anyhow::anyhow!("signal handler already installed"))?;

    extern "C" fn on_signal(_: libc::c_int) {
        if let Some(f) = HANDLER.get() {
            f();
        }
    }
    let handler = on_signal as *const () as libc::sighandler_t;
    // SAFETY: the handler only stores into an `AtomicBool`, which is async-signal-safe.
    unsafe {
        libc::signal(libc::SIGINT, handler);
        libc::signal(libc::SIGTERM, handler);
    }
    Ok(())
}
