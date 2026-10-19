let connection = null;
let files = [];
let videos = [];
let currentVideo = '';
let creating = false;
let confirming = false;
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
let eyeOn = false;
let eyes = [];
let eyeKey = '';
let eyeGen = 0;
let availableLayers = new Set();
let eyeVirtual = false;
let layersKey = '';
let layersGen = 0;
let touchSpace = { w: 1280, h: 800 };
let burning = false;
let backgroundGen = 0;
let backgroundKey = '';
let backgroundMissing = false;
let backgroundModalFor = '';
let modalGen = 0;
let filesBusy = false;
let filesAgain = false;
const FRAME_RATE = 60;
const BACKGROUND_NOTE =
    'Background color not set in this task! Defaulting to black for this video.';
const BACKGROUND_MISSING =
    BACKGROUND_NOTE + ' The task should call setBackground to set a background color.';
const EYE_TRAIL = 15;
const EYE_TRAIL_GAP = 3;
const EYE_TRAIL_FADE = 0.82;
const TOUCH_HOLD = 0.2;
const FINGERPRINT =
    '<svg viewBox="0 0 64 64" aria-hidden="true">' +
    '<circle cx="32" cy="32" r="28" fill="#fff" fill-opacity="0.65"/>' +
    '</svg>';

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

function setTrialOpen(open) {
    document.getElementById('trial-menu').hidden = !open;
    document.getElementById('trial-button').setAttribute('aria-expanded', open ? 'true' : 'false');
}

function paintTrials() {
    const trial = document.getElementById('trial');
    const menu = document.getElementById('trial-menu');
    const button = document.getElementById('trial-button');
    menu.innerHTML = '';
    for (const opt of trial.options) {
        const item = document.createElement('button');
        item.type = 'button';
        item.dataset.value = opt.value;
        item.textContent = trialLabel(opt.value);
        item.setAttribute('role', 'option');
        const selected = opt.value === trial.value;
        item.setAttribute('aria-selected', selected ? 'true' : 'false');
        if (selected) item.classList.add('is-selected');
        item.addEventListener('click', () => {
            setTrialOpen(false);
            if (trial.value === opt.value) return;
            trial.value = opt.value;
            paintTrials();
            trial.dispatchEvent(new Event('change'));
        });
        menu.appendChild(item);
    }
    button.textContent = trial.value === '' ? 'trial' : trialLabel(trial.value);
    button.disabled = trial.disabled;
    syncButtons();
}

