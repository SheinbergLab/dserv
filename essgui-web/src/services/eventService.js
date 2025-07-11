// eventService.js - Core event handling service for dserv event system

// Parameter types from eventlog.h (corrected based on essterm.go)
export const PUT_TYPES = {
  PUT_unknown: 0,
  PUT_null: 1,
  PUT_string: 1,  // This seems to conflict - let me check essterm.go
  PUT_short: 4,
  PUT_long: 5,
  PUT_float: 2,
  PUT_double: 7
};

// Let me use the constants from essterm.go instead:
export const PTYPE_CONSTANTS = {
  PtypeByte: 0,
  PtypeString: 1,
  PtypeFloat: 2,
  PtypeShort: 4,
  PtypeInt: 5
};

// Special event types
export const EVENT_TYPES = {
  E_MAGIC: 0,
  E_NAME: 1,
  E_FILE: 2,
  E_USER: 3,
  E_TRACE: 4,
  E_PARAM: 5,
  E_FSPIKE: 16,
  E_HSPIKE: 17,
  E_ID: 18,
  E_BEGINOBS: 19,
  E_ENDOBS: 20,
  E_ISI: 21,
  E_TRIALTYPE: 22,
  E_OBSTYPE: 23,
  E_EMLOG: 24,
  E_FIXSPOT: 25,
  E_EMPARAMS: 26,
  E_STIMULUS: 27,
  E_PATTERN: 28,
  E_STIMTYPE: 29,
  E_SAMPLE: 30,
  E_PROBE: 31,
  E_CUE: 32,
  E_TARGET: 33,
  E_DISTRACTOR: 34,
  E_SOUND: 35,
  E_FIXATE: 36,
  E_RESP: 37,
  E_SACCADE: 38,
  E_DECIDE: 39,
  E_ENDTRIAL: 40,
  E_ABORT: 41,
  E_REWARD: 42
};

// Event subtype definitions
export const EVENT_SUBTYPES = {
  // USER event subtypes
  USER: {
    E_USER_START: 0,
    E_USER_QUIT: 1, 
    E_USER_RESET: 2,
    E_USER_SYSTEM: 3
  },
  // TRACE event subtypes
  TRACE: {
    E_TRACE_ACT: 0,
    E_TRACE_TRANS: 1,
    E_TRACE_WAKE: 2,
    E_TRACE_DEBUG: 3
  },
  // FIXSPOT subtypes
  FIXSPOT: {
    E_FIXSPOT_OFF: 0,
    E_FIXSPOT_ON: 1,
    E_FIXSPOT_SET: 2
  },
  // STIMULUS subtypes
  STIMULUS: {
    E_STIMULUS_OFF: 0,
    E_STIMULUS_ON: 1,
    E_STIMULUS_SET: 2
  },
  // RESPONSE subtypes
  RESP: {
    E_RESP_LEFT: 0,
    E_RESP_RIGHT: 1,
    E_RESP_BOTH: 2,
    E_RESP_NONE: 3,
    E_RESP_MULTI: 4,
    E_RESP_EARLY: 5
  },
  // END TRIAL subtypes
  ENDTRIAL: {
    E_ENDTRIAL_INCORRECT: 0,
    E_ENDTRIAL_CORRECT: 1,
    E_ENDTRIAL_ABORT: 2
  },
  // ABORT subtypes
  ABORT: {
    E_ABORT_EM: 0,
    E_ABORT_LEVER: 1,
    E_ABORT_NORESPONSE: 2,
    E_ABORT_STIM: 3
  }
};

class EventService {
  constructor() {
    this.evtTypeNames = new Array(256);
    this.evtSubtypeNames = new Map();
    this.obsInfo = {
      obsStart: 0,
      obsCount: -1,
      events: []
    };
    this.eventHandlers = new Set();
    this.isInitialized = false;
    this.dservCleanup = null;
    this.initialize();
  }

