import {sumMetric} from './quality-stats.js';

const $ = id => document.getElementById(id);
const value = (number, suffix = '', digits = 1) => Number.isFinite(number) ? `${number.toFixed(digits)}${suffix}` : '—';
const bitrate = bps => Number.isFinite(bps) ? bps >= 1000000 ? value(bps / 1000000, ' Mbps', 2) : value(bps / 1000, ' kbps', 0) : '—';
const videos = metrics => (metrics?.streams || []).filter(stream => stream.kind === 'video');

export class QualityPanel {
  samples = [];
  events = [];
  lastRecordedAt = -Infinity;

  event(type, details = {}) {
    this.events.push({at: new Date().toISOString(), type, ...details});
    if (this.events.length > 100) this.events.shift();
  }
  clear() {
    this.samples = []; this.events = []; this.lastRecordedAt = -Infinity;
    this.event('recording-cleared'); this.draw();
    $('sample-count').textContent = '0 个采样'; $('export-report').disabled = true;
  }
  update(snapshot) {
    const tx = sumMetric(videos(snapshot.publish?.metrics), 'bitrate_bps');
    const rx = sumMetric(videos(snapshot.subscribe?.metrics), 'bitrate_bps');
    $('tx-rate').textContent = bitrate(tx); $('rx-rate').textContent = bitrate(rx);
    $('quality-rtt').textContent = value(snapshot.subscribe?.metrics?.ice_rtt_ms, ' ms');
    const capture = snapshot.publish?.capture;
    const requested = snapshot.publish?.requested;
    $('capture-actual').textContent = capture ? `${capture.width || '—'} × ${capture.height || '—'} · ${value(capture.frameRate, ' fps')}` : '—';
    $('capture-requested').textContent = requested ? `请求 ${requested.width} × ${requested.height} / ${requested.fps} fps · 上限 ${bitrate(requested.max_bitrate_bps)}` : '尚未开始推流';
    const streams = [snapshot.publish, snapshot.subscribe].flatMap(state => state?.metrics?.streams || []);
    const rows = streams.map(stream => {
      const row = document.createElement('tr');
      const fields = [`${stream.direction === 'send' ? '↑ 发送' : '↓ 接收'} / ${stream.kind}`,
        stream.codec || '—', bitrate(stream.bitrate_bps), value(stream.packets_per_second), value(stream.fps),
        stream.width && stream.height ? `${stream.width} × ${stream.height}` : '—',
        value(Number.isFinite(stream.loss_rate) ? stream.loss_rate * 100 : null, '%'),
        value(stream.jitter_ms, ' ms'), value(stream.jitter_buffer_ms, ' ms'), value(stream.freezes_total, '', 0),
        stream.quality_limitation || '—'];
      for (const field of fields) {const cell = document.createElement('td'); cell.textContent = field; row.append(cell);}
      row.title = `SSRC: ${stream.ssrc}`; return row;
    });
    if (!rows.length) {
      const row = document.createElement('tr'), cell = document.createElement('td');
      cell.colSpan = 11; cell.textContent = '开始推流或订阅后显示逐轨指标'; row.append(cell); rows.push(row);
    }
    $('stream-metrics').replaceChildren(...rows);
    if ((snapshot.publish || snapshot.subscribe) && performance.now() - this.lastRecordedAt >= 900) {
      this.lastRecordedAt = performance.now();
      // Keep the latest ten minutes without retaining PeerConnections, tokens or raw SDP.
      this.samples.push({at: new Date().toISOString(), room: snapshot.roomId,
        publish: snapshot.publish ? {session_id: snapshot.publish.sessionId, state: snapshot.publish.state,
          requested, capture, encoding: snapshot.publish.encoding, metrics: snapshot.publish.metrics} : null,
        subscribe: snapshot.subscribe ? {session_id: snapshot.subscribe.sessionId, state: snapshot.subscribe.state,
          metrics: snapshot.subscribe.metrics} : null, tx_video_bps: tx, rx_video_bps: rx});
      if (this.samples.length > 600) this.samples.shift();
      $('sample-count').textContent = `${this.samples.length} 个采样`;
      $('export-report').disabled = false; this.draw();
    }
  }
  draw() {
    const points = this.samples.slice(-90);
    const ceiling = Math.max(100000, ...points.flatMap(point => [point.tx_video_bps || 0, point.rx_video_bps || 0]));
    for (const [name, field] of [['tx', 'tx_video_bps'], ['rx', 'rx_video_bps']]) {
      let path = '', connected = false;
      points.forEach((point, index) => {
        if (!Number.isFinite(point[field])) {connected = false; return;}
        const x = index / 89 * 900, y = 150 - point[field] / ceiling * 140;
        path += `${connected ? 'L' : 'M'}${x.toFixed(1)},${y.toFixed(1)} `; connected = true;
      });
      $(name + '-line').setAttribute('d', path);
    }
    $('chart-scale').textContent = points.length ? `纵轴上限 ${Math.ceil(ceiling / 1000)} kbps · 最近 ${points.length} 次采样` : '等待采样';
  }
  report() {
    return {schema_version: 1, generated_at: new Date().toISOString(), source: 'browser-getStats',
      notes: ['Rates use per-stream counter differences and RTCStats timestamps.',
        'Missing measurements are null; loss is a receive-interval measurement.',
        'ICE RTT is browser-to-SFU, not end-to-end playback delay.',
        'Changing encoder maxBitrate does not emulate network congestion.',
        'Server BWE, pacer queues and probe state are not exposed by this frontend.'],
      events: this.events.slice(), samples: this.samples.slice()};
  }
  download() {
    const url = URL.createObjectURL(new Blob([JSON.stringify(this.report(), null, 2)], {type: 'application/json'}));
    const link = document.createElement('a'); link.href = url; link.download = `packetia-quality-${Date.now()}.json`;
    link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}
