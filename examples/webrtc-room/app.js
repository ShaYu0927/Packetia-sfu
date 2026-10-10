const $ = id => document.getElementById(id);
const config = await (await fetch('/config')).json();
let socket, pc, localStream, participant, roomId, busy = false, available = [];
let requests = Promise.resolve();
let pending;
let mode = '';
let candidateErrors = [], gatheredCandidates = [];
let resumeToken, reconnectTimeout = 15000, recovery, iceRestart;
let leaving = false, lifecycle = 0, recoveryCount = 0, iceRestartCount = 0;

function controls() {
  const joined = Boolean(participant);
  const unavailable = busy || Boolean(recovery);
  $('join').disabled = joined || unavailable;
  $('room').disabled = joined || busy;
  $('relay').disabled = Boolean(pc) || busy;
  $('leave').disabled = !joined || busy;
  $('publish').disabled = !joined || Boolean(pc) || unavailable;
  $('publisher').disabled = !joined || Boolean(pc) || busy;
  $('watch').disabled = !joined || Boolean(pc) || unavailable || !$('publisher').value;
}
function request(message) {
  const operation = requests.then(() => new Promise((resolve, reject) => {
    if (socket?.readyState !== WebSocket.OPEN) {reject(new Error('Signaling disconnected')); return;}
    const timeout = setTimeout(() => {
      pending = undefined;
      reject(new Error('Signaling request timed out'));
      socket?.close();
    }, 10000);
    pending = {resolve, reject, timeout};
    socket.send(JSON.stringify({...message, token: config.token}));
  }));
  requests = operation.catch(() => {});
  return operation;
}
function reset() {
  ++lifecycle;
  pc?.close(); pc = undefined;
  localStream?.getTracks().forEach(track => track.stop()); localStream = undefined;
  $('local').srcObject = $('remote').srcObject = null;
  participant = roomId = undefined;
  resumeToken = undefined;
  available = []; mode = '';
  $('publisher').replaceChildren(new Option('No published tracks', ''));
  $('identity').textContent = 'No room selected';
  $('connection').textContent = 'Disconnected';
  $('local-status').textContent = $('remote-status').textContent = 'Idle';
  $('ice').textContent = 'Idle'; $('candidate').textContent = '-';
  $('frames').textContent = $('bytes').textContent = '0';
  controls();
}
async function openSocket() {
  socket = new WebSocket(config.url, 'packetia');
  const current = socket;
  current.addEventListener('message', event => {
    if (!pending || current !== socket) return;
    const reply = JSON.parse(event.data);
    const operation = pending;
    pending = undefined; clearTimeout(operation.timeout);
    if (reply.type === 'error') operation.reject(new Error(reply.error));
    else operation.resolve(reply);
  });
  current.addEventListener('close', () => {
    if (current !== socket) return;
    if (pending) {clearTimeout(pending.timeout); pending.reject(new Error('Signaling disconnected')); pending = undefined;}
    if (participant && resumeToken && !leaving) {
      if (!recovery) {
        $('connection').textContent = 'Reconnecting';
        recovery = recover(lifecycle).catch(error => {
          $('error').textContent = error.message;
          reset(); socket?.close();
        }).finally(() => {recovery = undefined; controls();});
        controls();
      }
    } else if (!recovery) reset();
  });
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {current.close(); reject(new Error('Signaling connection timed out'));}, 5000);
    current.addEventListener('open', () => {clearTimeout(timeout); resolve();}, {once: true});
    current.addEventListener('close', () => {clearTimeout(timeout); reject(new Error('Could not connect signaling'));}, {once: true});
    current.addEventListener('error', () => {clearTimeout(timeout); reject(new Error('Could not connect signaling'));}, {once: true});
  });
}
async function recover(epoch) {
  const deadline = Date.now() + reconnectTimeout;
  let delay = 250, lastError;
  while (!leaving && lifecycle === epoch && Date.now() < deadline) {
    await new Promise(resolve => setTimeout(resolve, delay));
    if (leaving || lifecycle !== epoch) return;
    try {
      await openSocket();
      const reply = await request({type: 'resume', room_id: roomId, participant_id: participant, resume_token: resumeToken});
      if (leaving || lifecycle !== epoch) return;
      if (reply.type !== 'resumed') throw new Error('Unexpected recovery reply');
      resumeToken = reply.resume_token;
      if (reply.ice_restart) {
        if (!pc) throw new Error('Media connection is unavailable');
        await restartIce();
      }
      ++recoveryCount;
      $('connection').textContent = pc?.connectionState || 'Joined';
      await refreshTracks();
      return;
    } catch (error) {
      lastError = error;
      socket?.close();
      delay = Math.min(delay * 2, 2000);
      if (/expired|Invalid participant|Unauthorized|join again/.test(error.message)) break;
    }
  }
  if (!leaving && lifecycle === epoch) throw lastError || new Error('Participant recovery timed out');
}
async function join(id = $('room').value) {
  leaving = false;
  await openSocket();
  const reply = await request({type: 'join', room_id: id});
  participant = reply.participant_id; roomId = id;
  resumeToken = reply.resume_token;
  reconnectTimeout = reply.reconnect_timeout_ms;
  $('room').value = id;
  $('identity').textContent = `${id} / ${participant}`;
  $('connection').textContent = 'Joined';
  await refreshTracks(); controls();
  return reply;
}
async function refreshTracks() {
  if (!participant) return [];
  const reply = await request({type: 'tracks'});
  available = reply.tracks.filter(track => track.publisher_id !== participant);
  const previous = $('publisher').value;
  const publishers = [...new Set(available.map(track => track.publisher_id))];
  $('publisher').replaceChildren(...(publishers.length ? publishers.map(id => new Option(id, id)) : [new Option('No published tracks', '')]));
  if (publishers.includes(previous)) $('publisher').value = previous;
  controls(); return available;
}
function connection(forceRelay) {
  candidateErrors = []; gatheredCandidates = [];
  pc = new RTCPeerConnection({iceServers: [config.iceServer], iceTransportPolicy: forceRelay ? 'relay' : 'all'});
  const current = pc;
  current.onicecandidate = event => {
    if (event.candidate) gatheredCandidates.push({type: event.candidate.type, protocol: event.candidate.protocol});
  };
  current.onicecandidateerror = event => {
    candidateErrors.push({code: event.errorCode, text: event.errorText});
  };
  current.onconnectionstatechange = () => {
    if (current === pc) $('connection').textContent = current.connectionState;
  };
  current.oniceconnectionstatechange = () => {
    if (current !== pc) return;
    $('ice').textContent = current.iceConnectionState;
    if (current.iceConnectionState === 'failed' && !recovery && mode && socket?.readyState === WebSocket.OPEN)
      restartIce().catch(error => {$('error').textContent = error.message;});
  };
  current.ontrack = event => {
    if (current !== pc) return;
    const stream = $('remote').srcObject || new MediaStream();
    stream.addTrack(event.track); $('remote').srcObject = stream;
    $('remote').play().catch(() => {});
    $('remote-status').textContent = 'Receiving';
  };
  return current;
}
function preferences(transceiver, kind) {
  const name = kind === 'video' ? 'video/h264' : 'audio/opus';
  const codecs = RTCRtpReceiver.getCapabilities(kind).codecs.filter(codec => codec.mimeType.toLowerCase() === name);
  if (!codecs.length) throw new Error(`${name} is unavailable in this browser`);
  transceiver.setCodecPreferences(codecs);
}
async function offer(restart = false) {
  const current = pc;
  if (restart) {candidateErrors = []; gatheredCandidates = [];}
  const description = await current.createOffer({iceRestart: restart});
  // A restart can still report the previous generation as complete until
  // gathering begins. Register before setLocalDescription and await its events.
  let finish;
  const gathering = new Promise(resolve => {
    finish = () => {
      clearTimeout(timeout);
      current.removeEventListener('icegatheringstatechange', changed);
      current.removeEventListener('icecandidate', candidate);
      resolve();
    };
    let started = false;
    const changed = () => {
      if (current.iceGatheringState === 'gathering') started = true;
      if (started && current.iceGatheringState === 'complete') finish();
    };
    const candidate = event => {if (!event.candidate) finish();};
    const timeout = setTimeout(finish, 20000);
    current.addEventListener('icegatheringstatechange', changed);
    current.addEventListener('icecandidate', candidate);
  });
  try {await current.setLocalDescription(description); await gathering;}
  catch (error) {finish(); throw error;}
  if (current !== pc || current.signalingState === 'closed') throw new Error('Media connection closed');
  const relayOnly = current.getConfiguration().iceTransportPolicy === 'relay';
  if (!gatheredCandidates.some(candidate => candidate.protocol === 'udp' && (!relayOnly || candidate.type === 'relay')))
    throw new Error(relayOnly ? 'No UDP TURN candidate was gathered' : 'No UDP ICE candidate was gathered');
  return current.localDescription.sdp;
}
function restartIce() {
  if (iceRestart) return iceRestart;
  const current = pc;
  iceRestart = (async () => {
    if (!current || current.signalingState !== 'stable') throw new Error('Media negotiation is not stable');
    try {
      const reply = await request({type: 'restart_ice', sdp: await offer(true)});
      if (current !== pc) return;
      await current.setRemoteDescription({type: 'answer', sdp: reply.sdp});
      ++iceRestartCount;
    } catch (error) {
      if (current === pc && current.signalingState === 'have-local-offer')
        await current.setLocalDescription({type: 'rollback'}).catch(() => {});
      throw error;
    }
  })().finally(() => {iceRestart = undefined;});
  return iceRestart;
}
async function publish(forceRelay = $('relay').checked) {
  if (!participant || pc) throw new Error('Join a room with an idle connection');
  localStream = await navigator.mediaDevices.getUserMedia({video: {width: 640, height: 360, frameRate: 20}, audio: true});
  $('local').srcObject = localStream;
  const current = connection(forceRelay);
  for (const track of localStream.getTracks()) {
    const transceiver = current.addTransceiver(track, {direction: 'sendonly', streams: [localStream]});
    preferences(transceiver, track.kind);
  }
  const reply = await request({type: 'publish', sdp: await offer()});
  await current.setRemoteDescription({type: 'answer', sdp: reply.sdp});
  mode = 'publish'; $('local-status').textContent = 'Publishing'; controls();
  return reply;
}
async function watch(publisherId = $('publisher').value, forceRelay = $('relay').checked) {
  if (!participant || pc) throw new Error('Join a room with an idle connection');
  await refreshTracks();
  const tracks = available.filter(track => track.publisher_id === publisherId);
  if (!tracks.length) throw new Error('Publisher has no available tracks');
  const current = connection(forceRelay);
  const bindings = tracks.map(track => {
    const transceiver = current.addTransceiver(track.kind, {direction: 'recvonly'});
    preferences(transceiver, track.kind);
    return {track, transceiver};
  });
  const sdp = await offer();
  const reply = await request({type: 'subscribe', sdp,
    tracks: bindings.map(({track, transceiver}) => ({track_id: track.track_id, mid: transceiver.mid}))});
  await current.setRemoteDescription({type: 'answer', sdp: reply.sdp});
  mode = 'watch'; controls(); return reply;
}
async function stats() {
  if (!pc) return {};
  const report = await pc.getStats();
  let frames = 0, bytes = 0, audioBytes = 0, candidateType = '', pair;
  for (const stat of report.values()) {
    if (stat.type === 'transport' && stat.selectedCandidatePairId) pair = report.get(stat.selectedCandidatePairId);
    if (stat.type === 'inbound-rtp' && stat.kind === 'video') {frames += stat.framesDecoded || 0; bytes += stat.bytesReceived || 0;}
    if (stat.type === 'inbound-rtp' && stat.kind === 'audio') audioBytes += stat.bytesReceived || 0;
  }
  if (pair) candidateType = report.get(pair.localCandidateId)?.candidateType || '';
  $('candidate').textContent = candidateType || '-';
  $('frames').textContent = String(frames); $('bytes').textContent = String(bytes);
  return {frames, bytes, audioBytes, candidateType, state: pc.connectionState, ice: pc.iceConnectionState,
    gatheringState: pc.iceGatheringState, gatheredCandidates, candidateErrors,
    participant, roomId, mode, recoveryCount, iceRestartCount,
    width: $('remote').videoWidth, height: $('remote').videoHeight};
}
async function leave() {
  leaving = true;
  ++lifecycle;
  try {if (participant) await request({type: 'leave'});} finally {socket?.close(); reset();}
}
async function action(operation) {
  busy = true; $('error').textContent = ''; controls();
  try {await operation();} catch (error) {
    $('error').textContent = error.message;
    if (pc && !mode) await leave().catch(() => {});
  } finally {busy = false; controls();}
}
$('join-form').onsubmit = event => {event.preventDefault(); action(() => join());};
$('publish').onclick = () => action(() => publish());
$('watch').onclick = () => action(() => watch());
$('leave').onclick = () => action(leave);
$('publisher').onchange = controls;
setInterval(() => {if (!busy && !recovery && participant && !pc) refreshTracks().catch(() => {}); stats().catch(() => {});}, 2000);
window.addEventListener('pagehide', () => {leaving = true; pc?.close(); localStream?.getTracks().forEach(track => track.stop()); socket?.close();});
window.demo = {join, publish, watch, refreshTracks, stats, leave, restartIce, dropSignaling: () => socket?.close()};
controls();
