/**
 * ESS Registry Client
 * 
 * Handles communication with the dserv-agent ESS Registry API.
 * Provides script versioning, workgroup management, and collaboration features.
 */

class RegistryClient {
    constructor(options = {}) {
        this.baseUrl = options.baseUrl || '';  // Empty = same origin
        this.workgroup = options.workgroup || null;
        this.user = options.user || null;
        
        // Cache
        this.users = [];
        this.systems = [];
        this.templates = [];
    }
    
    // ==========================================
    // Configuration
    // ==========================================
    
    setWorkgroup(workgroup) {
        this.workgroup = workgroup;
        // Clear caches when workgroup changes
        this.users = [];
        this.systems = [];
    }
    
    setUser(user) {
        this.user = user;
        localStorage.setItem('ess_registry_user', user);
    }
    
    getUser() {
        if (!this.user) {
            this.user = localStorage.getItem('ess_registry_user') || null;
        }
        return this.user;
    }
    
    // ==========================================
    // HTTP Helpers
    // ==========================================
    
    async fetch(path, options = {}) {
        const url = `${this.baseUrl}/api/v1/ess${path}`;
        const response = await fetch(url, {
            ...options,
            headers: {
                'Content-Type': 'application/json',
                ...options.headers
            }
        });
        
        // Handle non-OK responses before trying to parse JSON
        if (!response.ok) {
            let errorMessage = `HTTP ${response.status}`;
            let errorData = null;
            
            // Try to parse error response as JSON, but don't fail if it's not JSON
            try {
                errorData = await response.json();
                errorMessage = errorData.error || errorMessage;
            } catch (e) {
                // Response wasn't JSON (e.g., HTML error page)
            }
            
            const error = new Error(errorMessage);
            error.status = response.status;
            error.data = errorData;
            throw error;
        }
        
        return await response.json();
    }
    
    // ==========================================
    // Users
    // ==========================================
    
    async getUsers(workgroup = null) {
        const wg = workgroup || this.workgroup;
        if (!wg) throw new Error('Workgroup required');
        
        const data = await this.fetch(`/users?workgroup=${encodeURIComponent(wg)}`);
        this.users = data.users || [];
        return this.users;
    }
    
    async addUser(username, fullName = '', email = '', role = 'editor') {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch(`/user/${encodeURIComponent(this.workgroup)}`, {
            method: 'POST',
            body: JSON.stringify({ username, fullName, email, role })
        });
    }
    
