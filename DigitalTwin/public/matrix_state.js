(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  root.MatrixState = api;
}(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';

  const SIZE = 16;
  const MASK = 0xffff;
  const PATTERN_NAMES = ['walking-1', '弹跳点', '边框', '图形滚动'];
  const SPEED_NAMES = ['4 fps', '8 fps', '20 fps', '40 fps'];

  function decodeM11(po) {
    const word = Number(po) >>> 0;
    return {
      encoding: 'exp3-04-state-v1',
      valid: (word >>> 24) === 0xd4,
      experimentId: word >>> 24,
      running: Boolean((word >>> 23) & 1),
      direction: (word >>> 22) & 1,
      patternMode: (word >>> 20) & 3,
      speed: (word >>> 16) & 3,
      framePos: (word >>> 8) & 0xff,
      scanHeartbeat: (word >>> 4) & 0x0f,
      scanRow: word & 0x0f
    };
  }

  function emptyRows() {
    return new Uint16Array(SIZE);
  }

  function setPixel(rows, x, y) {
    if (x < 0 || x >= SIZE || y < 0 || y >= SIZE) return;
    rows[y] = (rows[y] | (1 << x)) & MASK;
  }

  function rotateLeft16(value, shift) {
    const s = shift & 0x0f;
    const word = value & MASK;
    return s === 0 ? word : ((word << s) | (word >>> (16 - s))) & MASK;
  }

  function bounceCoordinate(stepCount) {
    let position = 1;
    let reverse = false;
    const count = Number(stepCount) & 0xff;
    for (let i = 0; i < count; i += 1) {
      if (!reverse && position === 15) reverse = true;
      else if (reverse && position === 0) reverse = false;
      else position += reverse ? -1 : 1;
    }
    return position;
  }

  function renderPattern(state) {
    const rows = emptyRows();
    const frame = Number(state.framePos) & 0xff;
    const mode = Number(state.patternMode) & 3;

    if (mode === 0) {
      setPixel(rows, frame & 0x0f, (frame >>> 4) & 0x0f);
    } else if (mode === 1) {
      // The physical exp3-04 matrix presents this pattern on the opposite
      // diagonal, so mirror both logical axes in the web-only renderer.
      setPixel(rows, 15 - bounceCoordinate(frame),
        15 - bounceCoordinate(frame >>> 1));
    } else if (mode === 2) {
      rows[0] = MASK;
      rows[15] = MASK;
      for (let y = 1; y < 15; y += 1) rows[y] = 0x8001;
    } else {
      const glyph = [
        0x0000, 0x0000, 0x3c78, 0x4284,
        0x999a, 0xa5a5, 0xa5a5, 0x999a,
        0x8181, 0x4242, 0x3c3c, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000
      ];
      for (let y = 0; y < SIZE; y += 1) rows[y] = rotateLeft16(glyph[y], frame);
      rows[13] = state.direction ? 0x00ff : 0xff00;
    }
    return rows;
  }

  return { SIZE, MASK, PATTERN_NAMES, SPEED_NAMES, decodeM11, renderPattern };
}));
