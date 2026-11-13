const $ = id => document.getElementById(id);
const config = await (await fetch('/config')).json();
const identity = `demo-${crypto.randomUUID()}`;
const roles = ['publish', 'subscribe'];
const fresh = role => ({role, pc: undefined, sessionId: undefined, stream: undefined, starting: false,
  stopping: false, restarting: undefined, candidates: [], errors: [], bindings: [], views: new Map(), restarts: 0});
const media = {publish: fresh('publish'), subscribe: fresh('subscribe')};
let socket, participant, participantInfo, roomId, resumeToken, recovery;
let busy = false, leaving = false, lifecycle = 0, memberRevision = 0, available = [];
let reconnectTimeout = 15000, recoveryCount = 0, iceRestartCount = 0, role = 'participant';
let requests = Promise.resolve(), pending;
const active = state => media[state.role] === state && Boolean(participant) && !leaving;
const hasMedia = () => roles.some(role => media[role].pc || media[role].starting);

function controls() {
  const joined = Boolean(participant), unavailable = busy || Boolean(recovery);
  $('join').disabled = joined || unavailable;
  $('room').disabled = $('role').disabled = joined || busy;
  $('relay').disabled = hasMedia() || unavailable;
  $('leave').disabled = !joined || busy;
  $('publish').disabled = !joined || !participantInfo?.permissions.can_publish || Boolean(media.publish.pc) || media.publish.starting || unavailable;
  $('publisher').disabled = !joined || Boolean(media.subscribe.pc) || unavailable;
  $('watch').disabled = !joined || !participantInfo?.permissions.can_subscribe || Boolean(media.subscribe.pc) || media.subscribe.starting || unavailable || !$('publisher').value;
  $('stop-publish').disabled = !media.publish.sessionId || unavailable || media.publish.stopping;
  $('stop-watch').disabled = !media.subscribe.sessionId || unavailable || media.subscribe.stopping;
}
function connectionStatus() {
  if (recovery) return;
  const states = roles.filter(role => media[role].pc).map(role => `${role}: ${media[role].pc.connectionState}`);
  $('connection').textContent = states.join(' / ') || (participant ? 'Joined' : 'Disconnected');
}
async function roomToken(id) {
  const response = await fetch('/token', {method: 'POST', signal: AbortSignal.timeout(5000), headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({identity, room: id, role})});
  if (!response.ok) throw new Error('Could not obtain a room token');
  return (await response.json()).token;
}
function request(message) {
  const epoch = lifecycle, target = socket;
  const operation = requests.then(() => new Promise((resolve, reject) => {
    if (epoch !== lifecycle || target !== socket || target?.readyState !== WebSocket.OPEN) {
      reject(new Error('Signaling disconnected or operation cancelled')); return;
    }
    const timeout = setTimeout(() => {
      pending = undefined; reject(new Error('Signaling request timed out')); target.close();
    }, 10000);
    pending = {resolve, reject, timeout};
    target.send(JSON.stringify(message));
  }));
  requests = operation.catch(() => {});
  return operation;
}
function emptyRemote() {
  if (!$('remotes').children.length) {
    const section = document.createElement('section'); section.id = 'remote-empty';
    section.innerHTML = '<div class="video-title"><h2>Remote participants</h2><span>Idle</span></div><div class="video-surface"><span class="placeholder">Publishers will appear here</span></div>';
    $('remotes').append(section);
  }
}
function resetMedia(role) {
  if (!role) {for (const item of roles) resetMedia(item); return;}
  const previous = media[role]; media[role] = fresh(role);
  previous.pc?.close(); previous.stream?.getTracks().forEach(track => track.stop());
  $(`${role}-ice`).textContent = 'Idle';
  if (role === 'publish') {$('local').srcObject = null; $('local-status').textContent = 'Idle';}
  else {
    for (const view of previous.views.values()) view.video.srcObject = null;
    $('remotes').replaceChildren(); emptyRemote();
    $('candidate').textContent = '-'; $('frames').textContent = $('bytes').textContent = '0';
  }
  connectionStatus(); controls();
}
function reset() {
  ++lifecycle; resetMedia();
  participant = participantInfo = roomId = resumeToken = undefined; memberRevision = 0;
  available = []; $('publisher').replaceChildren(new Option('No published tracks', ''));
  $('identity').textContent = 'No room selected';
  $('connection').textContent = 'Disconnected'; controls();
}
function applyStops(reply) {
  if (Array.isArray(reply.stopped_sessions)) {
    for (const stopped of reply.stopped_sessions)
      if (media[stopped.role]?.sessionId === stopped.session_id) resetMedia(stopped.role);
  } else if (reply.media_stopped) {
    for (const role of roles)
      if (!participantInfo.permissions[role === 'publish' ? 'can_publish' : 'can_subscribe']) resetMedia(role);
  }
}
async function openSocket() {
  const current = new WebSocket(config.url, 'packetia'); socket = current;
  current.addEventListener('message', event => {
    if (current !== socket) return;
    const reply = JSON.parse(event.data);
    if (reply.type === 'tracks_changed') {
      if (participant && reply.room_id === roomId) updateTracks(reply.tracks);
      return;
    }
    if (reply.type === 'participants_changed') {
      if (reply.room_id === roomId) memberRevision = reply.revision;
      return;
    }
    if (reply.type === 'permissions_changed') {
      if (reply.room_id === roomId && reply.participant.participant_id === participant) {
        participantInfo = reply.participant; memberRevision = reply.revision; applyStops(reply); controls();
      }
      return;
    }
    if (reply.type === 'media_closed') {
      if (media[reply.role]?.sessionId === reply.session_id) resetMedia(reply.role);
      $('error').textContent = reply.reason; return;
    }
    if (reply.type === 'removed') {
      leaving = true;
      if (pending) {clearTimeout(pending.timeout); pending.reject(new Error('Removed from room')); pending = undefined;}
      reset(); current.close(); $('error').textContent = 'Removed from room'; return;
    }
    if (reply.type === 'error' && reply.negotiation_id) {$('error').textContent = reply.error; return;}
    if (!pending) return;
    const operation = pending; pending = undefined; clearTimeout(operation.timeout);
    if (reply.type === 'error') operation.reject(new Error(reply.error)); else operation.resolve(reply);
  });
  current.addEventListener('close', () => {
    if (current !== socket) return;
    if (pending) {clearTimeout(pending.timeout); pending.reject(new Error('Signaling disconnected')); pending = undefined;}
    if (participant && resumeToken && !leaving) {
      if (!recovery) {
        $('connection').textContent = 'Reconnecting';
        const epoch = lifecycle;
        recovery = recover(epoch).catch(error => {
          if (epoch !== lifecycle || leaving) return;
          $('error').textContent = error.message; reset(); socket?.close();
        }).finally(() => {recovery = undefined; connectionStatus(); controls();});
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
      const token = await roomToken(roomId);
      const reply = await request({type: 'resume', token, room_id: roomId, participant_id: participant, resume_token: resumeToken});
      if (leaving || lifecycle !== epoch) return;
      if (reply.type !== 'resumed') throw new Error('Unexpected recovery reply');
      resumeToken = reply.resume_token; participantInfo = reply.participant; memberRevision = reply.revision;
      const sessions = reply.media_sessions || (reply.ice_restart ? roles.filter(role => media[role].sessionId)
        .map(role => ({role, session_id: media[role].sessionId, ice_restart: true})) : []);
      for (const role of roles) {
        const state = media[role], saved = sessions.find(session => session.role === role);
        if (!saved) {resetMedia(role); continue;}
        if (!state.pc || saved.session_id !== state.sessionId) throw new Error('Media session is unavailable');
        if (state.restarting) await state.restarting.catch(() => {});
        if (saved.ice_restart) await restartIce(role);
      }
      ++recoveryCount; await refreshTracks(); return;
    } catch (error) {
      lastError = error; socket?.close(); delay = Math.min(delay * 2, 2000);
      if (/expired|Invalid participant|Unauthorized|join again/.test(error.message)) break;
    }
  }
  if (!leaving && lifecycle === epoch) throw lastError || new Error('Participant recovery timed out');
}
async function join(id = $('room').value) {
  leaving = false; role = $('role').value;
  const token = await roomToken(id); await openSocket();
  const reply = await request({type: 'join', token, room_id: id});
  participant = reply.participant_id; roomId = id; participantInfo = reply.participant; memberRevision = reply.revision;
  resumeToken = reply.resume_token; reconnectTimeout = reply.reconnect_timeout_ms;
  $('room').value = id; $('identity').textContent = `${id} / ${participant}`;
  $('connection').textContent = 'Joined'; await refreshTracks(); controls(); return reply;
}
async function refreshTracks() {
  if (!participant) return [];
  const reply = await request({type: 'tracks'}); return updateTracks(reply.tracks);
}
async function listParticipants(options = {}) {return request({...options, type: 'participants'});}
async function getParticipant(id = participant) {return request({type: 'participant', participant_id: id});}
async function updateParticipant(patch, id = participant) {
  const reply = await request({...patch, type: 'update_participant', participant_id: id});
  if (id === participant) {participantInfo = reply.participant; applyStops(reply); controls();}
  memberRevision = reply.revision; return reply;
}
async function removeParticipant(id) {return request({type: 'remove_participant', participant_id: id});}
function updateTracks(tracks) {
  available = tracks.filter(track => track.publisher_id !== participant);
  const previous = $('publisher').value, publishers = [...new Set(available.map(track => track.publisher_id))];
  $('publisher').replaceChildren(...(publishers.length ? [new Option('All other publishers', '*'), ...publishers.map(id => new Option(id, id))]
    : [new Option('No published tracks', '')]));
  if (previous === '*' || publishers.includes(previous)) $('publisher').value = previous;
  const state = media.subscribe, ids = new Set(available.map(track => track.track_id));
  for (const binding of state.bindings)
    if (!ids.has(binding.track.track_id) && binding.received) {
      state.views.get(binding.track.publisher_id)?.stream.removeTrack(binding.received);
      binding.received = undefined;
    }
  for (const [id, view] of state.views)
    if (!view.stream.getTracks().length) {view.video.srcObject = null; view.section.remove(); state.views.delete(id);}
  emptyRemote(); controls(); return available;
}
function receive(state, event) {
  if (!active(state)) return;
  const binding = state.bindings.find(item => item.transceiver === event.transceiver);
  if (!binding) return;
  const id = binding.track.publisher_id;
  let view = state.views.get(id);
  if (!view) {
    $('remote-empty')?.remove();
    const section = document.createElement('section'); section.dataset.publisher = id;
    const title = document.createElement('div'); title.className = 'video-title';
    const name = document.createElement('h2'); name.textContent = id;
    const status = document.createElement('span'); status.textContent = 'Receiving'; title.append(name, status);
    const surface = document.createElement('div'); surface.className = 'video-surface';
    const video = document.createElement('video'); video.autoplay = video.playsInline = video.controls = true;
    if (!$('remote')) video.id = 'remote';
    surface.append(video); section.append(title, surface); $('remotes').append(section);
    view = {section, video, stream: new MediaStream()}; state.views.set(id, view); video.srcObject = view.stream;
  }
  if (binding.received) view.stream.removeTrack(binding.received);
  binding.received = event.track; view.stream.addTrack(event.track); view.video.play().catch(() => {});
}
function connection(state, forceRelay) {
  state.pc = new RTCPeerConnection({iceServers: [config.iceServer], iceTransportPolicy: forceRelay ? 'relay' : 'all'});
  const current = state.pc;
  current.onicecandidate = event => {
    if (active(state) && event.candidate) state.candidates.push({type: event.candidate.type, protocol: event.candidate.protocol});
  };
  current.onicecandidateerror = event => {if (active(state)) state.errors.push({code: event.errorCode, text: event.errorText});};
  current.onconnectionstatechange = () => {if (active(state)) connectionStatus();};
  current.oniceconnectionstatechange = () => {
    if (!active(state)) return;
    $(`${state.role}-ice`).textContent = current.iceConnectionState;
    if (current.iceConnectionState === 'failed' && state.sessionId && !state.starting && !state.stopping && !recovery && socket?.readyState === WebSocket.OPEN)
      restartIce(state.role).catch(error => {$('error').textContent = error.message;});
  };
  current.ontrack = event => receive(state, event);
  return current;
}
function preferences(transceiver, kind) {
  const name = kind === 'video' ? 'video/h264' : 'audio/opus';
  const codecs = RTCRtpReceiver.getCapabilities(kind).codecs.filter(codec => codec.mimeType.toLowerCase() === name);
  if (!codecs.length) throw new Error(`${name} is unavailable in this browser`);
  transceiver.setCodecPreferences(codecs);
}
async function offer(state, restart = false) {
  const current = state.pc;
  if (restart) {state.candidates = []; state.errors = [];}
  const description = await current.createOffer({iceRestart: restart});
  let finish;
  const gathering = new Promise(resolve => {
    finish = () => {
      clearTimeout(timeout); current.removeEventListener('icegatheringstatechange', changed);
      current.removeEventListener('icecandidate', candidate); resolve();
    };
    let started = false;
    const changed = () => {
      if (current.iceGatheringState === 'gathering') started = true;
      if (started && current.iceGatheringState === 'complete') finish();
    };
    const candidate = event => {if (!event.candidate) finish();};
    const timeout = setTimeout(finish, 20000);
    current.addEventListener('icegatheringstatechange', changed); current.addEventListener('icecandidate', candidate);
  });
  try {await current.setLocalDescription(description); await gathering;} catch (error) {finish(); throw error;}
  if (!active(state) || current.signalingState === 'closed') throw new Error('Media operation cancelled');
  const relayOnly = current.getConfiguration().iceTransportPolicy === 'relay';
  if (!state.candidates.some(candidate => candidate.protocol === 'udp' && (!relayOnly || candidate.type === 'relay')))
    throw new Error(relayOnly ? 'No UDP TURN candidate was gathered' : 'No UDP ICE candidate was gathered');
  return current.localDescription.sdp;
}
function restartIce(role) {
  if (!roles.includes(role)) {
    if (roles.filter(role => media[role].pc).length !== 1) return Promise.reject(new Error('Choose publish or subscribe for ICE restart'));
    role = roles.find(role => media[role].pc);
  }
  const state = media[role];
  if (state.restarting) return state.restarting;
  state.restarting = (async () => {
    if (!active(state) || !state.sessionId || state.pc.signalingState !== 'stable') throw new Error('Media negotiation is not stable');
    try {
      const reply = await request({type: 'restart_ice', session_id: state.sessionId, sdp: await offer(state, true)});
      if (!active(state)) return;
      await state.pc.setRemoteDescription({type: 'answer', sdp: reply.sdp});
      ++state.restarts; ++iceRestartCount;
    } catch (error) {
      if (active(state) && state.pc.signalingState === 'have-local-offer') await state.pc.setLocalDescription({type: 'rollback'}).catch(() => {});
      throw error;
    }
  })().finally(() => {state.restarting = undefined;});
  return state.restarting;
}
async function negotiate(state, message) {
  const reply = await request(message);
  if (!active(state)) throw new Error('Media operation cancelled');
  state.sessionId = reply.session_id;
  await state.pc.setRemoteDescription({type: 'answer', sdp: reply.sdp});
  return reply;
}
async function failed(state) {
  if (active(state) && state.sessionId && socket?.readyState === WebSocket.OPEN)
    await request({type: state.role === 'publish' ? 'unpublish' : 'unsubscribe', session_id: state.sessionId}).catch(() => {});
  if (media[state.role] === state) resetMedia(state.role);
}
async function publish(forceRelay = $('relay').checked) {
  const state = media.publish;
  if (!participant || state.pc || state.starting || recovery) throw new Error('Join a room with an idle publishing connection');
  state.starting = true; controls();
  try {
    const stream = await navigator.mediaDevices.getUserMedia({video: {width: 640, height: 360, frameRate: 20}, audio: true});
    if (!active(state)) {stream.getTracks().forEach(track => track.stop()); throw new Error('Media operation cancelled');}
    state.stream = stream; $('local').srcObject = stream;
    const current = connection(state, forceRelay);
    for (const track of stream.getTracks()) preferences(current.addTransceiver(track, {direction: 'sendonly', streams: [stream]}), track.kind);
    const reply = await negotiate(state, {type: 'publish', sdp: await offer(state)});
    $('local-status').textContent = 'Publishing'; return reply;
  } catch (error) {await failed(state); throw error;} finally {state.starting = false; controls();}
}
async function watch(publisherId = $('publisher').value, forceRelay = $('relay').checked) {
  const state = media.subscribe;
  if (!participant || state.pc || state.starting || recovery) throw new Error('Join a room with an idle subscribing connection');
  state.starting = true; controls();
  try {
    await refreshTracks();
    if (!active(state)) throw new Error('Media operation cancelled');
    const tracks = available.filter(track => publisherId === '*' || track.publisher_id === publisherId);
    if (!tracks.length || tracks.length > 8) throw new Error('Choose publishers with 1–8 tracks');
    const current = connection(state, forceRelay);
    state.bindings = tracks.map(track => {
      const transceiver = current.addTransceiver(track.kind, {direction: 'recvonly'}); preferences(transceiver, track.kind);
      return {track, transceiver};
    });
    const sdp = await offer(state);
    return await negotiate(state, {type: 'subscribe', sdp,
      tracks: state.bindings.map(({track, transceiver}) => ({track_id: track.track_id, mid: transceiver.mid}))});
  } catch (error) {await failed(state); throw error;} finally {state.starting = false; controls();}
}
async function stopMedia(role) {
  if (!role) {for (const item of roles) await stopMedia(item); return;}
  if (!roles.includes(role)) throw new Error('Unknown media role');
  const state = media[role]; if (!state.sessionId || state.stopping) return;
  state.stopping = true; controls();
  try {
    if (state.restarting) await state.restarting;
    const reply = await request({type: role === 'publish' ? 'unpublish' : 'unsubscribe', session_id: state.sessionId});
    if (media[role] === state) resetMedia(role);
    return reply;
  } finally {state.stopping = false; controls();}
}
async function roleStats(state) {
  if (!state.pc) return undefined;
  const report = await state.pc.getStats();
  if (!active(state)) return undefined;
  let frames = 0, bytes = 0, audioBytes = 0, sentBytes = 0, candidateType = '', pair;
  for (const stat of report.values()) {
    if (stat.type === 'transport' && stat.selectedCandidatePairId) pair = report.get(stat.selectedCandidatePairId);
    if (stat.type === 'inbound-rtp' && stat.kind === 'video') {frames += stat.framesDecoded || 0; bytes += stat.bytesReceived || 0;}
    if (stat.type === 'inbound-rtp' && stat.kind === 'audio') audioBytes += stat.bytesReceived || 0;
    if (stat.type === 'outbound-rtp') sentBytes += stat.bytesSent || 0;
  }
  if (pair) candidateType = report.get(pair.localCandidateId)?.candidateType || '';
  return {frames, bytes, audioBytes, sentBytes, candidateType, sessionId: state.sessionId, restarts: state.restarts,
    state: state.pc.connectionState, ice: state.pc.iceConnectionState, gatheringState: state.pc.iceGatheringState,
    gatheredCandidates: state.candidates, candidateErrors: state.errors};
}
async function stats() {
  const [publish, subscribe] = await Promise.all(roles.map(role => roleStats(media[role])));
  const current = subscribe || publish, remoteVideos = [...media.subscribe.views].map(([id, view]) =>
    ({publisher: id, width: view.video.videoWidth, height: view.video.videoHeight}));
  $('candidate').textContent = current?.candidateType || '-';
  $('frames').textContent = String(subscribe?.frames || 0); $('bytes').textContent = String(subscribe?.bytes || 0);
  const mode = publish && subscribe ? 'duplex' : publish ? 'publish' : subscribe ? 'watch' : '';
  return {...current, publish, subscribe, remoteVideos, participant, roomId, mode, recoveryCount, iceRestartCount,
    width: remoteVideos[0]?.width || 0, height: remoteVideos[0]?.height || 0};
}
async function leave() {
  leaving = true; ++lifecycle;
  try {if (participant) await request({type: 'leave'});} finally {socket?.close(); reset();}
}
async function action(operation) {
  busy = true; $('error').textContent = ''; controls();
  try {await operation();} catch (error) {$('error').textContent = error.message;} finally {busy = false; controls();}
}
$('join-form').onsubmit = event => {event.preventDefault(); action(() => join());};
$('publish').onclick = () => action(() => publish()); $('watch').onclick = () => action(() => watch());
$('stop-publish').onclick = () => action(() => stopMedia('publish'));
$('stop-watch').onclick = () => action(() => stopMedia('subscribe'));
$('leave').onclick = () => action(leave); $('publisher').onchange = controls;
setInterval(() => {if (!busy && !recovery && participant && !media.subscribe.pc) refreshTracks().catch(() => {}); stats().catch(() => {});}, 2000);
window.addEventListener('pagehide', () => {leaving = true; resetMedia(); socket?.close();});
window.demo = {join, publish, watch, stopMedia, refreshTracks, stats, leave, restartIce,
  listParticipants, getParticipant, updateParticipant, removeParticipant,
  get memberRevision() {return memberRevision;}, dropSignaling: () => socket?.close()};
emptyRemote(); controls();