function syncButtons() {
    const action = document.getElementById('action');
    const download = document.getElementById('download');
    const player = document.getElementById('player');
    const trial = document.getElementById('trial');
    const video = trialVideo(trial.value);
    const file = selectedFile();
    const layered = selectedLayers().length > 0;
    download.textContent = burning ? 'Preparing…' : 'Download';
    if (burning || creating || !videosReady || !video || !file) {
        download.removeAttribute('href');
        download.removeAttribute('download');
        download.setAttribute('aria-disabled', 'true');
    } else if (layered) {
        download.removeAttribute('href');
        download.removeAttribute('download');
        download.removeAttribute('aria-disabled');
    } else {
        const block = file.filename.replace(/\.[^.]+$/, '');
        download.href = '/' + video.url;
        download.download = block + '-' + video.name;
        download.removeAttribute('aria-disabled');
    }
    if (confirming) {
        action.disabled = true;
        return;
    }
    if (creating) {
        action.disabled = true;
        action.textContent = 'Creating…';
        action.dataset.mode = 'create';
        return;
    }
    if (!videosReady) {
        action.disabled = true;
        action.textContent = 'Create';
        action.dataset.mode = 'create';
        return;
    }
    const playing = !!video && currentVideo === video.name && !player.paused && !player.ended;
    if (playing) {
        action.disabled = false;
        action.textContent = 'Pause';
        action.dataset.mode = 'pause';
    } else if (video) {
        action.disabled = false;
        action.textContent = 'Play';
        action.dataset.mode = 'play';
    } else {
        action.disabled = trial.disabled || trial.value === '';
        action.textContent = 'Create';
        action.dataset.mode = 'create';
    }
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

function showModal(text, opts) {
    const gen = ++modalGen;
    const root = document.getElementById('modal');
    const card = root.querySelector('.tv-modal-card');
    const cancel = document.getElementById('modal-cancel');
    const ok = document.getElementById('modal-ok');
    document.getElementById('modal-text').textContent = text;
    card.classList.toggle('warn', !!opts.warn);
    cancel.hidden = !opts.cancel;
    cancel.textContent = opts.cancelLabel || 'Cancel';
    ok.textContent = opts.okLabel || 'OK';
    root.hidden = false;
    ok.focus();
    return new Promise((resolve) => {
        const finish = (yes) => {
            ok.removeEventListener('click', onOk);
            cancel.removeEventListener('click', onCancel);
            if (gen !== modalGen) return;
            root.hidden = true;
            resolve(yes);
        };
        const onOk = () => finish(true);
        const onCancel = () => finish(false);
        ok.addEventListener('click', onOk);
        cancel.addEventListener('click', onCancel);
    });
}

async function ensureBackground(file, trial, popup) {
    const gen = ++backgroundGen;
    const key = file.filename + '#' + trial;
    if (key !== backgroundKey) {
        const raw = await dfEval(
            'trial_video_background {' + file.filename + '} ' + trial,
            30000
        );
        if (gen !== backgroundGen) return;
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Could not read background');
        }
        backgroundMissing = Number(data.missing) === 1;
        backgroundKey = key;
    }
    if (!backgroundMissing) {
        backgroundModalFor = '';
        return;
    }
    if (!popup || backgroundModalFor === key) return;
    backgroundModalFor = key;
    await showModal(BACKGROUND_MISSING, { cancel: false, okLabel: 'OK', warn: true });
}

function applyBackgroundNote() {
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    const el = document.getElementById('message');
    const key = file ? file.filename + '#' + trial : '';
    if (file && trialVideo(trial) && backgroundKey === key && backgroundMissing) {
        if (!el.classList.contains('error')) setMessage(BACKGROUND_NOTE);
        return;
    }
    if (el.textContent === BACKGROUND_NOTE) setMessage('');
}

async function noteBackground(index) {
    const file = selectedFile();
    if (!file || index === '' || index === undefined || !trialVideo(index)) {
        applyBackgroundNote();
        return;
    }
    try {
        await ensureBackground(file, index, false);
    } catch (e) {
        if (document.getElementById('trial').value === String(index)) {
            setMessage(e.message, true);
        }
        return;
    }
    if (document.getElementById('trial').value !== String(index)) return;
    applyBackgroundNote();
}

