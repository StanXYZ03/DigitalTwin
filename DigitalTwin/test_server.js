'use strict';
const assert = require('assert');
const { ControlQueue, PanelControlQueue, TwinStore, buildControlFrame,
  buildPanelControlFrame, crc16Ccitt,
  displayModel, isNewer32, validateMatrixRaw, validateModuleData } = require('./server');

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
assert.strictEqual(s.accept({...packet(2),mode:12},'192.168.100.50',4300).reason,'identity');
assert.strictEqual(s.accept({...packet(2),mode:1},'192.168.100.50',4300).reason,'identity');
const modulePacket={ver:'1.0',type:'module.data',module:'ina226_u44',seq:1,
  ts_ms:4200,data:{cur_ma:103,over_cur:0,valid:1,level:0}};
assert.strictEqual(validateModuleData(modulePacket,'192.168.100.50'),modulePacket);
assert.strictEqual(s.acceptModule(modulePacket,'192.168.100.50',4300).accepted,true);
assert.strictEqual(s.snapshot(4300).modules.ina226_u44.data.cur_ma,103);
assert.strictEqual(s.acceptModule(modulePacket,'192.168.100.50',4400).reason,'duplicate');
assert.strictEqual(s.acceptModule({...modulePacket,module:'watchdog',seq:2},
  '192.168.100.50',4500).reason,'identity');
const panelPacket={ver:'1.0',type:'module.data',module:'pcal6524',seq:2,
  ts_ms:4500,data:{sw:0x1001,yds:0x06,eth_link:1,usb_active:0,
    buzzer_mute:0,alarm_level:0,valid:1,last_result:0}};
assert.strictEqual(validateModuleData(panelPacket,'192.168.100.50'),panelPacket);
assert.strictEqual(s.acceptModule(panelPacket,'192.168.100.50',4500).accepted,true);
assert.strictEqual(s.snapshot(4500).modules.pcal6524.data.sw,0x1001);
assert.throws(()=>validateModuleData({...panelPacket,data:{...panelPacket.data,sw:0x200}},
  '192.168.100.50'),/data/);

const matrixPacket={type:'matrix.raw',experiment:'exp3-04_dot_matrix_led',
  source:'fmc16',mode:11,frame_sequence:7,capture_timestamp_ms:4210,
  valid_columns:0xffff,columns:Array.from({length:16},(_,i)=>(1<<i)&0xffff)};
assert.strictEqual(validateMatrixRaw(matrixPacket,'192.168.100.50'),matrixPacket);
assert.strictEqual(s.acceptMatrix(matrixPacket,'192.168.100.50',4510).accepted,true);
assert.strictEqual(s.snapshot(4510).matrix.columns[8],0x0100);
assert.strictEqual(s.snapshot(4510).matrix.fresh,true);
assert.strictEqual(s.acceptMatrix(matrixPacket,'192.168.100.50',4520).reason,'duplicate');
assert.strictEqual(s.acceptMatrix({...matrixPacket,frame_sequence:6},
  '192.168.100.50',4530).reason,'outOfOrder');
assert.strictEqual(s.acceptMatrix({...matrixPacket,frame_sequence:8,valid_columns:0x7fff},
  '192.168.100.50',4540).reason,'frame');
assert.strictEqual(s.acceptMatrix({...matrixPacket,frame_sequence:8,columns:[1,2]},
  '192.168.100.50',4540).reason,'columns');

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
const panelFrame=buildPanelControlFrame('switch',14,1,0x10203040);
assert.strictEqual(panelFrame.length,16);
assert.strictEqual(panelFrame.subarray(0,4).toString('ascii'),'M0PC');
assert.strictEqual(panelFrame[5],1);
assert.strictEqual(panelFrame[6],14);
assert.strictEqual(panelFrame[7],1);
assert.strictEqual(panelFrame.readUInt32BE(8),0x10203040);
assert.strictEqual(panelFrame.readUInt16BE(14),crc16Ccitt(panelFrame,14));
assert.throws(()=>buildPanelControlFrame('switch',10,1,1),/switch/);
const modeFrame=buildPanelControlFrame('mode',11,0,0x10203041);
assert.strictEqual(modeFrame[5],3);
assert.strictEqual(modeFrame[6],11);
assert.strictEqual(modeFrame[7],0);
assert.throws(()=>buildPanelControlFrame('mode',12,0,1),/mode/);
assert.throws(()=>buildPanelControlFrame('mode',1,0,1),/mode/);
assert.throws(()=>buildPanelControlFrame('mode',1,1,1),/mode/);

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
const panelSent=[];
const pq=new PanelControlQueue(value=>panelSent.push(Buffer.from(value)),()=>{},200);
assert.strictEqual(pq.enqueue('buzzer',0,1,2000).id,200);
assert.strictEqual(panelSent[0][5],2);
assert.strictEqual(pq.acknowledge(200,2100),true);
assert.strictEqual(pq.enqueue('mode',11,0,2200).target,'mode');
assert.strictEqual(panelSent[1][5],3);
console.log('Digital twin tests: ALL PASS');
