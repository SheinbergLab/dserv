/**
 * TclParser.js
 * Utilities for parsing Tcl-formatted data in JavaScript
 * 
 * Handles nested braces, quoted strings, and whitespace-separated lists
 */

class TclParser {
    /**
     * Parse a Tcl list string into an array
     * Handles: {item1} {item2} item3 "quoted item"
     * 
     * @param {string} str - The Tcl list string
     * @returns {string[]} Array of list elements
     */
    static parseList(str) {
        if (!str || typeof str !== 'string') {
            return [];
        }
        
        str = str.trim();
        if (str === '') {
            return [];
        }
        
        const result = [];
        let i = 0;
        
        while (i < str.length) {
            // Skip whitespace
            while (i < str.length && /\s/.test(str[i])) {
                i++;
            }
            
            if (i >= str.length) break;
            
            let element;
            
            if (str[i] === '{') {
                // Braced element - find matching close brace
                const start = i + 1;
                let depth = 1;
                i++;
                
                while (i < str.length && depth > 0) {
                    if (str[i] === '{') {
                        depth++;
                    } else if (str[i] === '}') {
                        depth--;
                    }
                    i++;
                }
                
                element = str.slice(start, i - 1);
            } else if (str[i] === '"') {
                // Quoted element
                const start = i + 1;
                i++;
                
                while (i < str.length && str[i] !== '"') {
                    if (str[i] === '\\' && i + 1 < str.length) {
                        i++; // Skip escaped char
                    }
                    i++;
                }
                
                element = str.slice(start, i);
                i++; // Skip closing quote
            } else {
                // Unquoted element - read until whitespace
                const start = i;
                
                while (i < str.length && !/\s/.test(str[i])) {
                    i++;
                }
                
                element = str.slice(start, i);
            }
            
            result.push(element);
        }
        
        return result;
    }
    
    /**
     * Parse a Tcl dict string into a JavaScript object
     * Handles: {key1 val1 key2 val2} or key1 val1 key2 val2
     * 
     * @param {string} str - The Tcl dict string
     * @returns {Object} JavaScript object
     */
    static parseDict(str) {
        const list = this.parseList(str);
        const result = {};
        
        for (let i = 0; i < list.length - 1; i += 2) {
            result[list[i]] = list[i + 1];
        }
        
        return result;
    }
    
    /**
     * Parse nested variant settings format
     * Input: {variant1 {{param1 val1} {param2 val2}}} {variant2 {{param1 val3}}}
     * Output: { variant1: { param1: val1, param2: val2 }, variant2: { param1: val3 } }
     * 
     * @param {string} str - The variants string
     * @returns {Object} Nested object of variants and their parameters
     */
    static parseVariants(str) {
        const variants = {};
        const items = this.parseList(str);
        
        for (const item of items) {
            const parts = this.parseList(item);
            if (parts.length >= 2) {
                const variantName = parts[0];
                const paramsList = this.parseList(parts[1]);
                
                variants[variantName] = {};
                
                for (const paramItem of paramsList) {
                    const paramParts = this.parseList(paramItem);
                    if (paramParts.length >= 2) {
                        variants[variantName][paramParts[0]] = paramParts[1];
                    }
                }
            } else if (parts.length === 1) {
                // Variant with no params
                variants[parts[0]] = {};
            }
        }
        
        return variants;
    }
    
    /**
     * Parse parameter settings dict format
     * Input: {param1 {value type dtype} param2 {value type dtype}}
     * Output: { param1: { value, type, dtype }, param2: { value, type, dtype } }
     * 
     * @param {string} str - The param settings string
     * @returns {Object} Parsed parameters
     */
    static parseParamSettings(str) {
        const params = {};
        const dict = this.parseDict(str);
        
        for (const [key, value] of Object.entries(dict)) {
            const parts = this.parseList(value);
            
            if (parts.length === 3) {
                params[key] = {
                    value: parts[0],
                    varType: parts[1],  // 1=time, 2=variable
                    dataType: parts[2]  // int, float, string
                };
            } else if (parts.length === 2) {
                // Missing value
                params[key] = {
                    value: '',
                    varType: parts[0],
                    dataType: parts[1]
                };
            }
        }
        
        return params;
    }
    
    /**
     * Parse key-value pairs from a Tcl list
     * Input: "key1 val1 key2 val2"
     * Output: { key1: val1, key2: val2 }
     * 
     * @param {string} str - The key-value string
     * @returns {Object} JavaScript object
     */
    static parseKeyValue(str) {
        const list = this.parseList(str);
        const result = {};
        
        for (let i = 0; i < list.length - 1; i += 2) {
            result[list[i]] = list[i + 1];
        }
        
        return result;
    }
    
    /**
     * Convert a JavaScript value to Tcl format
     * 
     * @param {any} value - Value to convert
     * @returns {string} Tcl-formatted string
     */
    static toTcl(value) {
        if (value === null || value === undefined) {
            return '';
        }
        
        if (Array.isArray(value)) {
            return '{' + value.map(v => this.toTcl(v)).join(' ') + '}';
        }
        
        if (typeof value === 'object') {
            const pairs = [];
            for (const [k, v] of Object.entries(value)) {
                pairs.push(k, this.toTcl(v));
            }
            return '{' + pairs.join(' ') + '}';
        }
        
        const str = String(value);
        
        // Check if we need braces
        if (str === '' || /\s/.test(str) || /[{}"\\]/.test(str)) {
            return '{' + str + '}';
        }
        
        return str;
    }
    
    /**
     * Escape a string for Tcl
     * 
     * @param {string} str - String to escape
     * @returns {string} Escaped string
     */
    static escape(str) {
        if (typeof str !== 'string') {
            str = String(str);
        }
        
        return str
            .replace(/\\/g, '\\\\')
            .replace(/"/g, '\\"')
            .replace(/\[/g, '\\[')
            .replace(/\]/g, '\\]')
            .replace(/\$/g, '\\$')
            .replace(/\n/g, '\\n')
            .replace(/\r/g, '\\r')
            .replace(/\t/g, '\\t');
    }
}

// Export for use
if (typeof window !== 'undefined') {
    window.TclParser = TclParser;
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = TclParser;
}
