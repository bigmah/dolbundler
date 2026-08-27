// Stereo playback for the guest's DSP output.
//
// No SharedArrayBuffer: a WKWebView on a custom scheme cannot be cross-origin
// isolated, so the emulator posts Float32 chunks over the worklet port and this
// node drains a queue. That costs one structured clone per frame, which is
// nothing next to the frame's draw work.
class DolAudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];      // Float32Array chunks, interleaved stereo
    this.head = 0;        // read cursor into queue[0]
    this.queued = 0;      // frames available
    this.starved = 0;
    this.port.onmessage = (e) => {
      if (e.data && e.data.samples) {
        this.queue.push(e.data.samples);
        this.queued += e.data.samples.length >> 1;
        // Bound the backlog: if the guest outruns the clock, drop the oldest
        // audio rather than drift further and further behind the picture.
        while (this.queued > 12000) {
          const dropped = this.queue.shift();
          this.queued -= (dropped.length >> 1) - this.head;
          this.head = 0;
        }
      } else if (e.data && e.data.stats) {
        this.port.postMessage({ queued: this.queued, starved: this.starved });
      }
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0], right = out.length > 1 ? out[1] : out[0];
    for (let i = 0; i < left.length; i++) {
      if (this.queue.length === 0) {
        left[i] = 0; right[i] = 0;
        this.starved++;
        continue;
      }
      const chunk = this.queue[0];
      left[i] = chunk[this.head * 2];
      right[i] = chunk[this.head * 2 + 1];
      this.head++;
      this.queued--;
      if (this.head * 2 >= chunk.length) { this.queue.shift(); this.head = 0; }
    }
    return true;
  }
}
registerProcessor('dol-audio', DolAudioProcessor);
