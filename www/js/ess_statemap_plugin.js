/**
 * ESS State Map Plugin
 *
 * Draws the loaded system's state machine as a layered graph and keeps
 * it live: the current state lights up as a session runs, and clicking
 * a state shows its action/transition source. This is a READING
 * surface — nothing here edits anything.
 *
 * Data sources (all published by ess-2.0.tm):
 *   ess/state_table   — Tcl dict: state -> {next states}, refreshed on
 *                       every system load (also in snapshot.states)
 *   ess/action_state  — "<state>_a" written on every state entry
 *   ess/status        — stopped / running for the live chip
 *
 * The table is extracted by get_state_transitions, which regexp-matches
 * literal `return <state>` in each <state>_t body. A transition that
 * computes its target (`return $next`) is invisible to it, so a state
 * with no detected out-edges is labeled "no detected transitions" —
 * possibly terminal, possibly dynamic — rather than presented as fact.
 */

const StateMapPlugin = {

    // ==========================================
    // Lifecycle
    // ==========================================

    onInit(wb) {
        this._wb = wb;
        this._tableStr = '';        // last-rendered state table string
        this._nodes = new Map();    // name -> layout node
        this._edges = [];
        this._start = null;
        this._current = null;       // live state name
        this._prev = null;
        this._selected = null;
        this._trail = [];           // [{state, t}]
        this._sourceCache = new Map();  // "sys/proto/state" -> text

        this._els = {
            svg: document.getElementById('statemap-svg'),
            empty: document.getElementById('statemap-empty'),
            canvas: document.getElementById('statemap-canvas'),
            hero: document.getElementById('statemap-hero'),
            chips: document.getElementById('statemap-chips'),
            live: document.getElementById('statemap-live'),
            detail: document.getElementById('statemap-detail'),
            trail: document.getElementById('statemap-trail')
        };

        // Node selection (delegated on the svg)
        this._els.svg?.addEventListener('click', (e) => {
            const g = e.target.closest('.sm-node');
            if (g) this._selectState(wb, g.dataset.state);
        });

        // Chips can name states (unreachable list) — make them jump
        this._els.chips?.addEventListener('click', (e) => {
            const s = e.target.closest('[data-state]');
            if (s) this._selectState(wb, s.dataset.state);
        });

        this._initSideResize();
    },

    // Side panel is resizable by dragging the divider; width persists.
    _initSideResize() {
        const divider = document.getElementById('statemap-divider');
        const side = document.getElementById('statemap-side');
        if (!divider || !side) return;

        const saved = parseInt(localStorage.getItem('ess_statemap_side_w'), 10);
        if (saved >= 240 && saved <= 700) side.style.width = `${saved}px`;

        divider.addEventListener('pointerdown', (e) => {
            e.preventDefault();
            divider.setPointerCapture(e.pointerId);
            divider.classList.add('dragging');
            const onMove = (ev) => {
                const w = Math.min(700, Math.max(240,
                    Math.round(side.getBoundingClientRect().right - ev.clientX)));
                side.style.width = `${w}px`;
            };
            const onUp = (ev) => {
                divider.classList.remove('dragging');
                divider.removeEventListener('pointermove', onMove);
                divider.removeEventListener('pointerup', onUp);
                localStorage.setItem('ess_statemap_side_w',
                    String(Math.round(side.getBoundingClientRect().width)));
            };
            divider.addEventListener('pointermove', onMove);
            divider.addEventListener('pointerup', onUp);
        });
    },

    onConnected(wb) {
        wb.dpManager.subscribe('ess/state_table', (dp) => {
            this._maybeRender(this._dpValue(dp));
        });
        wb.dpManager.subscribe('ess/action_state', (dp) => {
            const v = String(this._dpValue(dp) ?? '');
            this._onActionState(v.replace(/_a$/, ''));
        });
        wb.dpManager.subscribe('ess/status', (dp) => {
            const prev = this._status;
            this._status = String(this._dpValue(dp) ?? '');
            // A halted machine isn't "in" a state: clear the highlight on
            // quit/stop (the trail keeps where it stopped). Each new run
            // starts a fresh trail.
            if (this._status === 'running' && prev !== 'running') {
                this._trail = [];
                this._clearCurrent();
                this._renderTrail();
            } else if (this._status !== 'running' && prev === 'running') {
                this._clearCurrent();
            }
            this._renderLiveChip();
        });

        // Subscriptions fire on change only — prime with a touch
        for (const name of ['ess/state_table', 'ess/status']) {
            try {
                wb.connection.ws.send(JSON.stringify({ cmd: 'touch', name }));
            } catch (e) { /* not connected yet; snapshot covers us */ }
        }
    },

    onSnapshot(wb, snapshot) {
        if (this._els.hero) {
            const sys = snapshot.system || '—';
            const proto = snapshot.protocol || '—';
            this._sysKey = `${sys}/${proto}`;
            this._els.hero.textContent = `${sys} / ${proto}`;
        }
        if (typeof snapshot.states === 'string') {
            this._maybeRender(snapshot.states);
        }
    },

    _dpValue(dp) {
        if (dp && typeof dp === 'object') return dp.data ?? dp.value ?? '';
        return dp;
    },

    // ==========================================
    // Graph model
    // ==========================================

    _parseTable(str) {
        const dict = TclParser.parseDict(str || '');
        const out = new Map();          // state -> [next...]
        const all = new Set(Object.keys(dict));
        for (const [state, nexts] of Object.entries(dict)) {
            const list = TclParser.parseList(nexts || '');
            out.set(state, [...new Set(list)]);
            list.forEach(n => all.add(n));
        }
        // Targets that never declared transitions still exist as states
        for (const s of all) if (!out.has(s)) out.set(s, []);
        return out;
    },

    _buildModel(table) {
        const nodes = new Map();
        for (const name of table.keys()) {
            nodes.set(name, { name, out: table.get(name), in: [] });
        }
        const edges = [];
        for (const [from, tos] of table) {
            for (const to of tos) {
                if (!nodes.has(to)) continue;
                edges.push({ from, to });
                nodes.get(to).in.push(from);
            }
        }

        // Start state: ESS convention is a literal "start"; fall back to
        // a node nothing points at, then to the first declared state.
        let start = null;
        if (nodes.has('start')) start = 'start';
        else {
            for (const n of nodes.values()) {
                if (n.in.length === 0) { start = n.name; break; }
            }
            if (!start) start = nodes.keys().next().value;
        }

        // Reachability from start
        const seen = new Set();
        const stack = [start];
        while (stack.length) {
            const s = stack.pop();
            if (!s || seen.has(s)) continue;
            seen.add(s);
            for (const t of (nodes.get(s)?.out || [])) stack.push(t);
        }
        for (const n of nodes.values()) {
            n.unreachable = !seen.has(n.name);
            n.terminal = n.out.length === 0 && n.name !== 'end';
        }
        return { nodes, edges, start };
    },

    // ==========================================
    // Layered layout (top-down flow)
    // ==========================================

    _layout(model) {
        const { nodes, edges, start } = model;

        // BFS layering from start (shortest-path depth)
        let maxLayer = 0;
        for (const n of nodes.values()) n.layer = -1;
        const q = [start];
        nodes.get(start).layer = 0;
        while (q.length) {
            const u = q.shift();
            const lu = nodes.get(u).layer;
            for (const v of nodes.get(u).out) {
                const nv = nodes.get(v);
                if (nv && nv.layer < 0) {
                    nv.layer = lu + 1;
                    maxLayer = Math.max(maxLayer, nv.layer);
                    q.push(v);
                }
            }
        }
        // Unreachable states park in a flagged bottom row
        for (const n of nodes.values()) {
            if (n.layer < 0) n.layer = maxLayer + 1;
        }
        const nLayers = Math.max(...[...nodes.values()].map(n => n.layer)) + 1;

        // Group by layer, initial order = insertion
        const layers = Array.from({ length: nLayers }, () => []);
        for (const n of nodes.values()) layers[n.layer].push(n);
        layers.forEach(l => l.forEach((n, i) => { n.order = i; }));

        // Barycenter ordering, a few alternating sweeps
        const sweep = (getNeighbors) => {
            for (const layer of layers) {
                for (const n of layer) {
                    const ns = getNeighbors(n).map(m => nodes.get(m)?.order)
                        .filter(o => o !== undefined);
                    n._bc = ns.length
                        ? ns.reduce((a, b) => a + b, 0) / ns.length
                        : n.order;
                }
                layer.sort((a, b) => a._bc - b._bc);
                layer.forEach((n, i) => { n.order = i; });
            }
        };
        for (let i = 0; i < 4; i++) {
            sweep(n => n.in);
            sweep(n => n.out);
        }

        // Sizes and coordinates
        const ROW = 74, GAP = 26, MX = 40, MY = 30;
        for (const n of nodes.values()) {
            n.w = Math.max(64, Math.round(n.name.length * 7.4) + 22);
            n.h = 28;
        }
        let globalW = 0;
        const layerW = layers.map(l =>
            l.reduce((w, n) => w + n.w, 0) + GAP * Math.max(0, l.length - 1));
        globalW = Math.max(...layerW, 0);

        layers.forEach((l, li) => {
            let x = MX + (globalW - layerW[li]) / 2;
            for (const n of l) {
                n.x = x + n.w / 2;          // center x
                n.y = MY + li * ROW + n.h / 2;
                x += n.w + GAP;
            }
        });

        const width = globalW + 2 * MX;
        const height = MY * 2 + (nLayers - 1) * ROW + 28;
        return { width, height };
    },

    // ==========================================
    // Rendering
    // ==========================================

    _maybeRender(tableStr) {
        if (typeof tableStr !== 'string') return;
        if (tableStr === this._tableStr) return;
        this._tableStr = tableStr;
        this._selected = null;
        this._trail = [];
        // A new system's graph starts with no live state; carrying the
        // old highlight over would wrongly light a same-named state.
        this._current = null;
        this._prev = null;
        this._render();
    },

    _render() {
        const svg = this._els.svg;
        if (!svg) return;

        const table = this._parseTable(this._tableStr);
        if (table.size === 0) {
            svg.innerHTML = '';
            if (this._els.empty) this._els.empty.hidden = false;
            if (this._els.chips) this._els.chips.innerHTML = '';
            return;
        }
        if (this._els.empty) this._els.empty.hidden = true;

        const model = this._buildModel(table);
        const { width, height } = this._layout(model);
        this._nodes = model.nodes;
        this._edges = model.edges;
        this._start = model.start;

        const esc = (s) => s.replace(/[&<>"]/g,
            c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

        // Edge paths; back-edges route around the side of the graph
        let backIdx = 0;
        const edgeSvg = this._edges.map(e => {
            const u = this._nodes.get(e.from), v = this._nodes.get(e.to);
            let d, cls = 'sm-edge';
            if (e.from === e.to) {
                const x = u.x + u.w / 2, y = u.y;
                d = `M ${x} ${y - 6} C ${x + 26} ${y - 14}, ${x + 26} ${y + 14}, ${x} ${y + 6}`;
                cls += ' sm-edge-self';
            } else if (v.layer > u.layer) {
                const y1 = u.y + u.h / 2, y2 = v.y - v.h / 2;
                const my = (y1 + y2) / 2;
                d = `M ${u.x} ${y1} C ${u.x} ${my}, ${v.x} ${my}, ${v.x} ${y2}`;
            } else if (v.layer === u.layer) {
                const dir = v.x > u.x ? 1 : -1;
                const x1 = u.x + dir * u.w / 2, x2 = v.x - dir * v.w / 2;
                d = `M ${x1} ${u.y} C ${x1 + dir * 18} ${u.y - 22}, ${x2 - dir * 18} ${v.y - 22}, ${x2} ${v.y}`;
                cls += ' sm-edge-lateral';
            } else {
                // Loop back up: swing wide of the columns
                const side = (u.x + v.x) / 2 < width / 2 ? -1 : 1;
                const xr = side < 0 ? 12 : width - 12;
                backIdx += 1;
                const off = side * Math.min(26, 8 * backIdx);
                const x1 = u.x + side * u.w / 2, x2 = v.x + side * v.w / 2;
                d = `M ${x1} ${u.y} C ${xr + off} ${u.y}, ${xr + off} ${v.y}, ${x2} ${v.y}`;
                cls += ' sm-edge-back';
            }
            return `<path class="${cls}" data-from="${esc(e.from)}" data-to="${esc(e.to)}"
                        d="${d}" marker-end="url(#sm-arrow)"><title>${esc(e.from)} → ${esc(e.to)}</title></path>`;
        }).join('');

        const nodeSvg = [...this._nodes.values()].map(n => {
            let cls = 'sm-node';
            if (n.name === this._start) cls += ' sm-node-start';
            if (n.name === 'end') cls += ' sm-node-end';
            if (n.unreachable) cls += ' sm-node-unreachable';
            else if (n.terminal) cls += ' sm-node-terminal';
            const tip = n.unreachable
                ? 'Unreachable from start (no literal transition path leads here)'
                : (n.terminal
                    ? 'No detected transitions — terminal, or a dynamic (computed) transition'
                    : n.name);
            return `<g class="${cls}" data-state="${esc(n.name)}">
                <rect x="${n.x - n.w / 2}" y="${n.y - n.h / 2}" width="${n.w}" height="${n.h}" rx="7"/>
                <text x="${n.x}" y="${n.y}">${esc(n.name)}</text>
                <title>${esc(tip)}</title>
            </g>`;
        }).join('');

        svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
        svg.style.maxWidth = `${Math.round(width * 1.15)}px`;
        svg.innerHTML = `
            <defs>
                <marker id="sm-arrow" markerWidth="9" markerHeight="7" refX="8"
                        refY="3.5" orient="auto" markerUnits="userSpaceOnUse">
                    <polygon points="0 0, 9 3.5, 0 7" class="sm-arrowhead"/>
                </marker>
            </defs>
            <g class="sm-edges">${edgeSvg}</g>
            <g class="sm-nodes">${nodeSvg}</g>`;

        this._renderChips();
        this._renderLiveChip();
        this._renderTrail();
        this._renderDetailHint();
    },

    _renderChips() {
        const el = this._els.chips;
        if (!el) return;
        const nodes = [...this._nodes.values()];
        const unreachable = nodes.filter(n => n.unreachable).map(n => n.name);
        const terminal = nodes.filter(n => n.terminal && !n.unreachable).map(n => n.name);
        const chips = [];
        chips.push(`<span class="sm-chip">${nodes.length} states · ${this._edges.length} transitions</span>`);
        if (unreachable.length) {
            chips.push(`<span class="sm-chip sm-chip-warn">unreachable: ${unreachable
                .map(s => `<a data-state="${s}">${s}</a>`).join(', ')}</span>`);
        } else {
            chips.push(`<span class="sm-chip sm-chip-ok">all reachable</span>`);
        }
        if (terminal.length) {
            chips.push(`<span class="sm-chip sm-chip-hint"
                title="Terminal, or a computed transition the extractor cannot see">no detected transitions: ${terminal
                .map(s => `<a data-state="${s}">${s}</a>`).join(', ')}</span>`);
        }
        el.innerHTML = chips.join('');
    },

    _renderLiveChip() {
        const el = this._els.live;
        if (!el) return;
        const running = this._status === 'running';
        el.innerHTML = `<span class="sm-live ${running ? 'sm-live-on' : ''}">
            ${running ? '● running' : '○ ' + (this._status || 'idle')}</span>
            ${this._current ? `<span class="sm-live-state">${this._current}</span>` : ''}`;
    },

    // ==========================================
    // Live state tracking
    // ==========================================

    _onActionState(state) {
        if (!state || !this._nodes.size) return;
        const prev = this._current;
        this._current = state;
        this._trail.push({ state, t: Date.now() });
        if (this._trail.length > 12) this._trail.shift();
        this._applyCurrent(state, prev);
        this._renderLiveChip();
        this._renderTrail();
    },

    _clearCurrent() {
        this._current = null;
        this._prev = null;
        const svg = this._els.svg;
        svg?.querySelectorAll('.sm-node-current').forEach(n =>
            n.classList.remove('sm-node-current'));
        svg?.querySelectorAll('.sm-edge-recent').forEach(e =>
            e.classList.remove('sm-edge-recent'));
    },

    _applyCurrent(state, prev) {
        const svg = this._els.svg;
        if (!svg) return;
        svg.querySelectorAll('.sm-node-current').forEach(n =>
            n.classList.remove('sm-node-current'));
        const g = svg.querySelector(`.sm-node[data-state="${CSS.escape(state)}"]`);
        if (g) g.classList.add('sm-node-current');
        if (prev && prev !== state) {
            const e = svg.querySelector(
                `.sm-edge[data-from="${CSS.escape(prev)}"][data-to="${CSS.escape(state)}"]`);
            if (e) {
                e.classList.remove('sm-edge-recent');
                void e.getBoundingClientRect();   // restart the CSS animation
                e.classList.add('sm-edge-recent');
            }
        }
    },

    _renderTrail() {
        const el = this._els.trail;
        if (!el) return;
        if (!this._trail.length) { el.innerHTML = ''; return; }
        const rows = [...this._trail].reverse().map((e, i, arr) => {
            const dt = i < arr.length - 1 ? `+${e.t - arr[i + 1].t} ms` : '';
            return `<div class="sm-trail-row"><span>${e.state}</span><span class="sm-trail-dt">${dt}</span></div>`;
        }).join('');
        el.innerHTML = `<div class="sm-side-title">Recent states</div>${rows}`;
    },

    // ==========================================
    // State source detail
    // ==========================================

    _renderDetailHint() {
        if (this._els.detail) {
            this._els.detail.innerHTML =
                '<div class="statemap-detail-hint">Click a state to view its action &amp; transition source.</div>';
        }
    },

    async _selectState(wb, name) {
        if (!name || !/^\w+$/.test(name)) return;
        this._selected = name;
        const svg = this._els.svg;
        svg?.querySelectorAll('.sm-node-selected').forEach(n =>
            n.classList.remove('sm-node-selected'));
        svg?.querySelector(`.sm-node[data-state="${CSS.escape(name)}"]`)
            ?.classList.add('sm-node-selected');

        const el = this._els.detail;
        if (!el) return;
        const key = `${this._sysKey || ''}:${name}`;
        if (!this._sourceCache.has(key)) {
            el.innerHTML = `<div class="sm-side-title">${name}</div><div class="statemap-detail-hint">loading…</div>`;
            try {
                const script =
                    `set __s $::ess::current(state_system)\n` +
                    `set __o {}\n` +
                    `foreach __m {${name}_a ${name}_t} {\n` +
                    `    if {[catch {info object definition $__s $__m} __d] == 0} {\n` +
                    `        append __o "── $__m ──\\n[string trim [lindex $__d 1]]\\n\\n"\n` +
                    `    }\n` +
                    `}\n` +
                    `set __o`;
                const resp = await wb.connection.send(script, 'ess');
                this._sourceCache.set(key, resp || '(no action or transition method)');
            } catch (err) {
                el.innerHTML = `<div class="sm-side-title">${name}</div>
                    <div class="statemap-detail-hint">source unavailable: ${err.message}</div>`;
                return;
            }
        }
        const esc = (s) => s.replace(/[&<>]/g,
            c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
        el.innerHTML = `<div class="sm-side-title">${name}</div>
            <pre class="sm-source">${esc(this._sourceCache.get(key))}</pre>`;
    }
};

ESSWorkbench.registerPlugin(StateMapPlugin);