async function showTrial(index, autoplay) {
    const video = trialVideo(index);
    const player = document.getElementById('player');
    if (!video) {
        clearPlayer();
        syncButtons();
        applyBackgroundNote();
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
    await noteBackground(index);
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
    await showTrial(document.getElementById('trial').value, false);
}

async function onDatafileChange() {
    clearPlayer();
    videos = [];
    applyFilters();
    try {
        await loadVideos();
        const el = document.getElementById('message');
        if (!el.classList.contains('error')) {
            setMessage('');
            applyBackgroundNote();
        }
    } catch (e) {
        setMessage(e.message, true);
    }
    refreshLayers();
}

function onTrialChange() {
    showTrial(document.getElementById('trial').value, false);
    refreshTouches();
    refreshEyes();
}

async function askToStopTask() {
    confirming = true;
    syncButtons();
    const stop = await showModal(
        'A task is running. Creating this video takes over the stim display until the capture finishes, so the task has to stop.',
        { cancel: true, okLabel: 'Agree', cancelLabel: 'Cancel' }
    );
    confirming = false;
    syncButtons();
    return stop;
}

async function essValue(name) {
    const raw = await connection.evalAsync(
        'if {[catch {dservGet ' + name + '} _tv]} { set _tv "" }; string trim $_tv'
    );
    return String(raw || '').trim();
}

async function reloadSelectedTask(onReload) {
    const system = await essValue('ess/system');
    const protocol = await essValue('ess/protocol');
    const variant = await essValue('ess/variant');
    if (!system || !protocol) return false;
    if (onReload) onReload();
    const cmd = variant ? 'ess::reload_system' : 'ess::reload_protocol';
    await connection.evalAsync('send ess {' + cmd + '}', 600000);
    return true;
}

async function createVideo() {
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    if (!file || trial === '') return;
    try {
        await ensureBackground(file, trial, true);
    } catch (e) {
        setMessage(e.message, true);
        return;
    }
    let status = '';
    try {
        status = await essValue('ess/status');
    } catch (e) {
        setMessage(e.message, true);
        return;
    }
    if (status === 'loading') {
        setMessage('The system is still loading.', true);
        return;
    }
    if (status === 'running') {
        const stop = await askToStopTask();
        if (!stop) return;
    }
    creating = true;
    syncButtons();
    setMessage('Rendering trial ' + trial + '. This holds stim2 until the trial finishes.');
    try {
        if (status === 'running') {
            await connection.evalAsync('send ess {ess::stop}', 60000);
        }
        const raw = await dfEval(
            'trial_video_make {' + file.filename + '} ' + trial,
            600000
        );
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Render failed');
        }
        await loadVideos();
        backgroundMissing = Number(data.background_missing) === 1;
        backgroundKey = file.filename + '#' + trial;
        const wrote = 'Wrote ' + (data.path || ('trial_' + trial + '.mp4'));
        try {
            const restored = await reloadSelectedTask(function () {
                setMessage('Restoring the selected task.');
            });
            setMessage(restored ? wrote + '. Reloaded the selected task.' : wrote);
        } catch (e) {
            setMessage(wrote + '. The selected task was not restored. ' + e.message, true);
        }
    } catch (e) {
        setMessage(e.message, true);
    } finally {
        creating = false;
        paintTrials();
    }
}

function touchAvailable() {
    const file = selectedFile();
    return !!file && file.filename === layersKey && availableLayers.has('touch');
}

function eyeAvailable() {
    const file = selectedFile();
    return !!file && file.filename === layersKey && availableLayers.has('eye');
}

function selectedLayers() {
    const names = [];
    if (touchOn && touchAvailable()) names.push('touch');
    if (eyeOn && eyeAvailable()) names.push('eye');
    return names;
}

function eyeLabel() {
    return eyeVirtual ? 'Virtual eye' : 'Eye';
}

function layerButtonText() {
    const names = [];
    if (touchOn && touchAvailable()) names.push('Touch');
    if (eyeOn && eyeAvailable()) names.push(eyeLabel());
    return names.length ? names.join(', ') : 'None';
}

function setLayersOpen(open) {
    document.getElementById('layers-menu').hidden = !open;
    document.getElementById('layers-button').setAttribute('aria-expanded', open ? 'true' : 'false');
}

function addLayerBox(menu, button, id, labelText, on, setOn, refresh) {
    const label = document.createElement('label');
    const box = document.createElement('input');
    box.type = 'checkbox';
    box.id = id;
    box.checked = on;
    box.addEventListener('change', () => {
        setOn(box.checked);
        button.textContent = layerButtonText();
        refresh();
        syncButtons();
    });
    label.append(box, document.createTextNode(labelText));
    menu.appendChild(label);
}

