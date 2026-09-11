'use strict';
const assert = require('assert');
const { ControlQueue, TwinStore, buildControlFrame, crc16Ccitt,
  displayModel, isNewer32 } = require('./server');

function packet(sequence=1, timestamp_ms=100, po=0, pio=0) {
  return { type:'telemetry', experiment:'exp2-01_hex_counter_32', source:'fmc16',
    sequence, timestamp_ms, mode:0, clk_sel:23, clock_hz:12500000,
    pi_applied:0, pi_requested:0, pi_virtual_toggle:0, virtual_key_state:0, virtual_key_count:0,
    control_ack:0, key_state:0, key_event_count:0, po, pio };
}
assert.deepStrictEqual(displayModel(0,0).digits, '00000000'.split(''));
assert.deepStrictEqual(displayModel(305441741,1).digits, '1234ABCD'.split(''));
assert.deepStrictEqual(displayModel(0xffffffff,0x800).digits, 'FFFFFFFF'.split(''));
assert.strictEqual(displayModel(0,1).leds.filter(Boolean).length,1);
assert.strictEqual(displayModel(0,0x800).leds[11],true);
assert.strictEqual(displayModel(0,0xfff).leds.every(Boolean),true);
assert.strictEqual(isNewer32(0,0xffffffff),true);
const s=new TwinStore();
assert.strictEqual(s.accept(packet(10,5000), '192.168.100.50', 1000).accepted,true);
assert.strictEqual(s.snapshot(1000).type,'digitalTwin.snapshot');
assert.strictEqual(s.accept(packet(10), '192.168.100.50', 1100).reason,'duplicate');
assert.strictEqual(s.accept(packet(9,5100), '192.168.100.50', 1200).reason,'outOfOrder');
assert.strictEqual(s.snapshot(1600).freshness,'stale');
assert.strictEqual(s.snapshot(4101).freshness,'offline');
assert.strictEqual(s.snapshot(4101).po,0);
assert.strictEqual(s.accept(packet(1,1,5,2),'192.168.100.50',4200).accepted,true);
assert.strictEqual(s.snapshot(4200).deviceEpoch,1);
assert.strictEqual(s.accept(packet(2),'10.0.0.1',4300).reason,'sourceIp');

const frame=buildControlFrame(2,'down',0x12345678);
assert.strictEqual(frame.length,16);
assert.strictEqual(frame.subarray(0,4).toString('ascii'),'M0KC');
assert.strictEqual(frame[4],1);
assert.strictEqual(frame[5],2);
assert.strictEqual(frame[6],1);
assert.strictEqual(frame.readUInt32BE(8),0x12345678);
assert.strictEqual(frame.readUInt16BE(12),0);
assert.strictEqual(frame.readUInt16BE(14),crc16Ccitt(frame,14));
assert.throws(()=>buildControlFrame(11,'down',1),/key/);
assert.throws(()=>buildControlFrame(2,'hold',1),/action/);
assert.throws(()=>buildControlFrame(2,'down',0),/commandId/);

const sent=[];
const q=new ControlQueue(value=>sent.push(Buffer.from(value)),()=>{},100);
assert.strictEqual(q.enqueue(4,'down',1000).id,100);
assert.strictEqual(sent.length,1);
assert.strictEqual(q.view().status,'pending');
q.tick(1249);
assert.strictEqual(sent.length,1);
q.tick(1250);
assert.strictEqual(sent.length,2);
assert.strictEqual(q.acknowledge(99,1300),false);
assert.strictEqual(q.acknowledge(100,1300),true);
assert.strictEqual(q.view().status,'acknowledged');
assert.strictEqual(q.enqueue(10,'up',1400).action,'up');
console.log('Digital twin tests: ALL PASS');
