import {readFile} from 'node:fs/promises';

export async function readQosLog(path) {
  const lines = (await readFile(path, 'utf8')).split('\n');
  // Ignore a partial final line while the asynchronous logger is writing.
  lines.pop();
  const records = [];
  for (const line of lines) {
    let record;
    try {record = JSON.parse(line);} catch {continue;}
    if (typeof record.event === 'string' && record.event.startsWith('qos_') && record.data)
      records.push(record);
  }
  return records;
}

const key = record => `${record.node_id || ''}:${record.pid}:${record.data.transport_id}`;
export function selectRoom(records, room) {
  const connections = new Set(records.filter(record => record.event === 'qos_session' && record.data.room_id === room).map(key));
  return records.filter(record => connections.has(key(record)));
}
function range(values) {
  const valid = values.filter(Number.isFinite);
  return valid.length ? {min: Math.min(...valid), mean: valid.reduce((sum, value) => sum + value, 0) / valid.length,
    max: Math.max(...valid)} : null;
}
export function summarizeQos(records) {
  const groups = new Map();
  for (const record of records) {
    const id = key(record);
    if (!groups.has(id)) groups.set(id, {node_id: record.node_id, pid: record.pid, transport_id: record.data.transport_id,
      session: null, samples: [], allocations: [], state_events: [], probe_events: []});
    const group = groups.get(id);
    if (record.event === 'qos_session') group.session = record.data;
    if (record.event === 'qos_sample') group.samples.push(record.data);
    if (record.event === 'qos_allocation') group.allocations.push(record.data);
    if (['qos_track_state', 'qos_control_state'].includes(record.event)) group.state_events.push({event: record.event, ...record.data});
    if (record.event.startsWith('qos_probe_')) group.probe_events.push({event: record.event, ...record.data});
  }
  return {schema_version: 1, source: 'server-qos-log', generated_at: new Date().toISOString(),
    notes: ['RTP send rates include RTP headers, audio, retransmission and padding; they are not browser video-payload rates.',
      'Queue values are periodic snapshots, not per-packet latency distributions.',
      'Monotonic timestamps are only comparable within the same server process.',
      'TWCC input and valid estimates do not establish recovery-probe success or weak-network performance.',
      ...([...groups.values()].some(group => group.samples.length && !group.samples.some(sample => sample.feedback_source === 'twcc'))
        ? ['Some sampled transports have no TWCC control input; inspect their feedback sources before evaluating trend detection.'] : [])],
    transports: [...groups.values()].map(group => {
      const last = group.samples.at(-1);
      return {node_id: group.node_id, pid: group.pid, transport_id: group.transport_id, session: group.session,
        sample_count: group.samples.length, feedback_sources: [...new Set(group.samples.map(sample => sample.feedback_source))],
        sent_rtp_bps: range(group.samples.map(sample => sample.sent_rtp_bps)),
        oldest_wait_ms: range(group.samples.map(sample => sample.oldest_wait_ms)),
        sent_packets_total: last?.sent_packets_total ?? null,
        twcc_reports_total: last?.twcc_reports_total ?? null, rr_reports_total: last?.rr_reports_total ?? null,
        matched_feedback_packets_total: last?.matched_feedback_packets_total ?? null,
        unmatched_twcc_packets_total: last?.unmatched_twcc_packets_total ?? null,
        delay_trend_ms: range(group.samples.map(sample => sample.delay_trend_ms)),
        delay_slope: range(group.samples.map(sample => sample.delay_slope)),
        rejected_packets_total: last?.rejected_packets_total ?? null,
        valid_allocation_estimate_seen: group.allocations.some(allocation => allocation.budget_estimate_valid),
        state_events: group.state_events, probe_events: group.probe_events};
    })};
}
