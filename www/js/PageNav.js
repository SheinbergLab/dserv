/**
 * PageNav - Navigation dropdown component for ESS GUI pages
 * 
 * Usage:
 *   const nav = new PageNav('nav-container', {
 *       currentPage: 'ess_control',  // ID of current page
 *       pages: [ ... ]  // Optional: override default page list
 *   });
 * 
 * Or use the default pages by just passing the current page ID:
 *   PageNav.init('nav-container', 'ess_control');
 */

class PageNav {
    // Default page definitions - add new pages here
    static defaultPages = [
        {
            id: 'ess_control',
            title: 'ESS Control',
            desc: 'Main experiment control panel',
            icon: '🎛️',
            href: 'ess_control.html',
            category: 'control',
            windowSize: { width: 1200, height: 800 }
        },
        // Placed second so its group renders near the top: category order
        // follows first appearance here. The menu scrolls now, so this is a
        // prominence choice rather than the reachability fix it once was.
        {
            id: 'agent_manage',
            title: 'Manage System',
            desc: 'Update software, services, reboot',
            icon: '⚙️',
            href: () => PageNav.agentPanelUrl(),
            windowName: () => `agent_${PageNav.dservHostname()}`,
            category: 'system',
            windowSize: { width: 820, height: 900 }
        },
        {
            id: 'data_manager',
            title: 'Data Manager',
            desc: 'Convert and Export Data Files',
            icon: '🗂️',
            href: 'data_manager.html',
            category: 'tools',
            windowSize: { width: 1240, height: 700 }
        },
        {
            id: 'ess_workbench',
            title: 'ESS Workbench',
            desc: 'Configure State Systems',
            icon: '🧰',
            href: 'ess_workbench.html',
            category: 'config',
            windowSize: { width: 900, height: 600 }
        },
        {
            id: 'stimdg_viewer',
            title: 'StimDG Viewer',
            desc: 'View stimulus data table',
            icon: '📋',
            href: 'stimdg_viewer.html',
            category: 'viewers',
            windowSize: { width: 900, height: 600 }
        },
        {
            id: 'dg_viewer',
            title: 'DG Viewer',
            desc: 'View trial data files (.dgz)',
            icon: '📈',
            href: 'dg_viewer.html',
            category: 'viewers',
            windowSize: { width: 1200, height: 800 }
        },
        {
            id: 'event_viewer',
            title: 'Event Viewer',
            desc: 'Real-time event log',
            icon: '📊',
            href: 'event_viewer.html',
            category: 'viewers',
            windowSize: { width: 800, height: 600 }
        },
        {
            id: 'extio',
            title: 'Extio Boxes',
            desc: 'I/O box fleet status',
            icon: '🔌',
            href: 'extio.html',
            category: 'viewers',
            windowSize: { width: 1000, height: 700 }
        },
        {
            id: 'input',
            title: 'Input Devices',
            desc: 'Adopt and verify mice, trackpads, touchscreens',
            icon: '🖱️',
            href: 'input.html',
            category: 'viewers',
            windowSize: { width: 1100, height: 780 }
        },
        {
            id: 'Terminal',
            title: 'Dserv Terminal',
            desc: 'Terminal and datapoint monitor',
            icon: '🖥️',
            href: 'terminal.html',
            category: 'tools',
            windowSize: { width: 950, height: 800 }
        },
        {
            id: 'Explorer',
            title: 'Datapoint Explorer',
            desc: 'Publish and subscribe to datapoints',
            icon: '🔭',
            href: 'explorer.html',
            category: 'tools',
            windowSize: { width: 950, height: 800 }
        },
        {
            id: 'dlsh',
            title: 'DLSH Workbench',
            desc: 'Basic analysis environment',
            icon: '🔧',
            href: 'dlsh.html',
            category: 'tools',
            windowSize: { width: 900, height: 900 }
        }

        // Add more pages here as needed:
        // {
        //     id: 'eye_tracker',
        //     title: 'Eye Tracker',
        //     desc: 'Eye tracking configuration',
        //     icon: '👁️',
        //     href: 'eye_tracker.html',
        //     category: 'tools',
        //     windowSize: { width: 600, height: 500 }
        // }
    ];

    // Category display names
    static categories = {
        control: 'Control',
        viewers: 'Viewers',
        tools: 'Tools',
        config: 'Configuration',
        system: 'System'
    };

    /**
     * The machine this page is driving, without a port.
     *
     * Not necessarily the machine serving the page: ess_app.js and extio.html
     * honour ?host=<host[:port]> so a dev copy of the GUI can drive a remote
     * dserv, and "this system" has to mean the one at the other end of the
     * WebSocket, not the one holding the HTML.
     */
    static dservHostname() {
        const qsHost = new URLSearchParams(location.search).get('host');
        return (qsHost || location.host).replace(/:\d+$/, '');
    }

