'use strict';

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
const CONTROL_RETRY_MS = 250;
const CONTROL_MAX_ATTEMPTS = 8;
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
  for (const name of ['pi_applied', 'pi_requested', 'pi_virtual_toggle', 'virtual_key_state', 'virtual_key_count',
    'key_state', 'key_event_count']) {
    if (value[name] !== undefined && !isUInt(value[name], 0xffff)) throw new Error(name);
  }
  if (value.control_ack !== undefined && !isUInt(value.control_ack, UINT32_MAX)) throw new Error('control_ack');
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

function crc16Ccitt(bytes, length = bytes.length) {
  let crc = 0xffff;
  for (let index = 0; index < length; index += 1) {
    crc ^= bytes[index] << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xffff) : ((crc << 1) & 0xffff);
    }
  }
  return crc;
}

function buildControlFrame(key, action, commandId) {
  if (!Number.isInteger(key) || key < 1 || key > 10) throw new Error('key');
  if (action !== 'down' && action !== 'up') throw new Error('action');
  if (!isUInt(commandId, UINT32_MAX) || commandId === 0) throw new Error('commandId');
  const frame = Buffer.alloc(16);
  frame.write('M0KC', 0, 'ascii');
  frame[4] = 1;
  frame[5] = key;
  frame[6] = action === 'down' ? 1 : 2;
  frame[7] = 0;
  frame.writeUInt32BE(commandId >>> 0, 8);
  frame.writeUInt16BE(0, 12);
  frame.writeUInt16BE(crc16Ccitt(frame, 14), 14);
  return frame;
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
      physicalKeys: Array.from({ length: 10 }, (_, i) => Boolean((value.key_state || 0) & (1 << i))),
      virtualKeys: Array.from({ length: 10 }, (_, i) => Boolean((value.virtual_key_state || 0) & (1 << i))),
      keys: Array.from({ length: 10 }, (_, i) =>
        Boolean(((value.key_state || 0) ^ (value.virtual_key_state || 0)) & (1 << i)))
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

class ControlQueue {
  constructor(sendFrame, onChange = () => {}, initialId = (Date.now() >>> 0) || 1) {
    this.sendFrame = sendFrame;
    this.onChange = onChange;
    this.nextId = initialId || 1;
    this.pending = [];
    this.inflight = null;
    this.last = { status: 'idle' };
  }

  enqueue(key, action, now = Date.now()) {
    if (!Number.isInteger(key) || key < 1 || key > 10) throw new Error('key');
    if (action !== 'down' && action !== 'up') throw new Error('action');
    if (this.pending.length >= 16) throw new Error('queueFull');
    const command = { id: this.nextId >>> 0, key, action, createdAtMs: now, attempts: 0, lastSentAtMs: 0 };
    this.nextId = (this.nextId + 1) >>> 0;
    if (this.nextId === 0) this.nextId = 1;
    this.pending.push(command);
    this.dispatch(now);
    this.onChange();
    return command;
  }

  dispatch(now = Date.now()) {
    if (this.inflight || this.pending.length === 0) return;
    this.inflight = this.pending.shift();
    this.send(now);
  }

  send(now = Date.now()) {
    if (!this.inflight) return;
    this.inflight.attempts += 1;
    this.inflight.lastSentAtMs = now;
    this.last = { status: 'pending', commandId: this.inflight.id, key: this.inflight.key,
      action: this.inflight.action,
      attempts: this.inflight.attempts };
    this.sendFrame(buildControlFrame(this.inflight.key, this.inflight.action, this.inflight.id));
  }

  acknowledge(commandId, now = Date.now()) {
    if (!this.inflight || commandId !== this.inflight.id) return false;
    this.last = { status: 'acknowledged', commandId, key: this.inflight.key,
      action: this.inflight.action,
      attempts: this.inflight.attempts, acknowledgedAtMs: now };
    this.inflight = null;
    this.dispatch(now);
    this.onChange();
    return true;
  }

  tick(now = Date.now()) {
    if (!this.inflight) { this.dispatch(now); return; }
    if (now - this.inflight.lastSentAtMs < CONTROL_RETRY_MS) return;
    if (this.inflight.attempts < CONTROL_MAX_ATTEMPTS) this.send(now);
    else {
      this.last = { status: 'failed', commandId: this.inflight.id, key: this.inflight.key,
        action: this.inflight.action,
        attempts: this.inflight.attempts, failedAtMs: now };
      this.inflight = null;
      this.dispatch(now);
      this.onChange();
    }
  }

  view() { return { ...this.last, queued: this.pending.length }; }
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
  let lastRemote = null;
  let controls;

  function currentSnapshot() {
    const snapshot = store.snapshot();
    return snapshot ? { ...snapshot, control: controls.view() } : null;
  }

  function broadcast(snapshot = currentSnapshot()) {
    if (!snapshot) return;
    const frame = websocketFrame(JSON.stringify(snapshot));
    for (const socket of clients) {
      if (!socket.destroyed) socket.write(frame); else clients.delete(socket);
    }
  }

  controls = new ControlQueue((frame) => {
    if (lastRemote) udp.send(frame, lastRemote.port, lastRemote.address);
  }, () => broadcast());

  udp.on('message', (message, remote) => {
    if (message.length > 1024) { store.stats.invalid += 1; return; }
    let packet;
    try { packet = JSON.parse(message.toString('utf8')); }
    catch (_) { store.stats.invalid += 1; return; }
    const result = store.accept(packet, remote.address);
    if (!result.accepted) return;
    lastRemote = { address: remote.address, port: remote.port };
    if (packet.control_ack) controls.acknowledge(packet.control_ack);
    broadcast();
  });
  udp.bind(UDP_PORT, '0.0.0.0');

  const web = http.createServer((req, res) => {
    if (req.method === 'GET' && req.url === `/api/digital-twin/devices/${DEVICE_ID}/snapshot`) {
      const snapshot = currentSnapshot();
      res.writeHead(snapshot ? 200 : 503, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify(snapshot || { type: 'digitalTwin.unavailable', deviceId: DEVICE_ID }));
      return;
    }

    const keyMatch = req.url.match(new RegExp(`^/api/digital-twin/devices/${DEVICE_ID}/keys/F(10|[1-9])/(down|up)$`));
    if (req.method === 'POST' && keyMatch) {
      const snapshot = store.snapshot();
      if (!lastRemote || !snapshot || !snapshot.online) {
        res.writeHead(503, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ error: 'deviceOffline' })); return;
      }
      try {
        const command = controls.enqueue(Number(keyMatch[1]), keyMatch[2]);
        res.writeHead(202, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' });
        res.end(JSON.stringify({ status: 'queued', commandId: command.id,
          key: `F${command.key}`, action: command.action }));
      } catch (error) {
        res.writeHead(error.message === 'queueFull' ? 429 : 400,
          { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ error: error.message }));
      }
      return;
    }

    if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
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
    const snapshot = currentSnapshot();
    if (snapshot) socket.write(websocketFrame(JSON.stringify(snapshot)));
  });
  web.listen(HTTP_PORT, '0.0.0.0');

  let lastFreshness = null;
  setInterval(() => {
    controls.tick();
    const snapshot = currentSnapshot();
    if (snapshot && snapshot.freshness !== lastFreshness) {
      lastFreshness = snapshot.freshness; broadcast(snapshot);
    }
  }, 100);
  return { store, controls, udp, web };
}

module.exports = { ControlQueue, TwinStore, buildControlFrame, crc16Ccitt,
  displayModel, isNewer32, validateTelemetry, start };