  // Initialize the service and start listening to dserv events immediately
  async initializeGlobalTracking(dservInstance) {
    if (this.isInitialized) return; // Already initialized
    
    console.log('Starting global event tracking...');
    
    // Subscribe to eventlog events globally
    this.dservCleanup = dservInstance.registerComponent('EventService', {
      subscriptions: [
        { pattern: 'eventlog/events', every: 1 }
      ]
    });

    // Set up global event handler
    dservInstance.on('datapoint:eventlog/events', this.handleGlobalDatapoint.bind(this));
    
    this.isInitialized = true;
    console.log('Global event tracking started');
  }

  // Global datapoint handler - processes ALL events for state management
  handleGlobalDatapoint(data) {
    if (data.name === 'eventlog/events') {
      // ALWAYS process events for names, state tracking, etc.
      this.processEvent(data); // Use processEvent, not parseEvent!
    }
  }

  // Cleanup global tracking
  cleanup() {
    if (this.dservCleanup) {
      this.dservCleanup();
      this.dservCleanup = null;
    }
    this.isInitialized = false;
  }

  initialize() {
    // Initialize event type names like in esteem.go
    for (let i = 0; i < 16; i++) {
      this.evtTypeNames[i] = `Reserved${i}`;
    }
    for (let i = 16; i < 128; i++) {
      this.evtTypeNames[i] = `System${i}`;
    }
    for (let i = 128; i < 256; i++) {
      this.evtTypeNames[i] = `User${i}`;
    }

    // Set known event names from evt_name.h
    this.evtTypeNames[0] = "Magic Number";
    this.evtTypeNames[1] = "Event Name";
    this.evtTypeNames[2] = "File I/O";
    this.evtTypeNames[3] = "User Interaction";
    this.evtTypeNames[4] = "State System Trace";
    this.evtTypeNames[5] = "Parameter Set";
    this.evtTypeNames[16] = "Time Stamped Spike";
    this.evtTypeNames[17] = "DIS-1 Hardware Spike";
    this.evtTypeNames[18] = "Name";
    this.evtTypeNames[19] = "Start Obs Period";
    this.evtTypeNames[20] = "End Obs Period";
    this.evtTypeNames[21] = "ISI";
    this.evtTypeNames[22] = "Trial Type";
    this.evtTypeNames[23] = "Obs Period Type";
    this.evtTypeNames[24] = "EM Log";
    this.evtTypeNames[25] = "Fixspot";
    this.evtTypeNames[26] = "EM Params";
    this.evtTypeNames[27] = "Stimulus";
    this.evtTypeNames[28] = "Pattern";
    this.evtTypeNames[29] = "Stimulus Type";
    this.evtTypeNames[30] = "Sample";
    this.evtTypeNames[31] = "Probe";
    this.evtTypeNames[32] = "Cue";
    this.evtTypeNames[33] = "Target";
    this.evtTypeNames[34] = "Distractor";
    this.evtTypeNames[35] = "Sound Event";
    this.evtTypeNames[36] = "Fixation";
    this.evtTypeNames[37] = "Response";
    this.evtTypeNames[38] = "Saccade";
    this.evtTypeNames[39] = "Decide";
    this.evtTypeNames[40] = "EOT";
    this.evtTypeNames[41] = "Abort";
    this.evtTypeNames[42] = "Reward";
  }

  // Process an event from dserv (similar to processEvent in esteem.go)
  processEvent(eventData) {
    const event = this.parseEvent(eventData);
    if (!event) return null;

    // Handle special events that modify the system state
    switch (event.type) {
      case EVENT_TYPES.E_USER: // USER event
        this.handleUserEvent(event);
        break;
        
      case EVENT_TYPES.E_NAME: // NAMESET - dynamic event type naming
        const newName = this.decodeParams(event);
        if (newName && event.subtype > 1 && event.subtype < 256) {
          this.evtTypeNames[event.subtype] = newName;
          this.notifyHandlers({ 
            type: 'name_update', 
            eventType: event.subtype, 
            newName: newName 
          });
        }
        return null; // Don't add to display
        
      case 6: // SUBTYPE NAMES (SUBTYPES event)
        this.handleSubtypeNames(event);
        return null;
        
      case EVENT_TYPES.E_BEGINOBS: // BEGINOBS
        this.obsStart(event);
        break;
        
      case EVENT_TYPES.E_ENDOBS: // ENDOBS
        this.obsEnd(event);
        break;
    }

    // Adjust timestamp relative to observation start
    if (this.obsInfo.obsCount >= 0) {
      event.timestamp -= this.obsInfo.obsStart;
    }

    this.obsAddEvent(event);
    this.notifyHandlers(event);
    return event;
  }

