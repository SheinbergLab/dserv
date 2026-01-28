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
            id: 'event_viewer',
            title: 'Event Viewer',
            desc: 'Real-time event log',
            icon: '📊',
            href: 'event_viewer.html',
            category: 'viewers',
            windowSize: { width: 800, height: 600 }
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
        config: 'Configuration'
    };
    
    // Default window sizes for popup windows
    static defaultWindowSize = { width: 800, height: 600 };

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
        
        // Match against page hrefs
        for (const page of PageNav.defaultPages) {
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
                    ${this.renderMenu()}
                </div>
            </div>
        `;

        this.navEl = this.container.querySelector('.page-nav');
        this.toggleBtn = this.container.querySelector('.page-nav-toggle');
        this.menuEl = this.container.querySelector('.page-nav-menu');
    }

    /**
     * Render menu contents
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

        // Add home link
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
        
        // Add reconnect action
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

        return html;
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
        
        return `
            <a href="${page.href}" class="${classes.join(' ')}" role="menuitem" 
               data-page-id="${page.id}"
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
        
        // Use page ID as window name so reopening focuses existing window
        const windowName = dataset.pageId || 'ess_popup';
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
