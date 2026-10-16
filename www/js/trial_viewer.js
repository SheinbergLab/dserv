let connection = null;
let files = [];
let videos = [];
let currentVideo = '';
let creating = false;
let videosReady = false;
let videoLoadGen = 0;
let scrubbing = false;
let resumeAfter = false;
let loadGen = 0;
let loading = null;
let loadingName = '';
let objectUrl = '';
let pendingFrame = -1;
let touchOn = false;
let touches = [];
let touchKey = '';
let touchGen = 0;
let availableLayers = new Set();
let layersKey = '';
let layersGen = 0;
let touchSpace = { w: 1280, h: 800 };
const FRAME_RATE = 60;
const TOUCH_HOLD = 0.2;
const TOUCH_RIDGES =
    '<path d="M32 22c-3.2 0-5.6 3-5.6 7.2 0 3.2.8 6 2 8.6"/>' +
    '<path d="M24.4 24.2c0-6 3.4-10.6 7.6-10.6s7.6 4.6 7.6 10.6c0 7.4-1.6 13.2-3.2 17"/>' +
    '<path d="M19.6 22.4c.6-8.6 5.8-14.2 12.4-14.2s11.8 5.6 12.4 14.2c.4 9-2.2 16.6-4.6 21.4"/>' +
    '<path d="M15 25c1.2-11.4 8-18 17-18s15.8 6.6 17 18c.8 9.2-1.4 18-4.4 23.6"/>' +
    '<path d="M26.2 36c1 4.4 2.2 8.4 2.4 12"/>' +
    '<path d="M36.4 34.4c.2 4.6-.4 9-1.2 12.6"/>';
const FINGERPRINT =
    '<svg viewBox="0 0 64 64" aria-hidden="true">' +
    '<g fill="none" stroke-linecap="round">' +
    '<g stroke="#000" stroke-width="5" opacity="0.45">' + TOUCH_RIDGES + '</g>' +
    '<g stroke="#fff" stroke-width="2.2">' + TOUCH_RIDGES + '</g>' +
    '</g></svg>';

function setMessage(text, isError) {
    const el = document.getElementById('message');
    el.textContent = text || '';
    el.classList.toggle('error', !!isError);
}

function setConnected(state, text) {
    const dot = document.getElementById('status-dot');
    dot.className = 'tv-dot' + (state ? ' ' + state : '');
    document.getElementById('status-text').textContent = text;
}

async function dfEval(cmd, timeoutMs) {
    return connection.evalAsync('send df {' + cmd + '}', timeoutMs);
}

function choiceLabel(value) {
    return value || '(none)';
}

function uniqueValues(rows, key) {
    const seen = new Set();
    for (const row of rows) seen.add(row[key] || '');
    return [...seen].sort((a, b) => a.localeCompare(b));
}

function setOptions(sel, items, preferred) {
    const keep = preferred !== undefined ? preferred : sel.value;
    sel.innerHTML = '';
    for (const item of items) {
        const opt = document.createElement('option');
        opt.value = item.value;
        opt.textContent = item.label;
        sel.appendChild(opt);
    }
    const values = items.map(item => item.value);
    sel.value = values.includes(keep) ? keep : (values[0] || '');
    sel.disabled = items.length === 0;
    return sel.value;
}

function blockStamp(file) {
    return [file.date, file.time].filter(Boolean).join(' ');
}

function applyFilters() {
    const subject = setOptions(
        document.getElementById('subject'),
        uniqueValues(files, 'subject').map(value => ({
            value: value,
            label: choiceLabel(value)
        }))
    );
    const subjectRows = files.filter(file => (file.subject || '') === subject);
    const system = setOptions(
        document.getElementById('system'),
        uniqueValues(subjectRows, 'system').map(value => ({
            value: value,
            label: choiceLabel(value)
        }))
    );
    const protocolRows = subjectRows.filter(file => (file.system || '') === system);
    const protocol = setOptions(
        document.getElementById('protocol'),
        uniqueValues(protocolRows, 'protocol').map(value => ({
            value: value,
            label: choiceLabel(value)
        }))
    );
    const blocks = protocolRows
        .filter(file => (file.protocol || '') === protocol)
        .sort((a, b) => (Number(b.timestamp) || 0) - (Number(a.timestamp) || 0)
            || String(b.filename).localeCompare(String(a.filename)));
    const stamps = blocks.map(blockStamp);
    setOptions(
        document.getElementById('block'),
        blocks.map(file => {
            const stamp = blockStamp(file);
            const dup = stamp && stamps.filter(item => item === stamp).length > 1;
            return {
                value: file.filename,
                label: stamp ? (dup ? stamp + '  ' + file.filename : stamp) : file.filename
            };
        })
    );
    fillTrials();
    fillLayers();
}

