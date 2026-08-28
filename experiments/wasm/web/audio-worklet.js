// Stereo playback for the guest's DSP output.
//
// No SharedArrayBuffer: a WKWebView on a custom scheme cannot be cross-origin
// isolated, so the emulator posts Float32 chunks over the worklet port and this
// node drains them. That costs one structured clone per frame, which is nothing
// next to the frame's draw work.
//
// Three things this has to survive that a naive queue does not:
//
//   * The context's rate is not the guest's rate. iOS gives you the hardware
//     rate (44 100 or 48 000) and the GameCube's AI runs at 32 000 or 48 000,
//     and the guest may switch between them mid-game. Asking for an
//     AudioContext at the guest rate works on some engines and silently
//     resamples badly on others, so the rate conversion lives here, in front of
//     a ring the writer and reader agree on.
//   * The producer is a frame loop, not a clock. Audio arrives in bursts of a
//     frame's worth at whatever rate the emulator manages; between bursts there
//     is nothing. Playing the instant the first sample lands means a starve a
//     few milliseconds later, and a starve is an audible click. So output does
//     not start until a prime buffer exists, and a starve re-primes rather than
//     stuttering sample by sample.
//   * The producer can outrun the clock. If it does, dropping the oldest audio
//     keeps latency bounded; drifting further behind the picture does not.
const RING_FRAMES = 1 << 15;         // 32 768 stereo frames, ~0.68 s at 48 kHz
const PRIME_SECONDS = 0.045;         // buffer this much before making a sound
const MAX_SECONDS = 0.20;            // drop the oldest beyond this

class DolAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(RING_FRAMES * 2);
    this.read = 0;                  // frame index
    this.write = 0;
    this.frac = 0;                  // fractional position between read and read+1
    this.srcRate = sampleRate;      // guest rate; corrected by the first push
    this.playing = false;
    this.starved = 0;
    this.dropped = 0;
    this.pushed = 0;
    this.port.onmessage = (e) => {
      const d = e.data;
      if (!d) return;
      if (d.rate && d.rate !== this.srcRate) {
        // A rate change mid-stream: keep what is queued (it plays out at the
        // new ratio, which is a few milliseconds of wrong pitch at worst) and
        // stop the interpolator straddling the change.
        this.srcRate = d.rate;
        this.frac = 0;
      }
      if (d.samples) this.push(d.samples);
      if (d.stats) this.report();
      if (d.flush) { this.read = this.write = 0; this.frac = 0; this.playing = false; }
    };
  }

  get queued() { return (this.write - this.read + RING_FRAMES) % RING_FRAMES; }
  get primeFrames() { return Math.min(RING_FRAMES >> 2, (this.srcRate * PRIME_SECONDS) | 0); }
  get maxFrames() { return Math.min(RING_FRAMES - 2, (this.srcRate * MAX_SECONDS) | 0); }

  push(samples) {
    const frames = samples.length >> 1;
    this.pushed += frames;
    for (let i = 0; i < frames; i++) {
      const next = (this.write + 1) % RING_FRAMES;
      if (next === this.read) {           // full: drop the oldest frame
        this.read = (this.read + 1) % RING_FRAMES;
        this.dropped++;
      }
      this.buf[this.write * 2] = samples[i * 2];
      this.buf[this.write * 2 + 1] = samples[i * 2 + 1];
      this.write = next;
    }
    // Bound latency even when the ring is nowhere near full: a producer running
    // ahead of the clock would otherwise sit at a fixed, audible delay.
    const over = this.queued - this.maxFrames;
    if (over > 0) {
      this.read = (this.read + over) % RING_FRAMES;
      this.dropped += over;
    }
  }

  report() {
    this.port.postMessage({ queued: this.queued, starved: this.starved,
                            dropped: this.dropped, pushed: this.pushed,
                            playing: this.playing, srcRate: this.srcRate,
                            ctxRate: sampleRate });
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const left = out[0], right = out.length > 1 ? out[1] : out[0];
    const n = left.length;

    if (!this.playing) {
      if (this.queued < this.primeFrames) {
        left.fill(0); if (right !== left) right.fill(0);
        return true;
      }
      this.playing = true;
      this.frac = 0;
    }

    const step = this.srcRate / sampleRate;
    for (let i = 0; i < n; i++) {
      // Linear interpolation needs the frame after `read` as well, so two are
      // the floor, not one.
      if (this.queued < 2) {
        this.starved++;
        this.playing = false;             // re-prime rather than click per sample
        for (; i < n; i++) { left[i] = 0; if (right !== left) right[i] = 0; }
        break;
      }
      const a = this.read * 2;
      const b = ((this.read + 1) % RING_FRAMES) * 2;
      const t = this.frac;
      left[i] = this.buf[a] + (this.buf[b] - this.buf[a]) * t;
      const rv = this.buf[a + 1] + (this.buf[b + 1] - this.buf[a + 1]) * t;
      if (right !== left) right[i] = rv;
      this.frac += step;
      while (this.frac >= 1) {
        this.frac -= 1;
        this.read = (this.read + 1) % RING_FRAMES;
        if (this.queued < 2) break;
      }
    }
    return true;
  }
}
registerProcessor('dol-audio', DolAudioProcessor);
