'use strict';

process.env.STM32_SOURCE_IP = '127.0.0.1';
process.env.UDP_PORT = '15005';
process.env.HTTP_PORT = '18080';

const assert = require('assert');
const dgram = require('dgram');
const http = require('http');
const { crc16Ccitt, start } = require('./server');

const app = start();
const stm32 = dgram.createSocket('udp4');

function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

function telemetry(sequence, controlAck = 0) {
  return Buffer.from(JSON.stringify({
    type: 'telemetry', experiment: 'exp2-01_hex_counter_32', source: 'fmc16',
    sequence, timestamp_ms: sequence * 100, mode: 0, clk_sel: 23,
    clock_hz: 12500000, pi_applied: 0, pi_requested: 0,
    pi_virtual_toggle: 0, virtual_key_state: 0, virtual_key_count: controlAck ? 1 : 0,
    control_ack: controlAck, key_state: 0, key_event_count: 0,
    po: sequence, pio: 1
  }));
}

function request(method, path) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port: 18080, method, path }, res => {
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => resolve({ status: res.statusCode,
        body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }));
    });
    req.on('error', reject); req.end();
  });
}

function receiveControl() {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('control timeout')), 1500);
    stm32.once('message', frame => { clearTimeout(timeout); resolve(frame); });
  });
}

(async () => {
  await new Promise(resolve => stm32.bind(0, '127.0.0.1', resolve));
  await delay(100);
  stm32.send(telemetry(1), 15005, '127.0.0.1');
  await delay(100);
  const framePromise = receiveControl();
  const queued = await request('POST', '/api/digital-twin/devices/board-001/keys/F2/down');
  assert.strictEqual(queued.status, 202);
  const frame = await framePromise;
  assert.strictEqual(frame.length, 16);
  assert.strictEqual(frame.subarray(0, 4).toString('ascii'), 'M0KC');
  assert.strictEqual(frame[5], 2);
  assert.strictEqual(frame[6], 1);
  assert.strictEqual(frame.readUInt16BE(14), crc16Ccitt(frame, 14));
  const commandId = frame.readUInt32BE(8);
  assert.strictEqual(commandId, queued.body.commandId);
  stm32.send(telemetry(2, commandId), 15005, '127.0.0.1');
  await delay(120);
  const snapshot = await request('GET', '/api/digital-twin/devices/board-001/snapshot');
  assert.strictEqual(snapshot.status, 200);
  assert.strictEqual(snapshot.body.control.status, 'acknowledged');
  assert.strictEqual(snapshot.body.control.commandId, commandId);
  console.log('Digital twin control E2E: ALL PASS');
  app.udp.close(); app.web.close(); stm32.close();
  process.exit(0);
})().catch(error => {
  console.error(error); app.udp.close(); app.web.close(); stm32.close();
  process.exit(1);
});