function selectedFile() {
    const name = document.getElementById('block').value;
    return files.find(file => file.filename === name) || null;
}

function fillTrials() {
    const file = selectedFile();
    const trial = document.getElementById('trial');
    const keep = trial.value;
    trial.innerHTML = '';
    // The player indexes observations (evt:20). n_trials is the extracted
    // table, which leaves out an observation that did not complete.
    const n = file ? (Number(file.n_obs) || 0) : 0;
    for (let i = 0; i < n; i++) {
        const opt = document.createElement('option');
        opt.value = String(i);
        trial.appendChild(opt);
    }
    const values = [...trial.options].map(opt => opt.value);
    trial.value = values.includes(keep) ? keep : (values[0] || '');
    trial.disabled = n === 0;
    paintTrials();
}

function trialVideo(index) {
    return videos.find(video => String(video.trial) === String(index)) || null;
}

function trialLabel(index) {
    const name = 'trial ' + index;
    return trialVideo(index) ? name + ' \u2713' : name;
}

function paintTrials() {
    const trial = document.getElementById('trial');
    for (const opt of trial.options) {
        opt.textContent = trialLabel(opt.value);
    }
    syncButtons();
}

function syncButtons() {
    const create = document.getElementById('create');
    const play = document.getElementById('play');
    const download = document.getElementById('download');
    const player = document.getElementById('player');
    const trial = document.getElementById('trial');
    const video = trialVideo(trial.value);
    const file = selectedFile();
    if (!creating && videosReady && video && file) {
        const block = file.filename.replace(/\.[^.]+$/, '');
        download.href = '/' + video.url;
        download.download = block + '-' + video.name;
        download.removeAttribute('aria-disabled');
    } else {
        download.removeAttribute('href');
        download.removeAttribute('download');
        download.setAttribute('aria-disabled', 'true');
    }
    if (creating || !videosReady) {
        create.disabled = true;
        play.disabled = true;
        if (!creating) play.textContent = 'Play';
        return;
    }
    create.disabled = trial.disabled || trial.value === '' || !!video;
    play.disabled = !video;
    const playing = !!video && currentVideo === video.name && !player.paused && !player.ended;
    play.textContent = playing ? 'Pause' : 'Play';
}

function showMask() {
    document.getElementById('mask').hidden = false;
}

function hideMask() {
    document.getElementById('mask').hidden = true;
}

function clearPlayer() {
    const player = document.getElementById('player');
    loadGen++;
    loading = null;
    loadingName = '';
    pendingFrame = -1;
    player.pause();
    currentVideo = '';
    if (objectUrl) {
        URL.revokeObjectURL(objectUrl);
        objectUrl = '';
    }
    player.removeAttribute('src');
    player.load();
    showMask();
    updatePlayback();
}

function ensureLoaded(video) {
    const player = document.getElementById('player');
    if (currentVideo === video.name && player.getAttribute('src')) return Promise.resolve();
    if (loading && loadingName === video.name) return loading;
    const gen = ++loadGen;
    loadingName = video.name;
    currentVideo = video.name;
    player.pause();
    const job = (async () => {
        const resp = await fetch('/' + video.url + '?v=' + Date.now());
        if (gen !== loadGen) return;
        if (!resp.ok) throw new Error('Could not load video');
        const buf = await resp.arrayBuffer();
        if (gen !== loadGen) return;
        if (objectUrl) URL.revokeObjectURL(objectUrl);
        objectUrl = URL.createObjectURL(new Blob([buf], { type: 'video/mp4' }));
        player.src = objectUrl;
        await new Promise((resolve, reject) => {
            if (gen !== loadGen) {
                resolve();
                return;
            }
            const cleanup = () => {
                player.removeEventListener('loadedmetadata', ok);
                player.removeEventListener('error', bad);
            };
            const ok = () => { cleanup(); resolve(); };
            const bad = () => { cleanup(); reject(new Error('Could not load video')); };
            player.addEventListener('loadedmetadata', ok);
            player.addEventListener('error', bad);
        });
    })();
    loading = job;
    return job;
}

