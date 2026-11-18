import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {readQosLog, selectRoom, summarizeQos} from './qos-log.mjs';

const record = (event, data, pid = 1) => ({event, data, pid, node_id: 'sfu-a'});
test('room correlation separates reused transport IDs in different server processes', () => {
  const records = [record('qos_session', {transport_id: 2, room_id: 'wanted'}),
    record('qos_sample', {transport_id: 2, sent_rtp_bps: 1000}),
    record('qos_session', {transport_id: 2, room_id: 'other'}, 2),
    record('qos_sample', {transport_id: 2, sent_rtp_bps: 9999}, 2)];
  const selected = selectRoom(records, 'wanted');
  assert.equal(selected.length, 2); assert.ok(selected.every(item => item.pid === 1));
});
test('summary preserves zero throughput and unknown values and distinguishes RR from TWCC', () => {
  const summary = summarizeQos([
    record('qos_sample', {transport_id: 2, sent_rtp_bps: null, feedback_source: 'none'}),
    record('qos_sample', {transport_id: 2, sent_rtp_bps: 0, feedback_source: 'rr', rr_reports_total: 2, twcc_reports_total: 0}),
    record('qos_sample', {transport_id: 2, sent_rtp_bps: 800000, feedback_source: 'rr', rr_reports_total: 3, twcc_reports_total: 0}),
    record('qos_allocation', {transport_id: 2, budget_estimate_valid: false}),
    record('qos_track_state', {transport_id: 2, to: 'paused'})]);
  const connection = summary.transports[0];
  assert.deepEqual(connection.sent_rtp_bps, {min: 0, mean: 400000, max: 800000});
  assert.deepEqual(connection.feedback_sources, ['none', 'rr']);
  assert.equal(connection.twcc_reports_total, 0); assert.equal(connection.valid_allocation_estimate_seen, false);
  assert.equal(connection.oldest_wait_ms, null); assert.equal(connection.state_events[0].to, 'paused');
});
test('live log reading ignores other output and an unfinished final record', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'packetia-qos-'));
  try {
    const path = join(directory, 'trace.jsonl');
    await writeFile(path, 'third-party output\n' + JSON.stringify(record('qos_sample', {transport_id: 3})) + '\n{"event":"qos_sample"');
    assert.equal((await readQosLog(path)).length, 1);
  } finally {await rm(directory, {recursive: true});}
});
