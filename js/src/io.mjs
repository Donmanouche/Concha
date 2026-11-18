// Concha's .mqtt(), .robot() and .serial(): the messages of @strudel/mqtt and @strudel/serial,
// built as there, sent by the plug-in (src/io/ExternalOut) at the hap's time instead of a
// browser's Paho client / Web Serial port. Each hap becomes an outbox event of kind "mqtt" or
// "serial"; the pattern thread hands it to the sender with its time.
//
// Differences a plug-in imposes:
//  - .mqtt(user, password, ...): there is no password prompt nor cookie, give the password;
//  - .serial(baud, crc, singlecharids, name): no port picker; `name` is the device ('/dev/ttyACM0',
//    'COM3') or 'default' (the first /dev/ttyACM*, then /dev/ttyUSB*).
import { Pattern, registerControl, logger } from '@strudel/core';
import { outbox, toEvent } from './stubs.mjs';

Pattern.prototype.mqtt = function (
  username = undefined,
  password = undefined,
  topic = undefined,
  host = 'wss://localhost:8883/',
  client = undefined,
  latency = 0,
  add_meta = true,
) {
  if (username && password === undefined)
    logger('[mqtt] no password prompt in Concha: pass it, .mqtt(user, password, ...)', 'warning');
  if (!client) client = 'strudel-' + String(Math.floor(Math.random() * 1000000));
  return this.onSchedule((hap, targetTime, cps) => {
    let msgTopic = topic;
    let message = '';
    if (typeof hap.value === 'object') {
      let value = hap.value;
      if (typeof msgTopic === 'undefined' && 'topic' in value) {
        msgTopic = value.topic;
        if (Array.isArray(msgTopic)) msgTopic = msgTopic.join('/');
        msgTopic = '/' + msgTopic;
      }
      if (add_meta) {
        const duration = hap.duration.div(cps);
        value = { ...value, duration: duration.valueOf(), cps: cps };
      }
      message = JSON.stringify(value);
    } else {
      message = String(hap.value);
    }
    const e = toEvent('mqtt', hap);
    e.value = {
      host: String(host),
      client: String(client),
      user: username === undefined ? '' : String(username),
      password: password === undefined ? '' : String(password),
      topic: msgTopic === undefined ? '' : String(msgTopic),
      payload: message,
      latency: Number(latency) || 0,
    };
    outbox.events?.push(e);
  }, true);
};

export const { move } = registerControl('move');
export const { motor } = registerControl('motor');
Pattern.prototype.robot = function (robot_id, address = 'ws://192.168.8.248:9001/mqtt') {
  return this.mqtt(undefined, undefined, '/move/' + robot_id, address);
};

// crc16 (CCITT-FALSE), as serial.mjs
function crc16(data) {
  if (!data.length) return 0;
  let crc = 0xffff;
  for (let i = 0; i < data.length; ++i) {
    crc ^= data.charCodeAt(i) << 8;
    for (let j = 0; j < 8; ++j) crc = (crc & 0x8000) > 0 ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc & 0xffff;
}

function utf8hex(s) {
  const hex = (b) => b.toString(16).padStart(2, '0');
  let out = '';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c < 0x80) out += hex(c);
    else if (c < 0x800) out += hex(0xc0 | (c >> 6)) + hex(0x80 | (c & 63));
    else if (c < 0x10000) out += hex(0xe0 | (c >> 12)) + hex(0x80 | ((c >> 6) & 63)) + hex(0x80 | (c & 63));
    else out += hex(0xf0 | (c >> 18)) + hex(0x80 | ((c >> 12) & 63)) + hex(0x80 | ((c >> 6) & 63)) + hex(0x80 | (c & 63));
  }
  return out;
}

Pattern.prototype.serial = function (br = 115200, sendcrc = false, singlecharids = false, name = 'default') {
  return this.onSchedule((hap) => {
    let message = '';
    let chk = 0;
    if (typeof hap.value === 'object') {
      if ('action' in hap.value) {
        let action = hap.value['action'];
        if (singlecharids) action = action.charAt(0);
        message += action + '(';
        let first = true;
        for (let [key, val] of Object.entries(hap.value)) {
          if (key === 'action') continue;
          if (first) first = false;
          else message += ',';
          if (singlecharids) key = key.charAt(0);
          message += key + ':' + val;
        }
        message += ')';
        if (sendcrc) chk = crc16(message);
      } else {
        for (const [key, val] of Object.entries(hap.value)) message += `${key}:${val}`;
      }
    } else {
      message = String(hap.value);
    }
    // writeMessagers: the message, then "|" chk(2 bytes) ";" when there is a checksum
    let hex = utf8hex(message);
    if (chk) hex += '7c' + ((chk >> 8) & 0xff).toString(16).padStart(2, '0') + (chk & 0xff).toString(16).padStart(2, '0') + '3b';
    const e = toEvent('serial', hap);
    e.value = { port: String(name), baud: Number(br) || 115200, bytes: hex, latency: 0.1 };
    outbox.events?.push(e);
  }, true);
};