    async deleteUser(username) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch(`/user/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(username)}`, {
            method: 'DELETE'
        });
    }
    
    // ==========================================
    // Templates (Zoo)
    // ==========================================
    
    async getTemplates() {
        const data = await this.fetch('/templates');
        this.templates = data.systems || [];
        return this.templates;
    }
    
    async addToWorkgroup(templateSystem, templateVersion = 'latest') {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch('/add-to-workgroup', {
            method: 'POST',
            body: JSON.stringify({
                templateSystem,
                templateVersion,
                targetWorkgroup: this.workgroup,
                addedBy: this.getUser() || 'unknown'
            })
        });
    }
    
    // ==========================================
    // Systems
    // ==========================================
    
    async getSystems(workgroup = null) {
        const wg = workgroup || this.workgroup;
        if (!wg) throw new Error('Workgroup required');
        
        const data = await this.fetch(`/systems?workgroup=${encodeURIComponent(wg)}`);
        this.systems = data.systems || [];
        return this.systems;
    }
    
    async getSystem(systemName, version = '') {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        let path = `/system/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(systemName)}`;
        if (version) path += `/${encodeURIComponent(version)}`;
        
        return this.fetch(path);
    }
    
    async getScripts(systemName) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        const data = await this.fetch(`/scripts/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(systemName)}`);
        return data;
    }
    
    // ==========================================
    // Scripts
    // ==========================================
    
    async getScript(systemName, protocol, type) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        // Use "_" for system-level scripts (empty protocol)
        const proto = protocol || '_';
        
        return this.fetch(`/script/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(systemName)}/${encodeURIComponent(proto)}/${encodeURIComponent(type)}`);
    }
    
    async saveScript(systemName, protocol, type, content, expectedChecksum = '', comment = '') {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        const proto = protocol || '_';
        
        return this.fetch(`/script/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(systemName)}/${encodeURIComponent(proto)}/${encodeURIComponent(type)}`, {
            method: 'PUT',
            body: JSON.stringify({
                content,
                expectedChecksum,
                updatedBy: this.getUser() || 'unknown',
                comment
            })
        });
    }
    
    async getScriptHistory(systemName, protocol, type) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        const proto = protocol || '_';
        
        return this.fetch(`/history/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(systemName)}/${encodeURIComponent(proto)}/${encodeURIComponent(type)}`);
    }
    
    // ==========================================
    // Libraries
    // ==========================================
    
    async getLibs(workgroup = null) {
        const wg = workgroup || this.workgroup || '_templates';
        return this.fetch(`/libs?workgroup=${encodeURIComponent(wg)}`);
    }
    
    async getLib(name, version = '') {
        const wg = this.workgroup || '_templates';
        let path = `/lib/${encodeURIComponent(wg)}/${encodeURIComponent(name)}`;
        if (version) path += `/${encodeURIComponent(version)}`;
        return this.fetch(path);
    }
    
    // ==========================================
    // Projects
    // ==========================================
    
    async getProjects(workgroup = null) {
        const wg = workgroup || this.workgroup;
        if (!wg) throw new Error('Workgroup required');
        
        return this.fetch(`/projects?workgroup=${encodeURIComponent(wg)}`);
    }
    
    async getProject(name) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch(`/project/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(name)}`);
    }
    
    async saveProject(name, config) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch(`/project/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(name)}`, {
            method: 'PUT',
            body: JSON.stringify(config)
        });
    }
    
    async deleteProject(name) {
        if (!this.workgroup) throw new Error('Workgroup required');
        
        return this.fetch(`/project/${encodeURIComponent(this.workgroup)}/${encodeURIComponent(name)}`, {
            method: 'DELETE'
        });
    }
    
    // ==========================================
    // Locks
    // ==========================================
    
    async acquireLock(systemName, protocol = null) {
        let key = `${this.workgroup}/${systemName}`;
        if (protocol) key += `/${protocol}`;
        
        return this.fetch('/lock', {
            method: 'POST',
            body: JSON.stringify({
                key,
                lockedBy: this.getUser() || 'unknown'
            })
        });
    }
    
    async releaseLock(systemName, protocol = null) {
        let key = `${this.workgroup}/${systemName}`;
        if (protocol) key += `/${protocol}`;
        
        return this.fetch('/lock', {
            method: 'DELETE',
            body: JSON.stringify({
                key,
                lockedBy: this.getUser() || 'unknown'
            })
        });
    }
    
    async getLocks() {
        return this.fetch('/locks');
    }
    
    // ==========================================
    // Admin
    // ==========================================
    
    async seedTemplates(sourcePath, systems = []) {
        return this.fetch('/admin/seed-templates', {
            method: 'POST',
            body: JSON.stringify({ sourcePath, systems })
        });
    }
    
    // ==========================================
    // Utilities
    // ==========================================
    
    /**
     * Normalize content for comparison (trim trailing whitespace, normalize line endings)
     */
    normalizeContent(content) {
        if (!content) return '';
        return content
            .replace(/\r\n/g, '\n')      // Normalize CRLF to LF
            .replace(/[ \t]+$/gm, '')     // Trim trailing whitespace from each line
            .replace(/\n+$/, '\n');       // Ensure single trailing newline
    }
    
    /**
     * Compare local script with registry version
     * Returns: { inSync: boolean, localChecksum: string, registryChecksum: string }
     */
    async compareScript(systemName, protocol, type, localContent) {
        try {
            const remote = await this.getScript(systemName, protocol, type);
            
            // Normalize both contents before comparing
            const normalizedLocal = this.normalizeContent(localContent);
            const normalizedRemote = this.normalizeContent(remote.content);
            
            const localChecksum = await this.computeChecksum(normalizedLocal);
            const remoteChecksum = await this.computeChecksum(normalizedRemote);
            
            return {
                inSync: normalizedLocal === normalizedRemote,
                localChecksum,
                registryChecksum: remote.checksum,
                registryUpdatedAt: remote.updatedAt,
                registryUpdatedBy: remote.updatedBy
            };
        } catch (e) {
            if (e.status === 404) {
                return { inSync: false, notInRegistry: true };
            }
            throw e;
        }
    }
    
    /**
     * Compute SHA256 checksum (matches server-side)
     */
    async computeChecksum(content) {
        const encoder = new TextEncoder();
        const data = encoder.encode(content);
        const hashBuffer = await crypto.subtle.digest('SHA-256', data);
        const hashArray = Array.from(new Uint8Array(hashBuffer));
        return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
    }
}

// Export for module systems
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { RegistryClient };
}
