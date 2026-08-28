// The reader half of AudioCommon/WebSoundStream.
//
// The emulator fills a ring of interleaved stereo s16 in the wasm module's
// memory and advances a write index; this drains it and advances a read index.
// Both live in that same memory, which is a SharedArrayBuffer, so this reads
// them with Atomics rather than messages: an AudioWorklet's render quantum is
// 128 frames and there is no room in it for a round trip.
//
// Three things this has to survive that a naive drain does not:
//
//   * The context's rate is not the guest's. iOS gives you the hardware rate --
//     44 100 or 48 000 -- and the mixer runs at 48 000, so the ratio is applied
//     here rather than asking the browser to resample.
//   * The producer is a frame loop, not a clock. Audio arrives in bursts of a
//     frame's worth and nothing in between, so output waits for a prime buffer
//     and a starve re-primes rather than stuttering sample by sample.
//   * The producer can outrun the clock. Dropping the oldest audio keeps
//     latency bounded; drifting further behind the picture does not.

const PRIME_SECONDS = 0.05;
const MAX_SECONDS = 0.25;

class DolWebAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const p = options.processorOptions;
    this.ring = new Int16Array(p.memory, p.ringPtr, p.ringFrames * 2);
    this.indices = new Int32Array(p.memory);
    this.readIndex = p.readPtr >> 2;
    this.writeIndex = p.writePtr >> 2;
    this.ringFrames = p.ringFrames;
    this.srcRate = p.sampleRate;
    this.ratio = this.srcRate / sampleRate;
    this.frac = 0;
    this.playing = false;
    this.starves = 0;
    this.port.onmessage = (e) => {
      if (e.data && e.data.stats) {
        this.port.postMessage({ queued: this.queued, starves: this.starves });
      }
    };
  }

  get queued() {
    // Unsigned difference: both indices count frames forever and wrap at 2^32,
    // which is what makes "how many are queued" a subtraction and nothing else.
    const w = Atomics.load(this.indices, this.writeIndex) >>> 0;
    const r = Atomics.load(this.indices, this.readIndex) >>> 0;
    return (w - r) >>> 0;
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0];
    const right = out.length > 1 ? out[1] : out[0];
    const frames = left.length;

    let queued = this.queued;
    const prime = Math.max(256, (this.srcRate * PRIME_SECONDS) | 0);
    if (!this.playing) {
      if (queued < prime) {
        left.fill(0);
        if (right !== left) right.fill(0);
        return true;
      }
      this.playing = true;
      this.frac = 0;
    }

    // Too far ahead: skip forward rather than let the picture and the sound
    // drift apart for the rest of the session.
    const maxFrames = Math.max(prime * 2, (this.srcRate * MAX_SECONDS) | 0);
    if (queued > maxFrames) {
      const drop = queued - maxFrames;
      Atomics.add(this.indices, this.readIndex, drop);
      queued -= drop;
    }

    let read = Atomics.load(this.indices, this.readIndex) >>> 0;
    let frac = this.frac;
    for (let i = 0; i < frames; i++) {
      if (queued < 2) {
        // Starved. Fill the rest with silence and re-prime; a click once is
        // better than a click every quantum.
        for (let j = i; j < frames; j++) {
          left[j] = 0;
          if (right !== left) right[j] = 0;
        }
        this.playing = false;
        this.starves++;
        break;
      }
      const slot = (read % this.ringFrames) * 2;
      const next = ((read + 1) % this.ringFrames) * 2;
      const l0 = this.ring[slot] / 32768;
      const r0 = this.ring[slot + 1] / 32768;
      const l1 = this.ring[next] / 32768;
      const r1 = this.ring[next + 1] / 32768;
      left[i] = l0 + (l1 - l0) * frac;
      if (right !== left) right[i] = r0 + (r1 - r0) * frac;

      frac += this.ratio;
      const advance = Math.floor(frac);
      if (advance > 0) {
        frac -= advance;
        read = (read + advance) >>> 0;
        queued -= advance;
      }
    }
    this.frac = frac;
    Atomics.store(this.indices, this.readIndex, read | 0);
    return true;
  }
}

registerProcessor('dolweb-audio', DolWebAudioProcessor);
