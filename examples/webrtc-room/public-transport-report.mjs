const fmt = (value, digits = 1) => Number.isFinite(value) ? value.toFixed(digits) : '—';

export function analyzePublicTransport(result) {
  const samples = result.phases.flatMap(phase => phase.samples);
  const first = samples[0], last = samples.at(-1);
  const subscriber = result.final_states?.[1]?.connections[0];
  const local = subscriber?.twcc_local_extension_ids;
  const remote = subscriber?.twcc_remote_extension_ids;
  return {
    sample_count: samples.length, measurement_started_at: first?.at, measurement_ended_at: last?.at,
    measurement_span_seconds: first && last ? (Date.parse(last.at) - Date.parse(first.at)) / 1000 : null,
    measured_window_seconds: result.phases.reduce((sum, phase) => sum + (phase.summary?.measured_seconds || 0), 0),
    nack_messages_in_measured_windows: result.phases.reduce((sum, phase) => sum + (phase.summary?.receiver_nack_delta || 0), 0),
    freezes_in_measured_windows: result.phases.reduce((sum, phase) => sum + (phase.summary?.receiver_freeze_delta || 0), 0),
    subscriber_twcc: local && remote ? {offer_extension_ids: [...new Set(local)], answer_extension_ids: [...new Set(remote)],
      extension_present_in_both: local.length > 0 && remote.length > 0} : null
  };
}

export function renderPublicTransportReport(result) {
  const analysis = analyzePublicTransport(result);
  const lines = ['# 公网音视频传输测试报告', '',
    `功能检查：**${result.passed ? '通过' : '未通过／未完成'}**。此结果不代表质量指标全部达标。`, '',
    `- 服务器：${result.endpoint}；媒体选中地址见各阶段原始 candidate pair。`,
    `- 浏览器：Chrome ${result.browser_version || '未知'}；两个独立浏览器进程。`,
    '- 场景：同一台 Mac 推送一路模拟摄像头和音频，通过公网 SFU 接收；视频 H264、音频 Opus（以原始统计为准）。',
    `- 采样时间：${analysis.measurement_started_at || '未开始'} → ${analysis.measurement_ended_at || '未完成'}。`,
    `- ${analysis.sample_count} 组采样；采样首尾跨度 ${fmt(analysis.measurement_span_seconds)} 秒，各阶段测量窗口合计 ${fmt(analysis.measured_window_seconds)} 秒。`,
    `- 任务时间：${result.started_at} → ${result.finished_at}，包含准备、截图和浏览器退出等待，不等于媒体测量时长。`,
    '- 未注入限速、延迟或丢包；编码器码率上限调整不等同于网络带宽限制。', '',
    '| 阶段 | 采样数 | 发送视频 kbps | 接收视频 kbps | 解码 fps | 接收视频丢包计数净增比例 % | ICE RTT 均值 / P95 ms | NACK 消息增量 | 冻结次数增量 |',
    '|---|---:|---:|---:|---:|---:|---:|---:|---:|'];
  for (const phase of result.phases) {
    const s = phase.summary || {};
    lines.push(`| ${phase.name} | ${s.sample_count || 0} | ${fmt(s.send_video_kbps)} | ${fmt(s.receive_video_kbps)} | ` +
      `${fmt(s.decode_fps)} | ${fmt(s.receive_video_loss_percent, 3)} | ${fmt(s.ice_rtt_mean_ms)} / ${fmt(s.ice_rtt_p95_ms)} | ` +
      `${fmt(s.receiver_nack_delta, 0)} | ${fmt(s.receiver_freeze_delta, 0)} |`);
  }
  lines.push('', '## 恢复与实际路径', '');
  const restart = result.checks.ice_restart;
  if (restart) {
    lines.push(`订阅 ICE 重启后保留同一媒体 session，观察到恢复耗时 ${fmt(restart.observed_recovery_ms / 1000, 2)} 秒。`,
      `解码帧数从 ${restart.frames_before} 增至 ${restart.frames_after}。该耗时包含协商、等待帧增长及轮询，不是精确黑屏时长。`);
  } else lines.push('ICE 重启恢复尚未验证。');
  const pairs = result.phases.map(phase => phase.summary?.selected_receive_pair).filter(Boolean);
  const paths = [...new Set(pairs.map(pair => `${pair.remote?.address}:${pair.remote?.port}/${pair.remote?.protocol}`))];
  if (paths.length) lines.push('', `采样阶段选中的服务器媒体地址：${paths.join('、')}。这确认媒体使用公网 UDP。`);
  lines.push('', '## 需要继续排查的质量问题', '',
    `采样窗口内浏览器共报告 NACK 消息增量 ${analysis.nack_messages_in_measured_windows}，冻结次数增量 ${analysis.freezes_in_measured_windows}。`,
    'NACK 一条消息可能请求多个包，不能直接当成丢包数量；冻结计数也不能仅凭本报告归因于服务器算法。',
    '表中的 0% 表示接收端累计丢包计数在窗口内净增为零，不能排除丢包后重传恢复或乱序迟到。平均帧率正常也不能排除短暂冻结。');
  if (analysis.subscriber_twcc) {
    const twcc = analysis.subscriber_twcc;
    lines.push('', `订阅 SDP offer 的 TWCC 扩展 ID：${twcc.offer_extension_ids.join(', ') || '无'}；` +
      `answer 的 TWCC 扩展 ID：${twcc.answer_extension_ids.join(', ') || '无'}。`,
    twcc.extension_present_in_both ?
      '双方描述均包含该扩展；实际 TWCC 反馈及发送历史匹配仍需服务端日志验证。' :
      '**该订阅连接没有在双方 SDP 中协商出 TWCC 扩展，不能据此宣称 TWCC/GCC 输入链路已工作。**应检查预编译镜像的版本和订阅 SDP answer 生成逻辑。');
  }
  lines.push('', '## 数据口径与后续验证', '',
    '视频码率由同一 SSRC 的 RTCStats 字节差和实际时间差计算；视频、音频分开。缺失或不适用的指标显示 —。',
    'ICE RTT 是浏览器到 SFU 的往返时间，不是端到端播放延迟。音频字节在所有已完成阶段均增长，详见 result.json。',
    '低复杂度测试画面可能不耗尽编码上限，900 → 300 → 1500 kbps 上限变化不能单独证明拥塞降码率效果。',
    '本报告没有收集当前服务器的 BWE、Pacer、TWCC 匹配和 CPU/内存原始日志；无法把冻结或 NACK 定位到某个算法。',
    '下一步优先核对订阅 TWCC 协商，再结合服务端 QoS 日志和逐帧时间排查冻结；正常基线确认后，再独立施加 UDP 丢包、延迟和带宽限制。',
    '本次仅一路推流、一路观看，未验证多人容量、链路最大带宽或复杂 NAT/TURN。');
  if (result.error) lines.push('', '失败原因：', '```text', result.error, '```');
  return lines.join('\n') + '\n';
}