    /**
     * dserv-agent's management panel for a machine, opened solo.
     *
     * Defaults to the machine this page drives; pass a host to reach another
     * one (the stim2 display, which on a split rig is a different box with its
     * own agent). Always `manage=local`, never `manage=<nodeId>`: we go to that
     * host's OWN agent and ask it about itself, rather than asking the local
     * agent to resolve a node id it may never have heard of -- a stim-only box
     * runs an agent but no dserv, so it is absent from the mesh directory.
     *
     * Always plain http on port 80: dserv-agent.service runs with --no-tls.
     * That is fine for a popup even when this page is HTTPS (mixed content
     * doesn't apply to top-level windows) but is exactly why the panel can't
     * be iframed into a dserv page instead.
     */
    static agentPanelUrl(host) {
        const target = PageNav.stripPort(host) || PageNav.dservHostname();
        return `http://${target}/?manage=local&solo=1`;
    }

    /**
     * Drop a trailing :port, leaving bare IPv6 literals alone.
     *
     * ess/rmt_host may carry stim2's port; the agent is always on :80.
     */
    static stripPort(host) {
        const s = String(host || '').trim();
        if (s.startsWith('[')) return s.replace(/^(\[[^\]]*\]):\d+$/, '$1');
        // a bare IPv6 literal has several colons and no port to strip
        if ((s.match(/:/g) || []).length > 1) return s;
        return s.replace(/:\d+$/, '');
    }

    /**
     * Resolve an href/windowName that may be declared as a function.
     * Dynamic entries (the agent panel) can't be baked in at class-definition
     * time because they depend on the page's query string.
     */
    static resolve(value) {
        return typeof value === 'function' ? value() : value;
    }

    // Default window sizes for popup windows
    static defaultWindowSize = { width: 800, height: 600 };

    // Floor for the open menu on very short viewports: below this the list is
    // too small to browse, and letting it spill is the lesser evil.
    static MIN_MENU_HEIGHT = 200;

    constructor(containerId, options = {}) {
        this.container = typeof containerId === 'string' 
            ? document.getElementById(containerId) 
            : containerId;
        
        if (!this.container) {
            console.error('PageNav: Container not found:', containerId);
            return;
        }

        this.options = {
            currentPage: options.currentPage || this.detectCurrentPage(),
            pages: options.pages || PageNav.defaultPages,
            showHome: options.showHome !== false,  // Show link back to index
            homeHref: options.homeHref || '/',
            groupByCategory: options.groupByCategory !== false,
            showDescriptions: options.showDescriptions !== false,
            openMode: options.openMode || 'popup',  // 'popup', 'tab', or 'same'
            showReconnect: options.showReconnect !== false,  // Show reconnect action
            onReconnect: options.onReconnect || null,  // Callback for reconnect
            ...options
        };

        this.isOpen = false;
        this.render();
        this.bindEvents();
    }

    /**
     * Auto-detect current page from URL
     */
    detectCurrentPage() {
        const path = window.location.pathname;
        const filename = path.substring(path.lastIndexOf('/') + 1);
        
        // Match against page hrefs (dynamic hrefs point off-page, never here)
        for (const page of PageNav.defaultPages) {
            if (typeof page.href === 'function') continue;
            if (filename === page.href || filename === page.id + '.html') {
                return page.id;
            }
        }
        
        return null;
    }

    /**
     * Get current page info
     */
    getCurrentPage() {
        return this.options.pages.find(p => p.id === this.options.currentPage);
    }

    /**
     * Render the navigation component
     */
    render() {
        const currentPage = this.getCurrentPage();
        const buttonLabel = currentPage ? currentPage.title : 'Pages';
        const buttonIcon = currentPage ? currentPage.icon : '📄';

        this.container.innerHTML = `
            <div class="page-nav">
                <button class="page-nav-toggle" aria-haspopup="true" aria-expanded="false">
                    <span class="page-nav-icon">${buttonIcon}</span>
                    <span class="page-nav-label">${buttonLabel}</span>
                    <span class="page-nav-arrow">▼</span>
                </button>
                <div class="page-nav-menu" role="menu">
                    <div class="page-nav-scroll">
                        ${this.renderMenu()}
                    </div>
                    ${this.renderFooter()}
                </div>
            </div>
        `;

        this.navEl = this.container.querySelector('.page-nav');
        this.toggleBtn = this.container.querySelector('.page-nav-toggle');
        this.menuEl = this.container.querySelector('.page-nav-menu');
        this.scrollEl = this.container.querySelector('.page-nav-scroll');
    }

    /**
     * Cap the menu to the space actually below the toggle.
     *
     * page_nav.css carries a calc(100vh - ...) fallback, but that assumes the
     * toggle sits in a top bar. Measuring at open time is correct wherever the
     * host page puts it, and re-measures after a window resize for free.
     */
    fitToViewport() {
        const rect = this.toggleBtn.getBoundingClientRect();
        const avail = window.innerHeight - rect.bottom - 16;  // 4px gap + margin
        this.menuEl.style.maxHeight = `${Math.max(PageNav.MIN_MENU_HEIGHT, avail)}px`;
    }

    /**
     * Render the scrolling page list.
     *
     * Only pages live here. Home and Reconnect are in the footer: they are the
     * escape hatches, and the escape hatch must not be the thing that scrolls
     * out of reach.
     */
    renderMenu() {
        let html = '';
        
        if (this.options.groupByCategory) {
            // Group pages by category
            const grouped = this.groupPagesByCategory();
            
            for (const [category, pages] of Object.entries(grouped)) {
                const categoryName = PageNav.categories[category] || category;
                html += `<div class="page-nav-group">`;
                html += `<div class="page-nav-group-title">${categoryName}</div>`;
                html += pages.map(page => this.renderMenuItem(page)).join('');
                html += `</div>`;
            }
        } else {
            // Flat list
            html = this.options.pages.map(page => this.renderMenuItem(page)).join('');
        }

        return html;
    }

    /**
     * Render the pinned action footer (Home, Reconnect).
     *
     * Sits outside .page-nav-scroll so it stays visible however long the page
     * list grows. Both are still descendants of .page-nav-menu, so the click
     * and arrow-key handlers pick them up unchanged.
     */
    renderFooter() {
        let html = '';

        if (this.options.showHome) {
            html += `
                <a href="${this.options.homeHref}" class="page-nav-item home" role="menuitem" data-open-mode="tab">
                    <span class="page-nav-item-icon">🏠</span>
                    <div class="page-nav-item-content">
                        <div class="page-nav-item-title">Home</div>
                        ${this.options.showDescriptions ? '<div class="page-nav-item-desc">Back to dashboard</div>' : ''}
                    </div>
                </a>
            `;
        }

        if (this.options.showReconnect) {
            html += `
                <div class="page-nav-divider"></div>
                <a href="#" class="page-nav-item action" role="menuitem" data-action="reconnect">
                    <span class="page-nav-item-icon">🔄</span>
                    <div class="page-nav-item-content">
                        <div class="page-nav-item-title">Reconnect</div>
                        ${this.options.showDescriptions ? '<div class="page-nav-item-desc">Re-establish server connection</div>' : ''}
                    </div>
                </a>
            `;
        }

        if (!html) return '';
        return `<div class="page-nav-footer">${html}</div>`;
    }

    /**
     * Render a single menu item
     */
    renderMenuItem(page) {
        const isCurrent = page.id === this.options.currentPage;
        const classes = ['page-nav-item'];
        if (isCurrent) classes.push('current');
        
        // Store page info as data attributes for click handler
        const size = page.windowSize || PageNav.defaultWindowSize;
        const href = PageNav.resolve(page.href);
        const windowName = PageNav.resolve(page.windowName) || page.id;

        return `
            <a href="${href}" class="${classes.join(' ')}" role="menuitem"
               data-page-id="${page.id}"
               data-window-name="${windowName}"
               data-window-width="${size.width}"
               data-window-height="${size.height}"
               ${isCurrent ? 'aria-current="page"' : ''}>
                <span class="page-nav-item-icon">${page.icon || '📄'}</span>
                <div class="page-nav-item-content">
                    <div class="page-nav-item-title">${page.title}</div>
                    ${this.options.showDescriptions && page.desc ? `<div class="page-nav-item-desc">${page.desc}</div>` : ''}
                </div>
            </a>
        `;
    }

    /**
     * Group pages by category
     */
    groupPagesByCategory() {
        const grouped = {};
        
        for (const page of this.options.pages) {
            const category = page.category || 'other';
            if (!grouped[category]) {
                grouped[category] = [];
            }
            grouped[category].push(page);
        }
        
        return grouped;
    }

    /**
     * Bind event handlers
     */
    bindEvents() {
        // Toggle menu
        this.toggleBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.toggle();
        });

        // Handle menu item clicks
        this.menuEl.addEventListener('click', (e) => {
            const item = e.target.closest('.page-nav-item');
            if (!item) return;
            
            // Handle reconnect action
            if (item.dataset.action === 'reconnect') {
                e.preventDefault();
                this.close();
                if (this.options.onReconnect) {
                    this.options.onReconnect();
                } else if (typeof window.reconnect === 'function') {
                    window.reconnect();
                }
                return;
            }
            
            // Don't navigate if current page
            if (item.classList.contains('current')) {
                e.preventDefault();
                this.close();
                return;
            }
            
            // Handle open mode
            const openMode = item.dataset.openMode || this.options.openMode;
            const href = item.getAttribute('href');
            
            if (openMode === 'popup' && href && href !== '#') {
                e.preventDefault();
                this.openPopup(href, item.dataset);
                this.close();
            } else if (openMode === 'tab' && href && href !== '#') {
                e.preventDefault();
                window.open(href, '_blank');
                this.close();
            }
            // 'same' mode: let default link behavior happen
        });

        // Close on outside click
        document.addEventListener('click', (e) => {
            if (this.isOpen && !this.navEl.contains(e.target)) {
                this.close();
            }
        });

        // Keyboard navigation
        this.navEl.addEventListener('keydown', (e) => {
            this.handleKeydown(e);
        });

        // Keep the cap honest if the window is resized with the menu open
        window.addEventListener('resize', () => {
            if (this.isOpen) this.fitToViewport();
        });

        // Close on escape
        document.addEventListener('keydown', (e) => {
            if (e.key === 'Escape' && this.isOpen) {
                this.close();
                this.toggleBtn.focus();
            }
        });
    }
    
    /**
     * Open a page in a popup window
     */
    openPopup(href, dataset) {
        const width = parseInt(dataset.windowWidth) || PageNav.defaultWindowSize.width;
        const height = parseInt(dataset.windowHeight) || PageNav.defaultWindowSize.height;
        
        // Center the popup on the screen
        const left = Math.max(0, (screen.width - width) / 2);
        const top = Math.max(0, (screen.height - height) / 2);
        
        const features = [
            `width=${width}`,
            `height=${height}`,
            `left=${left}`,
            `top=${top}`,
            'menubar=no',
            'toolbar=no',
            'location=no',
            'status=yes',
            'resizable=yes',
            'scrollbars=yes'
        ].join(',');
        
        // Named window so reopening focuses the existing one instead of
        // stacking popups. Entries that point at a specific machine name
        // themselves per host, so two rigs get two windows.
        const windowName = dataset.windowName || dataset.pageId || 'ess_popup';
        window.open(href, windowName, features);
    }

    /**
     * Handle keyboard navigation
     */
    handleKeydown(e) {
        const items = this.menuEl.querySelectorAll('.page-nav-item:not(.current)');
        const currentIndex = Array.from(items).indexOf(document.activeElement);

        switch (e.key) {
            case 'ArrowDown':
                e.preventDefault();
                if (!this.isOpen) {
                    this.open();
                } else {
                    const nextIndex = currentIndex < items.length - 1 ? currentIndex + 1 : 0;
                    items[nextIndex]?.focus();
                }
                break;
                
            case 'ArrowUp':
                e.preventDefault();
                if (this.isOpen) {
                    const prevIndex = currentIndex > 0 ? currentIndex - 1 : items.length - 1;
                    items[prevIndex]?.focus();
                }
                break;
                
            case 'Enter':
            case ' ':
                if (document.activeElement === this.toggleBtn) {
                    e.preventDefault();
                    this.toggle();
                }
                break;
        }
    }

    /**
     * Toggle menu open/closed
     */
    toggle() {
        if (this.isOpen) {
            this.close();
        } else {
            this.open();
        }
    }

    /**
     * Open menu
     */
    open() {
        this.isOpen = true;
        this.fitToViewport();
        if (this.scrollEl) this.scrollEl.scrollTop = 0;
        this.navEl.classList.add('open');
        this.toggleBtn.classList.add('active');
        this.toggleBtn.setAttribute('aria-expanded', 'true');
        
        // Focus first non-current item
        const firstItem = this.menuEl.querySelector('.page-nav-item:not(.current)');
        if (firstItem) {
            firstItem.focus();
        }
    }

    /**
     * Close menu
     */
    close() {
        this.isOpen = false;
        this.navEl.classList.remove('open');
        this.toggleBtn.classList.remove('active');
        this.toggleBtn.setAttribute('aria-expanded', 'false');
    }

    /**
     * Static factory method for simple initialization
     */
    static init(containerId, currentPage, options = {}) {
        return new PageNav(containerId, { currentPage, ...options });
    }

    /**
     * Add a page to the default pages list (for dynamic registration)
     */
    static registerPage(page) {
        // Check if already registered
        const existing = PageNav.defaultPages.find(p => p.id === page.id);
        if (!existing) {
            PageNav.defaultPages.push(page);
        }
    }
}

// Export for module usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = PageNav;
}
