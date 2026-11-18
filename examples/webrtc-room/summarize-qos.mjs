import {writeFile} from 'node:fs/promises';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {readQosLog, selectRoom, summarizeQos} from './qos-log.mjs';

const directory = dirname(fileURLToPath(import.meta.url));
const input = resolve(process.env.PACKETIA_QOS_TRACE_FILE || resolve(directory, 'artifacts/server-qos.jsonl'));
let records = await readQosLog(input);
if (process.env.PACKETIA_QOS_ROOM) records = selectRoom(records, process.env.PACKETIA_QOS_ROOM);
if (!records.length) throw new Error('No QoS events found; enable PACKETIA_QOS_LOG=1 and PACKETIA_LOG_FORMAT=json');
const summary = summarizeQos(records);
const output = resolve(directory, 'artifacts/server-qos-summary.json');
await writeFile(output, JSON.stringify(summary, null, 2));
console.log(`Saved ${output}`);
for (const transport of summary.transports)
  console.log(`transport=${transport.transport_id} samples=${transport.sample_count} sources=${transport.feedback_sources.join(',')} twcc=${transport.twcc_reports_total} rr=${transport.rr_reports_total}`);