async function showTrial(index, autoplay) {
    const video = trialVideo(index);
    const player = document.getElementById('player');
    if (!video) {
        clearPlayer();
        syncButtons();
        return;
    }
    if (!autoplay) showMask();
    try {
        await ensureLoaded(video);
    } catch (e) {
        if (currentVideo === video.name) setMessage(e.message, true);
        syncButtons();
        return;
    }
    if (currentVideo !== video.name) return;
    if (!autoplay) {
        syncButtons();
        return;
    }
    hideMask();
    if (player.ended) player.currentTime = 0;
    player.play();
    syncButtons();
}

async function loadVideos() {
    const gen = ++videoLoadGen;
    const file = selectedFile();
    videosReady = false;
    videos = [];
    paintTrials();
    if (!file) {
        videosReady = true;
        clearPlayer();
        paintTrials();
        return;
    }
    const raw = await dfEval('trial_video_list {' + file.filename + '}', 30000);
    if (gen !== videoLoadGen) return;
    const data = JSON.parse(raw);
    if (data.status !== 'ok') {
        throw new Error(data.error || 'Could not list videos');
    }
    if (gen !== videoLoadGen) return;
    videos = data.videos || [];
    videosReady = true;
    paintTrials();
    showTrial(document.getElementById('trial').value, false);
}

async function onDatafileChange() {
    clearPlayer();
    videos = [];
    applyFilters();
    try {
        await loadVideos();
        setMessage('');
    } catch (e) {
        setMessage(e.message, true);
    }
    refreshLayers();
}

function onTrialChange() {
    showTrial(document.getElementById('trial').value, false);
    refreshTouches();
}

async function createVideo() {
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    if (!file || trial === '') return;
    creating = true;
    const button = document.getElementById('create');
    syncButtons();
    button.textContent = 'Creating…';
    setMessage('Rendering trial ' + trial + '. This holds stim2 until the trial finishes.');
    try {
        const raw = await dfEval(
            'trial_video_make {' + file.filename + '} ' + trial,
            600000
        );
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Render failed');
        }
        await loadVideos();
        setMessage('Wrote ' + (data.path || ('trial_' + trial + '.mp4')));
    } catch (e) {
        setMessage(e.message, true);
    } finally {
        creating = false;
        button.textContent = 'Create';
        paintTrials();
    }
}

function touchAvailable() {
    const file = selectedFile();
    return !!file && file.filename === layersKey && availableLayers.has('touch');
}

function layerButtonText() {
    return touchOn && touchAvailable() ? 'Touch' : 'None';
}

function setLayersOpen(open) {
    document.getElementById('layers-menu').hidden = !open;
    document.getElementById('layers-button').setAttribute('aria-expanded', open ? 'true' : 'false');
}

function fillLayers() {
    const menu = document.getElementById('layers-menu');
    const button = document.getElementById('layers-button');
    menu.replaceChildren();
    if (touchAvailable()) {
        const label = document.createElement('label');
        const box = document.createElement('input');
        box.type = 'checkbox';
        box.id = 'layer-touch';
        box.checked = touchOn;
        box.addEventListener('change', () => {
            touchOn = box.checked;
            button.textContent = layerButtonText();
            refreshTouches();
        });
        label.append(box, document.createTextNode('Touch'));
        menu.appendChild(label);
    } else {
        const empty = document.createElement('div');
        empty.className = 'tv-layers-empty';
        empty.textContent = 'None';
        menu.appendChild(empty);
    }
    button.textContent = layerButtonText();
}

async function refreshLayers() {
    const gen = ++layersGen;
    const file = selectedFile();
    if (!file) {
        availableLayers = new Set();
        layersKey = '';
        fillLayers();
        refreshTouches();
        return;
    }
    if (file.filename === layersKey) {
        fillLayers();
        refreshTouches();
        return;
    }
    availableLayers = new Set();
    layersKey = '';
    fillLayers();
    try {
        const raw = await dfEval('trial_video_layers {' + file.filename + '}', 30000);
        if (gen !== layersGen) return;
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Could not read layers');
        }
        if (selectedFile() !== file) return;
        availableLayers = new Set(data.layers || []);
        layersKey = file.filename;
        fillLayers();
        refreshTouches();
    } catch (e) {
        if (gen !== layersGen) return;
        availableLayers = new Set();
        layersKey = '';
        fillLayers();
        setMessage(e.message, true);
    }
}

