// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
// The generic page of the web player: load a cartridge, run a frame each
// time one is due, draw it on a canvas, play its sound, read the keyboard as
// joystick 1. The core's functions are the web_* of player.cpp.
(() => {
  const $ = (id) => document.getElementById(id);
  const canvas = $('screen');
  const context = canvas.getContext('2d');
  const status = (text) => { $('status').textContent = text; };

  // The joystick bits of the libretro pad: up, down, left, right, buttons.
  const keys = new Map([['ArrowUp', 4], ['ArrowDown', 5], ['ArrowLeft', 6], ['ArrowRight', 7],
    ['KeyZ', 0], ['KeyX', 8]]);
  let pad = 0;
  addEventListener('keydown', (e) => {
    if (keys.has(e.code)) { pad |= 1 << keys.get(e.code); e.preventDefault(); }
  });
  addEventListener('keyup', (e) => {
    if (keys.has(e.code)) { pad &= ~(1 << keys.get(e.code)); e.preventDefault(); }
  });

  // Sound: the core gives stereo 16-bit samples at 48 kHz for each frame;
  // they are queued and played as they come.
  let audio = null, queue = [], playing = null, offset = 0;
  function startAudio() {
    if (audio) return;
    audio = new AudioContext({ sampleRate: 48000 });
    const node = audio.createScriptProcessor(1024, 0, 2);
    node.onaudioprocess = (event) => {
      const left = event.outputBuffer.getChannelData(0), right = event.outputBuffer.getChannelData(1);
      for (let i = 0; i < left.length; i++) {
        if (!playing || offset >= playing.length) { playing = queue.shift(); offset = 0; }
        if (playing) { left[i] = playing[offset] / 32768; right[i] = playing[offset + 1] / 32768; offset += 2; }
        else left[i] = right[i] = 0;
      }
    };
    node.connect(audio.destination);
  }

  createGearSC3000().then((core) => {
    core._web_init();
    status('Choose a cartridge.');
    let running = false, image = null, last = 0, due = 0;

    function draw() {
      const width = core._web_width(), height = core._web_height();
      if (!width || !height) return;
      if (!image || image.width !== width || image.height !== height) {
        canvas.width = width; canvas.height = height;
        image = context.createImageData(width, height);
      }
      const pixels = new Uint16Array(core.HEAPU8.buffer, core._web_pixels(), width * height);
      const rgba = image.data;
      for (let i = 0, j = 0; i < pixels.length; i++, j += 4) {   // RGB565 to RGBA
        const p = pixels[i];
        rgba[j] = (p >> 11) * 255 / 31;
        rgba[j + 1] = ((p >> 5) & 63) * 255 / 63;
        rgba[j + 2] = (p & 31) * 255 / 31;
        rgba[j + 3] = 255;
      }
      context.putImageData(image, 0, 0);
    }

    function frame(now) {
      requestAnimationFrame(frame);
      if (!running) return;
      due += Math.min(now - (last || now), 100);
      last = now;
      const step = 1000 / core._web_fps();
      while (due >= step) {
        due -= step;
        core._web_pad(0, pad);
        core._web_run();
        const frames = core._web_audio_frames();
        if (audio && frames && queue.length < 8)
          queue.push(new Int16Array(core.HEAPU8.buffer, core._web_audio(), frames * 2).slice());
      }
      draw();
    }
    requestAnimationFrame(frame);

    $('rom').addEventListener('change', async () => {
      const file = $('rom').files[0];
      if (!file) return;
      startAudio();
      running = false;
      const bytes = new Uint8Array(await file.arrayBuffer());
      const ptr = core._malloc(bytes.length);
      core.HEAPU8.set(bytes, ptr);
      core._web_set_mapper(0);                 // the mapper found from the cartridge
      const loaded = core._web_load(ptr, bytes.length);
      core._free(ptr);
      if (!loaded) { status('This cartridge cannot be loaded.'); return; }
      status(file.name);
      $('reset').disabled = false;
      queue = []; due = 0; last = 0; running = true;
    });
    $('reset').addEventListener('click', () => core._web_reset());
  });
})();
