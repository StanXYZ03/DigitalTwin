'use strict';

/* Compatibility entry point.  The bidirectional implementation lives in a
 * separate file so existing launch commands (`node server.js`) remain valid. */
const controlServer = require('./server_control');
if (require.main === module) controlServer.start();
module.exports = controlServer;

if (false) {

const crypto = require('crypto');
const dgram = require('dgram');
const fs = require('fs');
const http = require('http');
const path = require('path');

const UINT32_MAX = 0xffffffff;
const DEVICE_ID = process.env.DEVICE_ID || 'board-001';
const STM32_SOURCE_IP = process.env.STM32_SOURCE_IP || '192.168.100.50';
const UDP_PORT = Number(process.env.UDP_PORT || 5005);
const HTTP_PORT = Number(process.env.HTTP_PORT || 8080);
const clients = new Set();

function isUInt(value, max) {
  return Number.isInteger(value) && value >= 0 && value <= max;
}

function validateTelemetry(value, sourceIp, expectedIp = STM32_SOURCE_IP) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('object');
  if (expectedIp && sourceIp !== expectedIp) throw new Error('sourceIp');
  if (value.type !== 'telemetry' || value.source !== 'fmc16' || value.mode !== 0) throw new Error('identity');
  if (typeof value.experiment !== 'string' || value.experiment.length < 1 || value.experiment.length > 80) throw new Error('experiment');
  if (!isUInt(value.sequence, UINT32_MAX) || !isUInt(value.timestamp_ms, UINT32_MAX)) throw new Error('sequence');
  if (!isUInt(value.po, UINT32_MAX) || !isUInt(value.pio, 0xffff)) throw new Error('io');
  for (const name of ['pi_applied', 'pi_requested', 'key_state', 'key_event_count']) {
    if (value[name] !== undefined && !isUInt(value[name], 0xffff)) throw new Error(name);
  }
  return value;
}

function isNewer32(candidate, current) {
  const delta = (candidate - current) >>> 0;
  return delta !== 0 && delta < 0x80000000;
}

function displayModel(po, pio) {
  return {
    digits: (po >>> 0).toString(16).toUpperCase().padStart(8, '0').split(''),
    leds: Array.from({ length: 12 }, (_, bit) => Boolean(pio & (1 << bit)))
  };
}

class TwinStore {
  constructor(deviceId = DEVICE_ID) {
    this.deviceId = deviceId;
    this.serverSeq = 0;
    this.epoch = 0;
    this.current = null;
    this.stats = { accepted: 0, invalid: 0, duplicate: 0, outOfOrder: 0 };
  }

  accept(packet, sourceIp, now = Date.now(), expectedIp = STM32_SOURCE_IP) {
    let value;
    try { value = validateTelemetry(packet, sourceIp, expectedIp); }
    catch (error) { this.stats.invalid += 1; return { accepted: false, reason: error.message }; }

    if (this.current) {
      const silence = now - this.current.receivedAtMs;
      const timestampRollback = this.current.timestamp_ms - value.timestamp_ms;
      const bothWentBack = value.sequence < this.current.sequence && timestampRollback > 1000;
      const restart = silence > 3000 || bothWentBack;
      if (restart) this.epoch += 1;
      else if (value.sequence === this.current.sequence) {
        this.stats.duplicate += 1; return { accepted: false, reason: 'duplicate' };
      } else if (!isNewer32(value.sequence, this.current.sequence)) {
        this.stats.outOfOrder += 1; return { accepted: false, reason: 'outOfOrder' };
      }
    }

    this.serverSeq += 1;
    this.stats.accepted += 1;
    this.current = {
      ...value,
      type: 'digitalTwin.snapshot', deviceId: this.deviceId,
      deviceEpoch: this.epoch, serverSeq: this.serverSeq,
      online: true, freshness: 'fresh', receivedAtMs: now,
      po: value.po >>> 0, display: displayModel(value.po, value.pio),
      keys: Array.from({ length: 5 }, (_, i) => Boolean((value.key_state || 0) & (1 << i)))
    };
    return { accepted: true, snapshot: this.snapshot(now) };
  }

  snapshot(now = Date.now()) {
    if (!this.current) return null;
    const ageMs = Math.max(0, now - this.current.receivedAtMs);
    const freshness = ageMs > 3000 ? 'offline' : (ageMs > 500 ? 'stale' : 'fresh');
    return { ...this.current, ageMs, online: freshness !== 'offline', freshness };
  }
}

function websocketFrame(text) {
  const body = Buffer.from(text);
  if (body.length < 126) return Buffer.concat([Buffer.from([0x81, body.length]), body]);
  const head = Buffer.alloc(4); head[0] = 0x81; head[1] = 126; head.writeUInt16BE(body.length, 2);
  return Buffer.concat([head, body]);
}

function start() {
  const store = new TwinStore();
  const html = fs.readFileSync(path.join(__dirname, 'public', 'index.html'));
  const udp = dgram.createSocket('udp4');

  function broadcast(snapshot) {
    if (!snapshot) return;
    const frame = websocketFrame(JSON.stringify(snapshot));
    for (const socket of clients) {
      if (!socket.destroyed) socket.write(frame); else clients.delete(socket);
    }
  }

  udp.on('message', (message, remote) => {
    if (message.length > 1024) { store.stats.invalid += 1; return; }
    let packet;
    try { packet = JSON.parse(message.toString('utf8')); }
    catch (_) { store.stats.invalid += 1; return; }
    const result = store.accept(packet, remote.address);
    if (result.accepted) broadcast(result.snapshot);
  });
  udp.bind(UDP_PORT, '0.0.0.0');

  const web = http.createServer((req, res) => {
    if (req.url === `/api/digital-twin/devices/${DEVICE_ID}/snapshot`) {
      const snapshot = store.snapshot();
      res.writeHead(snapshot ? 200 : 503, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify(snapshot || { type: 'digitalTwin.unavailable', deviceId: DEVICE_ID }));
      return;
    }
    if (req.url === '/' || req.url === '/index.html') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(html); return;
    }
    res.writeHead(404); res.end('Not found');
  });

  web.on('upgrade', (req, socket) => {
    if (req.url !== `/ws/digital-twin/devices/${DEVICE_ID}` || !req.headers['sec-websocket-key']) { socket.destroy(); return; }
    const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
    clients.add(socket);
    socket.on('close', () => clients.delete(socket));
    socket.on('error', () => clients.delete(socket));
    const snapshot = store.snapshot();
    if (snapshot) socket.write(websocketFrame(JSON.stringify(snapshot)));
  });
  web.listen(HTTP_PORT, '0.0.0.0');

  let lastFreshness = null;
  setInterval(() => {
    const snapshot = store.snapshot();
    if (snapshot && snapshot.freshness !== lastFreshness) {
      lastFreshness = snapshot.freshness; broadcast(snapshot);
    }
  }, 100);
  return { store, udp, web };
}

if (require.main === module) start();
module.exports = { TwinStore, displayModel, isNewer32, validateTelemetry, start };
}