function playheadSeconds() {
    const player = document.getElementById('player');
    const scrub = document.getElementById('scrub');
    if (scrubbing || pendingFrame >= 0) {
        const frame = scrubbing ? Number(scrub.value) : pendingFrame;
        return frame / FRAME_RATE;
    }
    if (!player.getAttribute('src')) return null;
    return player.currentTime || 0;
}

function marksAt(t) {
    const out = [];
    let open = null;
    for (const ev of touches) {
        if (ev.kind === 'press') {
            if (open && t >= open.t && t <= open.t + TOUCH_HOLD) out.push(open);
            open = ev;
        } else if (ev.kind === 'release' && open) {
            const until = ev.t >= open.t ? ev.t : open.t + TOUCH_HOLD;
            if (t >= open.t && t <= until) out.push(open);
            open = null;
        }
    }
    if (open && t >= open.t && t <= open.t + TOUCH_HOLD) out.push(open);
    return out;
}

function pictureBox() {
    const screen = document.querySelector('.tv-screen');
    const player = document.getElementById('player');
    const vw = player.videoWidth || touchSpace.w || 1280;
    const vh = player.videoHeight || touchSpace.h || 800;
    const bw = screen.clientWidth;
    const bh = screen.clientHeight;
    const scale = Math.min(bw / vw, bh / vh);
    const width = vw * scale;
    const height = vh * scale;
    return {
        left: (bw - width) / 2,
        top: (bh - height) / 2,
        width: width,
        height: height
    };
}

function paintTouches() {
    const layer = document.getElementById('touch-layer');
    const mask = document.getElementById('mask');
    if (!touchOn || !touchAvailable() || !mask.hidden) {
        layer.replaceChildren();
        return;
    }
    const t = playheadSeconds();
    const marks = t == null ? [] : marksAt(t);
    layer.replaceChildren();
    if (!marks.length) return;
    const box = pictureBox();
    const spaceW = touchSpace.w || 1280;
    const spaceH = touchSpace.h || 800;
    for (const mark of marks) {
        const el = document.createElement('div');
        el.className = 'tv-touch';
        const size = (56 / spaceW) * box.width;
        el.style.width = size + 'px';
        el.style.height = size + 'px';
        el.style.left = (box.left + (mark.x / spaceW) * box.width) + 'px';
        el.style.top = (box.top + (mark.y / spaceH) * box.height) + 'px';
        el.innerHTML = FINGERPRINT;
        layer.appendChild(el);
    }
}

async function refreshTouches() {
    const gen = ++touchGen;
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    const show = touchOn && touchAvailable() && file && trial !== '';
    if (!show) {
        touches = [];
        touchKey = '';
        paintTouches();
        return;
    }
    const key = file.filename + '#' + trial;
    if (key === touchKey) {
        paintTouches();
        return;
    }
    touches = [];
    paintTouches();
    try {
        const raw = await dfEval('trial_video_touches {' + file.filename + '} ' + trial, 30000);
        if (gen !== touchGen) return;
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Could not read touches');
        }
        touches = data.touches || [];
        touchSpace = {
            w: Number(data.width) || 1280,
            h: Number(data.height) || 800
        };
        touchKey = key;
        paintTouches();
    } catch (e) {
        if (gen !== touchGen) return;
        touches = [];
        touchKey = '';
        setMessage(e.message, true);
    }
}

function spotText(frame, seconds) {
    if (!Number.isFinite(frame) || !Number.isFinite(seconds)) return 'frame —';
    return 'frame ' + frame + '    ' + seconds.toFixed(3) + ' s';
}

function lastFrame() {
    const player = document.getElementById('player');
    if (!player.getAttribute('src')) return -1;
    if (!Number.isFinite(player.duration) || player.duration <= 0) return -1;
    return Math.max(0, Math.round(player.duration * FRAME_RATE) - 1);
}

