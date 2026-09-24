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

function telemetry(sequence, controlAck = 0, panelControlAck = 0) {
  return Buffer.from(JSON.stringify({
    type: 'telemetry', experiment: 'exp2-01_hex_counter_32', source: 'fmc16',
    sequence, timestamp_ms: sequence * 100, mode: 0, clk_sel: 23,
    clock_hz: 12500000, pi_applied: 0, pi_requested: 0,
    pi_virtual_toggle: 0, virtual_key_state: 0, virtual_key_count: controlAck ? 1 : 0,
    control_ack: controlAck, panel_control_ack: panelControlAck,
    key_state: 0, key_event_count: 0,
    po: sequence, pio: 1
  }));
}

function maintenance(sequence, panelControlAck = 0, fpgaDone = 1) {
  return Buffer.from(JSON.stringify({ type: 'jtag.maintenance', source: 'stm32h743',
    state: 'ready', sequence, timestamp_ms: sequence * 100,
    panel_control_ack: panelControlAck, fpga_done: fpgaDone }));
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
  const resetFramePromise = receiveControl();
  const resetQueued = await request('POST', '/api/digital-twin/devices/board-001/system/reset');
  assert.strictEqual(resetQueued.status, 202);
  const resetFrame = await resetFramePromise;
  assert.strictEqual(resetFrame.subarray(0, 4).toString('ascii'), 'M0PC');
  assert.strictEqual(resetFrame[5], 4);
  assert.strictEqual(resetFrame[6], 0);
  assert.strictEqual(resetFrame[7], 0xa5);
  assert.strictEqual(resetFrame.readUInt16BE(14), crc16Ccitt(resetFrame, 14));
  const resetCommandId = resetFrame.readUInt32BE(8);
  assert.strictEqual(resetCommandId, resetQueued.body.commandId);
  stm32.send(telemetry(3, commandId, resetCommandId), 15005, '127.0.0.1');
  await delay(120);
  const resetSnapshot = await request('GET', '/api/digital-twin/devices/board-001/snapshot');
  assert.strictEqual(resetSnapshot.body.panelControl.status, 'acknowledged');
  assert.strictEqual(resetSnapshot.body.panelControl.commandId, resetCommandId);
  assert.strictEqual(resetSnapshot.body.panelControl.target, 'reset');

  const enterFramePromise = receiveControl();
  const enterQueued = await request('POST', '/api/digital-twin/devices/board-001/system/jtag-maintenance');
  assert.strictEqual(enterQueued.status, 202);
  const enterFrame = await enterFramePromise;
  assert.strictEqual(enterFrame[5], 5);
  assert.strictEqual(enterFrame[7], 0x5a);
  const enterCommandId = enterFrame.readUInt32BE(8);
  stm32.send(maintenance(1, enterCommandId), 15005, '127.0.0.1');
  await delay(120);
  const maintenanceSnapshot = await request('GET', '/api/digital-twin/devices/board-001/snapshot');
  assert.strictEqual(maintenanceSnapshot.body.systemState, 'jtag-maintenance');
  assert.strictEqual(maintenanceSnapshot.body.maintenance.online, true);
  assert.strictEqual(maintenanceSnapshot.body.panelControl.status, 'acknowledged');

  const exitFramePromise = receiveControl();
  const exitQueued = await request('POST', '/api/digital-twin/devices/board-001/system/jtag-maintenance/exit');
  assert.strictEqual(exitQueued.status, 202);
  const exitFrame = await exitFramePromise;
  assert.strictEqual(exitFrame[5], 5);
  assert.strictEqual(exitFrame[7], 0xa6);
  const exitCommandId = exitFrame.readUInt32BE(8);
  assert.strictEqual(exitCommandId, exitQueued.body.commandId);
  stm32.send(maintenance(2, exitCommandId), 15005, '127.0.0.1');
  await delay(120);
  const exitAckSnapshot = await request('GET', '/api/digital-twin/devices/board-001/snapshot');
  assert.strictEqual(exitAckSnapshot.body.panelControl.status, 'acknowledged');
  assert.strictEqual(exitAckSnapshot.body.panelControl.commandId, exitCommandId);
  stm32.send(telemetry(4), 15005, '127.0.0.1');
  await delay(120);
  const restoredSnapshot = await request('GET', '/api/digital-twin/devices/board-001/snapshot');
  assert.strictEqual(restoredSnapshot.body.systemState, 'normal');
  console.log('Digital twin control E2E: ALL PASS');
  app.udp.close(); app.web.close(); stm32.close();
  process.exit(0);
})().catch(error => {
  console.error(error); app.udp.close(); app.web.close(); stm32.close();
  process.exit(1);
});
