let connection = null;
let files = [];
let currentVideo = '';

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

function selectedFile() {
    const sel = document.getElementById('datafile');
    return files.find(f => f.filename === sel.value) || null;
}

function fillTrials() {
    const file = selectedFile();
    const trial = document.getElementById('trial');
    const create = document.getElementById('create');
    trial.innerHTML = '';
    // The player indexes observations (evt:20). n_trials is the extracted
    // table, which leaves out an observation that did not complete.
    const n = file ? (Number(file.n_obs) || 0) : 0;
    for (let i = 0; i < n; i++) {
        const opt = document.createElement('option');
        opt.value = String(i);
        opt.textContent = 'trial ' + i;
        trial.appendChild(opt);
    }
    const ok = n > 0;
    trial.disabled = !ok;
    create.disabled = !ok;
}

async function loadVideos(selectName) {
    const box = document.getElementById('videos');
    box.innerHTML = '';
    currentVideo = '';
    const file = selectedFile();
    if (!file) return;
    const raw = await dfEval('trial_video_list {' + file.filename + '}', 30000);
    const data = JSON.parse(raw);
    if (data.status !== 'ok') {
        throw new Error(data.error || 'Could not list videos');
    }
    for (const video of data.videos || []) {
        const opt = document.createElement('option');
        opt.value = video.name;
        opt.dataset.url = video.url;
        opt.textContent = video.name;
        if (video.name === selectName) opt.selected = true;
        box.appendChild(opt);
    }
}

async function onDatafileChange() {
    fillTrials();
    try {
        await loadVideos();
        setMessage('');
    } catch (e) {
        setMessage(e.message, true);
    }
}

async function createVideo() {
    const file = selectedFile();
    const trial = document.getElementById('trial').value;
    if (!file || trial === '') return;
    const button = document.getElementById('create');
    button.disabled = true;
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
        const name = 'trial_' + trial + '.mp4';
        await loadVideos(name);
        setMessage('Wrote ' + (data.path || name));
    } catch (e) {
        setMessage(e.message, true);
    } finally {
        button.disabled = document.getElementById('trial').disabled;
        button.textContent = 'Create';
    }
}

function playSelected() {
    const box = document.getElementById('videos');
    const opt = box.selectedOptions[0];
    if (!opt) {
        setMessage('Select a video', true);
        return;
    }
    const player = document.getElementById('player');
    if (currentVideo !== opt.value) {
        currentVideo = opt.value;
        player.src = '/' + opt.dataset.url + '?v=' + Date.now();
    }
    player.play();
    setMessage('');
}

function pauseSelected() {
    document.getElementById('player').pause();
}

async function loadFiles() {
    const raw = await dfEval('list_datafiles', 30000);
    files = JSON.parse(raw);
    const sel = document.getElementById('datafile');
    sel.innerHTML = '';
    for (const file of files) {
        const opt = document.createElement('option');
        opt.value = file.filename;
        opt.textContent = [file.filename, file.subject, file.system, file.protocol]
            .filter(Boolean).join('  ');
        sel.appendChild(opt);
    }
    sel.disabled = files.length === 0;
    await onDatafileChange();
}

async function init() {
    PageNav.init('page-nav-container', 'trial_viewer');
    document.getElementById('datafile').addEventListener('change', onDatafileChange);
    document.getElementById('create').addEventListener('click', createVideo);
    document.getElementById('play').addEventListener('click', playSelected);
    document.getElementById('pause').addEventListener('click', pauseSelected);

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