function updatePlayback() {
    const player = document.getElementById('player');
    const scrub = document.getElementById('scrub');
    const last = lastFrame();
    const ready = last >= 0;
    scrub.disabled = !ready;
    scrub.max = ready ? String(last) : '0';
    document.getElementById('playback-end').textContent = ready
        ? spotText(last, last / FRAME_RATE)
        : 'frame —';
    if (!ready) {
        document.getElementById('playback').textContent = 'frame —';
        scrub.value = '0';
        scrub.style.setProperty('--pct', '0%');
        paintTouches();
        return;
    }
    let frame;
    let seconds;
    if (scrubbing || pendingFrame >= 0) {
        frame = scrubbing ? Number(scrub.value) : pendingFrame;
        seconds = frame / FRAME_RATE;
    } else {
        frame = Math.floor((player.currentTime || 0) * FRAME_RATE + 1e-4);
        if (frame > last) frame = last;
        if (frame < 0) frame = 0;
        seconds = player.currentTime || 0;
        if (frame >= last) seconds = last / FRAME_RATE;
        scrub.value = String(frame);
    }
    const pct = last > 0 ? (frame / last) * 100 : 0;
    scrub.style.setProperty('--pct', pct + '%');
    document.getElementById('playback').textContent = spotText(frame, seconds);
    paintTouches();
}

function seekFrame(frame) {
    const player = document.getElementById('player');
    hideMask();
    pendingFrame = frame;
    const t = frame / FRAME_RATE;
    if (player.readyState >= 1 && Math.abs((player.currentTime || 0) - t) > 0.0005) {
        player.currentTime = t;
    } else {
        pendingFrame = -1;
    }
    updatePlayback();
}

function watchPlayback() {
    const player = document.getElementById('player');
    const tick = () => {
        updatePlayback();
        if (!player.paused && !player.ended) requestAnimationFrame(tick);
    };
    player.addEventListener('play', () => {
        tick();
        syncButtons();
    });
    player.addEventListener('pause', () => {
        updatePlayback();
        syncButtons();
    });
    player.addEventListener('ended', syncButtons);
    player.addEventListener('seeked', () => {
        pendingFrame = -1;
        updatePlayback();
    });
    player.addEventListener('loadedmetadata', updatePlayback);
    player.addEventListener('durationchange', updatePlayback);
    player.addEventListener('emptied', updatePlayback);

    const scrub = document.getElementById('scrub');
    scrub.addEventListener('pointerdown', () => {
        const playing = !player.paused && !player.ended;
        scrubbing = true;
        resumeAfter = playing;
        if (playing) player.pause();
    });
    scrub.addEventListener('input', () => seekFrame(Number(scrub.value)));
    window.addEventListener('pointerup', () => {
        if (!scrubbing) return;
        scrubbing = false;
        updatePlayback();
        if (resumeAfter) {
            resumeAfter = false;
            hideMask();
            player.play();
        }
    });
}

function togglePlay() {
    const trial = document.getElementById('trial').value;
    const video = trialVideo(trial);
    const player = document.getElementById('player');
    if (!video) return;
    if (currentVideo === video.name && !player.paused && !player.ended) {
        player.pause();
        return;
    }
    showTrial(trial, true);
    setMessage('');
}

async function loadFiles() {
    const raw = await dfEval('list_datafiles', 30000);
    const tapedRaw = await dfEval('taped_datafiles', 30000);
    const parsed = JSON.parse(raw);
    const taped = JSON.parse(tapedRaw);
    if (!Array.isArray(parsed)) {
        throw new Error(parsed.error || 'Could not list datafiles');
    }
    if (!Array.isArray(taped)) {
        throw new Error(taped.error || 'Could not list tapes');
    }
    const have = new Set(taped);
    files = parsed.filter(file => have.has(file.filename));
    await onDatafileChange();
    if (files.length === 0) setMessage('no blocks with tape');
}

async function init() {
    PageNav.init('page-nav-container', 'trial_viewer');
    for (const id of ['subject', 'system', 'protocol', 'block']) {
        document.getElementById(id).addEventListener('change', onDatafileChange);
    }
    watchPlayback();
    document.getElementById('trial').addEventListener('change', onTrialChange);
    document.getElementById('create').addEventListener('click', createVideo);
    document.getElementById('play').addEventListener('click', togglePlay);
    document.getElementById('layers-button').addEventListener('click', () => {
        setLayersOpen(document.getElementById('layers-menu').hidden);
    });
    document.addEventListener('click', (ev) => {
        if (!document.querySelector('.tv-layers-wrap').contains(ev.target)) {
            setLayersOpen(false);
        }
    });
    window.addEventListener('resize', paintTouches);

    connection = new DservConnection({
        subprocess: 'df',
        autoReconnect: true,
        connectTimeout: 10000,
        onStatus: (state, text) => setConnected(state, text || state)
    });
    try {
        await connection.connect();
        setConnected('connected', 'Connected');
        await loadFiles();
    } catch (e) {
        setConnected('', 'Disconnected');
        setMessage(e.message, true);
    }
}

init();