function fillLayers() {
    const menu = document.getElementById('layers-menu');
    const button = document.getElementById('layers-button');
    menu.replaceChildren();
    if (touchAvailable()) {
        addLayerBox(menu, button, 'layer-touch', 'Touch', touchOn, (v) => { touchOn = v; }, refreshTouches);
    }
    if (eyeAvailable()) {
        addLayerBox(menu, button, 'layer-eye', eyeLabel(), eyeOn, (v) => { eyeOn = v; }, refreshEyes);
    }
    if (!touchAvailable() && !eyeAvailable()) {
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
        eyeVirtual = false;
        layersKey = '';
        fillLayers();
        refreshTouches();
        refreshEyes();
        return;
    }
    if (file.filename === layersKey) {
        fillLayers();
        refreshTouches();
        refreshEyes();
        return;
    }
    availableLayers = new Set();
    eyeVirtual = false;
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
        eyeVirtual = !!data.eye_virtual;
        layersKey = file.filename;
        fillLayers();
        refreshTouches();
        refreshEyes();
    } catch (e) {
        if (gen !== layersGen) return;
        availableLayers = new Set();
        eyeVirtual = false;
        layersKey = '';
        fillLayers();
        refreshTouches();
        refreshEyes();
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

function eyeAt(t) {
    if (!eyes.length || t == null) return null;
    let lo = 0;
    let hi = eyes.length - 1;
    let found = -1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (eyes[mid].t <= t + 1e-6) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found < 0 ? null : eyes[found];
}

function eyeTrail(t) {
    const picked = [];
    const seen = new Set();
    for (let i = 0; i < EYE_TRAIL; i++) {
        const sample = eyeAt(t - (i * EYE_TRAIL_GAP) / FRAME_RATE);
        if (!sample || seen.has(sample.t)) continue;
        seen.add(sample.t);
        picked.push({
            sample: sample,
            opacity: 0.5 * Math.pow(EYE_TRAIL_FADE, i)
        });
    }
    picked.reverse();
    return picked;
}

function paintMarks() {
    const layer = document.getElementById('touch-layer');
    const mask = document.getElementById('mask');
    layer.replaceChildren();
    if (!mask.hidden) return;
    const t = playheadSeconds();
    const box = pictureBox();
    const spaceW = touchSpace.w || 1280;
    const spaceH = touchSpace.h || 800;
    if (touchOn && touchAvailable() && t != null) {
        for (const mark of marksAt(t)) {
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
    if (eyeOn && eyeAvailable()) {
        const size = (8 / spaceW) * box.width;
        for (const dot of eyeTrail(t)) {
            const el = document.createElement('div');
            el.className = 'tv-eye';
            el.style.opacity = String(dot.opacity);
            el.style.width = size + 'px';
            el.style.height = size + 'px';
            el.style.left = (box.left + (dot.sample.x / spaceW) * box.width) + 'px';
            el.style.top = (box.top + (dot.sample.y / spaceH) * box.height) + 'px';
            layer.appendChild(el);
        }
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
        paintMarks();
        return;
    }
    const key = file.filename + '#' + trial;
    if (key === touchKey) {
        paintMarks();
        return;
    }
    touches = [];
    paintMarks();
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
        paintMarks();
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
        paintMarks();
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
    paintMarks();
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

function onAction() {
    if (creating || confirming) return;
    const trial = document.getElementById('trial').value;
    const video = trialVideo(trial);
    const player = document.getElementById('player');
    if (!video) {
        createVideo();
        return;
    }
    if (currentVideo === video.name && !player.paused && !player.ended) {
        player.pause();
        return;
    }
    showTrial(trial, true);
    if (document.getElementById('message').textContent !== BACKGROUND_NOTE) {
        setMessage('');
    }
}

async function refreshEyes() {
    const gen = ++eyeGen;
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    const show = eyeOn && eyeAvailable() && file && trial !== '';
    if (!show) {
        eyes = [];
        eyeKey = '';
        paintMarks();
        return;
    }
    const key = file.filename + '#' + trial;
    if (key === eyeKey) {
        paintMarks();
        return;
    }
    eyes = [];
    paintMarks();
    try {
        const raw = await dfEval('trial_video_eyes {' + file.filename + '} ' + trial, 30000);
        if (gen !== eyeGen) return;
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Could not read eye position');
        }
        eyes = data.eyes || [];
        touchSpace = {
            w: Number(data.width) || 1280,
            h: Number(data.height) || 800
        };
        eyeKey = key;
        paintMarks();
    } catch (e) {
        if (gen !== eyeGen) return;
        eyes = [];
        eyeKey = '';
        setMessage(e.message, true);
    }
}

function layerStatusText() {
    const names = [];
    if (touchOn && touchAvailable()) names.push('Touch');
    if (eyeOn && eyeAvailable()) names.push(eyeLabel());
    if (names.length === 1) return 'Adding ' + names[0] + ' to the video.';
    return 'Adding ' + names.join(' and ') + ' to the video.';
}

async function onDownload(ev) {
    const layers = selectedLayers();
    if (!layers.length) return;
    ev.preventDefault();
    if (burning) return;
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    if (!file || trial === '' || !trialVideo(trial)) return;
    burning = true;
    syncButtons();
    setMessage(layerStatusText());
    try {
        const raw = await dfEval(
            'trial_video_burn {' + file.filename + '} ' + trial + ' ' + layers.join(' '),
            600000
        );
        const data = JSON.parse(raw);
        if (data.status !== 'ok') {
            throw new Error(data.error || 'Could not add layers');
        }
        const block = file.filename.replace(/\.[^.]+$/, '');
        const base = String(data.url || '').split('/').pop();
        const a = document.createElement('a');
        a.href = '/' + data.url;
        a.download = block + '-' + base;
        document.body.appendChild(a);
        a.click();
        a.remove();
        setMessage('');
        applyBackgroundNote();
    } catch (e) {
        setMessage(e.message, true);
    } finally {
        burning = false;
        syncButtons();
    }
}

async function refreshFileList() {
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
    const block = document.getElementById('block');
    const previous = block.value;
    files = parsed.filter(file => have.has(file.filename));
    applyFilters();
    if (block.value !== previous) {
        await onDatafileChange();
    }
    if (files.length === 0) setMessage('no blocks with tape');
}

async function loadFiles() {
    if (filesBusy) {
        filesAgain = true;
        return;
    }
    filesBusy = true;
    try {
        do {
            filesAgain = false;
            try {
                await refreshFileList();
            } catch (e) {
                setMessage(e.message, true);
            }
        } while (filesAgain);
    } finally {
        filesBusy = false;
    }
}

async function init() {
    PageNav.init('page-nav-container', 'trial_viewer');
    for (const id of ['subject', 'system', 'protocol', 'block']) {
        document.getElementById(id).addEventListener('change', onDatafileChange);
    }
    watchPlayback();
    document.getElementById('trial').addEventListener('change', onTrialChange);
    document.getElementById('action').addEventListener('click', onAction);
    document.getElementById('download').addEventListener('click', onDownload);
    document.getElementById('layers-button').addEventListener('click', () => {
        setTrialOpen(false);
        setLayersOpen(document.getElementById('layers-menu').hidden);
    });
    document.getElementById('trial-button').addEventListener('click', () => {
        if (document.getElementById('trial-button').disabled) return;
        setLayersOpen(false);
        setTrialOpen(document.getElementById('trial-menu').hidden);
    });
    document.addEventListener('click', (ev) => {
        if (!document.querySelector('.tv-layers-wrap').contains(ev.target)) {
            setLayersOpen(false);
        }
        if (!document.querySelector('.tv-trial-wrap').contains(ev.target)) {
            setTrialOpen(false);
        }
    });
    window.addEventListener('resize', paintMarks);

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
        const datapoints = new DatapointManager(connection, { autoGetKeys: false });
        datapoints.subscribe('df/file_closed', () => { loadFiles(); });
    } catch (e) {
        setConnected('', 'Disconnected');
        setMessage(e.message, true);
    }
}

init();