  // Parse event from dserv datapoint
  parseEvent(eventData) {
    try {
      if (eventData.name !== "eventlog/events") {
        return null;
      }

      return {
        type: eventData.e_type,
        subtype: eventData.e_subtype,
        timestamp: eventData.timestamp,
        ptype: eventData.e_dtype,
        params: eventData.e_params
      };
    } catch (error) {
      console.error('Failed to parse event:', error, eventData);
      return null;
    }
  }

  // Decode event parameters based on parameter type
  decodeParams(event) {
    if (!event.params) return '';

    try {
      // Use the constants from essterm.go
      switch (event.ptype) {
        case PTYPE_CONSTANTS.PtypeByte:
        case 0: // PUT_unknown/null
          return '';
        case PTYPE_CONSTANTS.PtypeString:
        case 1: // String type
          if (typeof event.params === 'string') {
            return event.params;
          } else {
            return JSON.stringify(event.params).replace(/"/g, '');
          }
        case PTYPE_CONSTANTS.PtypeFloat:
        case 2: // Float type
          if (Array.isArray(event.params)) {
            return event.params.map(p => Number(p).toFixed(3)).join(' ');
          }
          return Number(event.params).toFixed(3);
        case PTYPE_CONSTANTS.PtypeShort:
        case 4: // Short type
          return Array.isArray(event.params) ? event.params.join(' ') : event.params.toString();
        case PTYPE_CONSTANTS.PtypeInt:
        case 5: // Int/Long type
          return Array.isArray(event.params) ? event.params.join(' ') : event.params.toString();
        default:
          return event.params ? event.params.toString() : '';
      }
    } catch (error) {
      console.error('Failed to decode params:', error, event);
      return event.params ? event.params.toString() : '';
    }
  }

  // Get event type name
  getEventTypeName(type) {
    return this.evtTypeNames[type] || `Unknown${type}`;
  }

  // Get event subtype name
  getEventSubtypeName(type, subtype) {
    const key = `${type}:${subtype}`;
    return this.evtSubtypeNames.get(key) || subtype.toString();
  }

  // Handle user events
  handleUserEvent(event) {
    switch (event.subtype) {
      case EVENT_SUBTYPES.USER.E_USER_START:
        console.log('System started');
        break;
      case EVENT_SUBTYPES.USER.E_USER_QUIT:
        console.log('System stopped');
        break;
      case EVENT_SUBTYPES.USER.E_USER_RESET:
        this.obsReset();
        break;
    }
  }

  // Handle subtype name events
  handleSubtypeNames(event) {
    const params = this.decodeParams(event);
    
    if (typeof params === 'string' && params.trim() !== '') {
      const stypes = params.split(' ');
      
      // Format is "DURATION 0 TYPE 1 MICROLITERS 2" - name first, then number
      for (let i = 0; i < stypes.length; i += 2) {
        if (i + 1 < stypes.length) {
          const subtypeName = stypes[i];       // "DURATION", "TYPE", "MICROLITERS"
          const subtypeNumber = stypes[i + 1]; // "0", "1", "2"
          const key = `${event.subtype}:${subtypeNumber}`;
          this.evtSubtypeNames.set(key, subtypeName);
        }
      }
    }
    
    // Notify handlers of subtype name updates
    this.notifyHandlers({ 
      type: 'subtype_names_update', 
      eventType: event.subtype,
      subtypes: params
    });
  }

  // Observation handling
  obsReset() {
    this.obsInfo.obsCount = -1;
    this.obsInfo.events = [];
    this.notifyHandlers({ type: 'obs_reset' });
  }

  obsStart(event) {
    this.obsInfo.obsCount++;
    this.obsInfo.obsStart = event.timestamp;
    this.obsInfo.events.push([event]);
    this.notifyHandlers({ type: 'obs_start', obsCount: this.obsInfo.obsCount });
  }

  obsAddEvent(event) {
    if (this.obsInfo.obsCount === -1) return;
    
    if (!this.obsInfo.events[this.obsInfo.obsCount]) {
      this.obsInfo.events[this.obsInfo.obsCount] = [];
    }
    this.obsInfo.events[this.obsInfo.obsCount].push(event);
  }

  obsEnd(event) {
    this.notifyHandlers({ type: 'obs_end', obsCount: this.obsInfo.obsCount });
  }

  // Event handler management
  addHandler(handler) {
    this.eventHandlers.add(handler);
    return () => this.eventHandlers.delete(handler);
  }

  notifyHandlers(event) {
    this.eventHandlers.forEach(handler => {
      try {
        handler(event);
      } catch (error) {
        console.error('Event handler error:', error);
      }
    });
  }

  // Format timestamp for display
  formatTimestamp(timestamp) {
    if (this.obsInfo.obsCount >= 0) {
      // Relative to observation start, in seconds with 1 decimal place
      return (timestamp / 1000).toFixed(1);
    } else {
      // Absolute timestamp
      return new Date(timestamp / 1000).toLocaleTimeString();
    }
  }

  // Get current observation info
  getObsInfo() {
    return { ...this.obsInfo };
  }

  // Get all events for current observation
  getCurrentObsEvents() {
    if (this.obsInfo.obsCount >= 0 && this.obsInfo.events[this.obsInfo.obsCount]) {
      return [...this.obsInfo.events[this.obsInfo.obsCount]];
    }
    return [];
  }

  // Export event data
  exportEvents(format = 'json') {
    const data = {
      obsInfo: this.obsInfo,
      typeNames: this.evtTypeNames,
      subtypeNames: Object.fromEntries(this.evtSubtypeNames)
    };

    switch (format) {
      case 'json':
        return JSON.stringify(data, null, 2);
      case 'csv':
        return this.exportEventsCSV();
      default:
        return data;
    }
  }

  // Request fresh event names from the system (useful when system changes)
  async requestEventNameRefresh(dservInstance) {
    if (!dservInstance) {
      console.warn('No dserv instance provided for event name refresh');
      return;
    }
    
    try {
      console.log('Requesting event name refresh...');
      // This triggers the system to send all current event names
      await dservInstance.essCommand('::ess::store_evt_names');
      console.log('Event name refresh requested');
    } catch (error) {
      console.error('Failed to request event name refresh:', error);
    }
  }

  // Clear all dynamic names and reset to defaults
  resetEventNames() {
    console.log('Resetting event names to defaults');
    this.initialize();
    this.evtSubtypeNames.clear();
    this.notifyHandlers({ type: 'names_reset' });
  }

  // Get statistics about current event names
  getEventNameStats() {
    const customNames = this.evtTypeNames.filter((name, index) => 
      name && !name.startsWith('Reserved') && !name.startsWith('System') && !name.startsWith('User')
    ).length;
    
    return {
      totalTypeNames: this.evtTypeNames.filter(name => name && name.trim()).length,
      customTypeNames: customNames,
      subtypeNames: this.evtSubtypeNames.size,
      lastUpdated: Date.now()
    };
  }

  exportEventsCSV() {
    const headers = ['Timestamp', 'Type', 'Subtype', 'TypeName', 'SubtypeName', 'Parameters'];
    const rows = [headers];

    this.obsInfo.events.forEach((obsEvents, obsIndex) => {
      obsEvents.forEach(event => {
        rows.push([
          this.formatTimestamp(event.timestamp),
          event.type,
          event.subtype,
          this.getEventTypeName(event.type),
          this.getEventSubtypeName(event.type, event.subtype),
          this.decodeParams(event)
        ]);
      });
    });

    return rows.map(row => row.map(cell => `"${cell}"`).join(',')).join('\n');
  }
}

  // Create singleton instance and expose globally
export const eventService = new EventService();

// Auto-initialize when dserv is available (call this from your main app)
export const initializeEventTracking = async (dservInstance) => {
  return await eventService.initializeGlobalTracking(dservInstance);
};
