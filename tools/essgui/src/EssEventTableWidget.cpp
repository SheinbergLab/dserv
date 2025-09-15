// EssEventTableWidget.cpp - FLTK version
#include "EssEventTableWidget.h"
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <jansson.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

// EssEventTable implementation
EssEventTable::EssEventTable(int X, int Y, int W, int H, const char *L)
    : Fl_Table_Row(X, Y, W, H, L)
    , m_currentObsIndex(-1)
    , m_currentObsStart(0)
    , m_obsCount(0)
    , m_obsTotal(0)
    , m_maxEvents(1000)
{
    cols(5);
    col_header(1);
    col_width(0, 60);   // Time
    col_width(1, 50);   // Δt
    col_width(2, 100);  // Type
    col_width(3, 100);  // Subtype
    col_width(4, 200);  // Parameters
    
    rows(0);
    row_height_all(20);
    
    initializeEventNames();
    
    end();
}

EssEventTable::~EssEventTable() {
}

void EssEventTable::initializeEventNames() {
    m_eventTypeNames.resize(256);
    
    // Initialize with default names (from essterm.go)
    for (int i = 0; i < 16; i++) {
        m_eventTypeNames[i] = "Reserved" + std::to_string(i);
    }
    for (int i = 16; i < 128; i++) {
        m_eventTypeNames[i] = "System" + std::to_string(i);
    }
    for (int i = 128; i < 256; i++) {
        m_eventTypeNames[i] = "User" + std::to_string(i);
    }
}

bool EssEventTable::shouldDisplayEvent(const EssEvent &event) const {
    switch (event.type) {
        case EVT_TRACE:
        case EVT_USER:
        case EVT_NAMESET:
        case EVT_PARAM:
        case EVT_FILEIO:
        case EVT_SYSTEM_CHANGES:
        case EVT_SUBTYPE_NAMES:
        case EVT_STATE_DEBUG:
            return false;
        default:
            return true;
    }
}

std::string EssEventTable::formatEventParams(const EssEvent &event) const {
    std::string paramStr = event.params;
    
    // Check for empty parameters
    if (paramStr.empty() || 
        paramStr == "[]" || 
        paramStr == "{}" || 
        paramStr == "null" ||
        paramStr == "\"\"") {
        return "";
    }
    
    // Remove surrounding quotes if present for strings
    if (paramStr.length() >= 2 && paramStr[0] == '"' && paramStr.back() == '"') {
        paramStr = paramStr.substr(1, paramStr.length() - 2);
        return paramStr;
    }
    
    // Handle JSON arrays of numbers (from DSERV_FLOAT, DSERV_DOUBLE, etc.)
    if (paramStr[0] == '[' && paramStr.back() == ']') {
        json_error_t error;
        json_t *array = json_loads(paramStr.c_str(), 0, &error);
        
        if (array && json_is_array(array)) {
            std::ostringstream formatted;
            size_t array_size = json_array_size(array);
            
            for (size_t i = 0; i < array_size; i++) {
                json_t *element = json_array_get(array, i);
                
                if (json_is_real(element)) {
                    double value = json_real_value(element);
                    formatted << std::fixed << std::setprecision(3) << value;
                } else if (json_is_integer(element)) {
                    formatted << json_integer_value(element);
                } else if (json_is_string(element)) {
                    formatted << json_string_value(element);
                }
                
                if (i < array_size - 1) {
                    formatted << ",";
                }
            }
            
            json_decref(array);
            return formatted.str();
        }
        
        if (array) json_decref(array);
    }
    
    // Handle single float values
    char *endptr;
    double value = strtod(paramStr.c_str(), &endptr);
    if (endptr != paramStr.c_str() && *endptr == '\0') {
        // This is a valid number
        if (value == floor(value)) {
            // Integer value, no decimal places needed
            return std::to_string((long long)value);
        } else {
            // Float value, limit to 3 decimal places
            std::ostringstream formatted;
            formatted << std::fixed << std::setprecision(3) << value;
            std::string result = formatted.str();
            
            // Remove trailing zeros
            result.erase(result.find_last_not_of('0') + 1, std::string::npos);
            result.erase(result.find_last_not_of('.') + 1, std::string::npos);
            
            return result;
        }
    }
    
    return paramStr;
}

std::string EssEventTable::getEventTypeName(uint8_t type) const {
    if (type < m_eventTypeNames.size()) {
        return m_eventTypeNames[type];
    }
    return "Type_" + std::to_string(type);
}

std::string EssEventTable::getEventSubtypeName(uint8_t type, uint8_t subtype) const {
    std::string key = std::to_string(type) + ":" + std::to_string(subtype);
    auto it = m_eventSubtypeNames.find(key);
    if (it != m_eventSubtypeNames.end()) {
        return it->second;
    }
    return std::to_string(subtype);
}

void EssEventTable::draw_cell(TableContext context, int ROW, int COL, int X, int Y, int W, int H) {
    switch (context) {
        case CONTEXT_STARTPAGE:
            fl_font(FL_HELVETICA, 12);
            return;
            
        case CONTEXT_COL_HEADER: {
            fl_push_clip(X, Y, W, H);
            fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, FL_BACKGROUND_COLOR);
            fl_color(FL_BLACK);
            const char* headers[] = {"Time", "Δt", "Type", "Subtype", "Parameters"};
            if (COL < 5) {
                fl_draw(headers[COL], X + 3, Y, W - 6, H, FL_ALIGN_LEFT);
            }
            fl_pop_clip();
            return;
        }
        
        case CONTEXT_CELL: {
            if (m_currentObsIndex < 0 || m_currentObsIndex >= (int)m_observationHistory.size()) {
                return;
            }
            
            const auto& currentObs = m_observationHistory[m_currentObsIndex];
            std::vector<EssEvent> displayEvents;
            
            for (const auto& event : currentObs.events) {
                if (shouldDisplayEvent(event)) {
                    displayEvents.push_back(event);
                }
            }
            
            if (ROW >= (int)displayEvents.size()) {
                return;
            }
            
            const EssEvent& event = displayEvents[ROW];
            
            fl_push_clip(X, Y, W, H);
            
            // Alternate row colors
            Fl_Color bg_color = (ROW % 2) ? FL_WHITE : fl_rgb_color(245, 245, 245);
            fl_draw_box(FL_FLAT_BOX, X, Y, W, H, bg_color);
            
            fl_color(FL_BLACK);
            std::string cellText;
            
            switch (COL) {
                case 0: { // Time
                    uint64_t relativeTime = event.timestamp - currentObs.startTime;
                    cellText = std::to_string(relativeTime / 1000); // ms
                    break;
                }
                case 1: { // Elapsed time
                    if (ROW > 0) {
                        const EssEvent& prevEvent = displayEvents[ROW - 1];
                        uint64_t elapsed = event.timestamp - prevEvent.timestamp;
                        if (elapsed < 1000000) { // Less than 1 second
                            cellText = std::to_string(elapsed / 1000.0).substr(0, 5);
                        } else {
                            cellText = std::to_string(elapsed / 1000);
                        }
                    }
                    break;
                }
                case 2: // Type
                    cellText = getEventTypeName(event.type);
                    break;
                case 3: // Subtype
                    cellText = getEventSubtypeName(event.type, event.subtype);
                    break;
                case 4: // Parameters
                    cellText = formatEventParams(event);
                    break;
            }
            
            fl_draw(cellText.c_str(), X + 3, Y, W - 6, H, FL_ALIGN_LEFT);
            fl_pop_clip();
            return;
        }
        
        default:
            return;
    }
}

void EssEventTable::processEvent(const EssEvent &event) {
    if (event.type == EVT_SYSTEM_CHANGES) {
        // Clear all history on system changes
        m_observationHistory.clear();
        m_currentObsIndex = -1;
        rows(0);
        redraw();
        return;
    }

    // Handle EVT_BEGINOBS to start new observation
    if (event.type == EVT_BEGINOBS) {
        ObservationData newObs;
        newObs.startTime = event.timestamp;
        newObs.obsCount = m_obsCount;
        newObs.obsTotal = m_obsTotal;
        
        m_observationHistory.push_back(newObs);
        m_currentObsIndex = m_observationHistory.size() - 1;
        m_currentObsStart = event.timestamp;
        
        rows(0); // Clear display for new observation
    }
    
    addEventToCurrentObs(event);
    
    // Update display if we're viewing the current observation
    if (m_currentObsIndex == (int)m_observationHistory.size() - 1) {
        showObservation(m_currentObsIndex);
    }
}

void EssEventTable::addEventToCurrentObs(const EssEvent &event) {
    if (m_currentObsIndex >= 0 && m_currentObsIndex < (int)m_observationHistory.size()) {
        m_observationHistory[m_currentObsIndex].events.push_back(event);
    }
}

void EssEventTable::processEventlogData(const std::string &jsonData) {
    json_error_t error;
    json_t *root = json_loads(jsonData.c_str(), 0, &error);
    
    if (!root) {
        return;
    }
    
    // First check if this is actually an eventlog/events message
    json_t *name_obj = json_object_get(root, "name");
    json_t *dtype_obj = json_object_get(root, "dtype");
    
    const char *name_str = json_string_value(name_obj);
    if (!name_str || strcmp(name_str, "eventlog/events") != 0) {
        json_decref(root);
        return;
    }
    
    int dtype = json_integer_value(dtype_obj);
    if (dtype != 9) { // DSERV_EVT
        json_decref(root);
        return;
    }
    
    EssEvent event;
    
    // For DSERV_EVT type, the event fields are directly in the JSON object
    json_t *type_obj = json_object_get(root, "e_type");
    json_t *subtype_obj = json_object_get(root, "e_subtype");
    json_t *timestamp_obj = json_object_get(root, "timestamp");
    json_t *ptype_obj = json_object_get(root, "e_dtype");
    json_t *params_obj = json_object_get(root, "e_params");
    
    if (type_obj && json_is_integer(type_obj)) {
        event.type = json_integer_value(type_obj);
    }
    if (subtype_obj && json_is_integer(subtype_obj)) {
        event.subtype = json_integer_value(subtype_obj);
    }
    if (timestamp_obj && json_is_integer(timestamp_obj)) {
        event.timestamp = json_integer_value(timestamp_obj);
    }
    if (ptype_obj && json_is_integer(ptype_obj)) {
        event.ptype = json_integer_value(ptype_obj);
    }
    if (params_obj) {
        if (json_is_string(params_obj)) {
            const char* param_str = json_string_value(params_obj);
            if (param_str) {
                event.params = param_str;
            }
        } else if (json_is_array(params_obj)) {
            // Convert array to string representation
            char *params_str = json_dumps(params_obj, JSON_COMPACT);
            if (params_str) {
                event.params = params_str;
                free(params_str);
            }
        } else {
            // For other JSON types, convert to string
            char *params_str = json_dumps(params_obj, JSON_COMPACT);
            if (params_str) {
                event.params = params_str;
                free(params_str);
            }
        }
    }
    
    // Handle special event types for name mapping
    if (event.type == EVT_NAMESET) {
        if (event.subtype < m_eventTypeNames.size() && !event.params.empty()) {
            m_eventTypeNames[event.subtype] = event.params;
        }
        json_decref(root);
        return;
    }
    
    if (event.type == EVT_SUBTYPE_NAMES) {
        // Parse subtype names from params
        // Format should be space-separated pairs: "name1 value1 name2 value2..."
        std::istringstream iss(event.params);
        std::string name, value;
        while (iss >> name >> value) {
            std::string key = std::to_string(event.subtype) + ":" + value;
            m_eventSubtypeNames[key] = name;
        }
        json_decref(root);
        return;
    }
    
    processEvent(event);
    json_decref(root);
}

void EssEventTable::showObservation(int index) {
    if (index < 0 || index >= (int)m_observationHistory.size()) {
        return;
    }
    
    m_currentObsIndex = index;
    const auto& obs = m_observationHistory[index];
    
    // Count displayable events
    int displayCount = 0;
    for (const auto& event : obs.events) {
        if (shouldDisplayEvent(event)) {
            displayCount++;
        }
    }
    
    rows(displayCount);
    redraw();
}

void EssEventTable::clearEvents() {
    m_observationHistory.clear();
    m_currentObsIndex = -1;
    rows(0);
    redraw();
}

void EssEventTable::onObservationReset() {
    m_currentObsStart = 0;
    // Don't clear history, just stop recording
}

// EssEventTableWidget implementation
EssEventTableWidget::EssEventTableWidget(int X, int Y, int W, int H, const char *L)
    : Fl_Group(X, Y, W, H, L)
{
    setupUI();
}

EssEventTableWidget::~EssEventTableWidget() {
}

void EssEventTableWidget::setupUI() {
    int statusHeight = 30;
    int buttonHeight = 25;
    int spacing = 5;
    
    // Status bar at top
    m_statusLabel = new Fl_Box(x() + 5, y() + 5, 120, statusHeight - 10, "System: Stopped");
    m_statusLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    
    m_obsLabel = new Fl_Box(x() + 130, y() + 5, 120, statusHeight - 10, "No observation");
    m_obsLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    
    // Navigation controls
    int navY = y() + 5;
    int navX = x() + w() - 200;
    
    m_prevObsButton = new Fl_Button(navX, navY, 25, buttonHeight, "<");
    m_prevObsButton->callback(prevObsCallback, this);
    
    m_obsNavigationLabel = new Fl_Box(navX + 30, navY, 50, buttonHeight, "");
    m_obsNavigationLabel->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    
    m_nextObsButton = new Fl_Button(navX + 85, navY, 25, buttonHeight, ">");
    m_nextObsButton->callback(nextObsCallback, this);
    
    m_clearButton = new Fl_Button(navX + 120, navY, 70, buttonHeight, "Clear All");
    m_clearButton->callback(clearCallback, this);
    
    // Event table
    m_table = new EssEventTable(x() + 5, y() + statusHeight + spacing, 
                               w() - 10, h() - statusHeight - spacing - 5);
    
    updateNavigationControls();
    
    end();
}

void EssEventTableWidget::updateNavigationControls() {
    bool hasObs = m_table->hasObservations();
    int currentIndex = m_table->getCurrentObsIndex();
    int obsCount = m_table->getObservationCount();
    
    m_prevObsButton->deactivate();
    m_nextObsButton->deactivate();
    
    if (hasObs) {
        if (currentIndex > 0) {
            m_prevObsButton->activate();
        }
        if (currentIndex < obsCount - 1) {
            m_nextObsButton->activate();
        }
        
        std::string navText = std::to_string(currentIndex + 1) + "/" + std::to_string(obsCount);
        m_obsNavigationLabel->copy_label(navText.c_str());
    } else {
        m_obsNavigationLabel->label("");
    }
    
    redraw();
}

void EssEventTableWidget::updateObservationLabel() {
    // This would be updated based on observation info from events
    // For now, keep it simple
}

void EssEventTableWidget::processEventlogData(const std::string &jsonData) {
    m_table->processEventlogData(jsonData);
    updateNavigationControls();
}

void EssEventTableWidget::onSystemStateChanged(bool running) {
    if (running) {
        m_statusLabel->copy_label("System: Running");
        m_statusLabel->labelcolor(FL_GREEN);
    } else {
        m_statusLabel->copy_label("System: Stopped");
        m_statusLabel->labelcolor(FL_RED);
        if (m_table->getCurrentObsIndex() >= 0) {
            m_obsLabel->label("");
        }
    }
    m_statusLabel->redraw();
}

void EssEventTableWidget::onExperimentStateChanged(const std::string &newstate) {
    std::string label = "System: " + newstate;
    m_statusLabel->copy_label(label.c_str());
    
    if (newstate == "Stopped") {
        m_statusLabel->labelcolor(FL_RED);
        m_obsLabel->label("");
    }
    m_statusLabel->redraw();
}

void EssEventTableWidget::onHostConnected() {
    m_table->clearEvents();
    m_obsLabel->label("No observation");
    updateNavigationControls();
    m_statusLabel->copy_label("System: Stopped");
    m_statusLabel->labelcolor(FL_RED);
}

void EssEventTableWidget::onHostDisconnected() {
    m_table->clearEvents();
    m_obsLabel->label("No observation");
    updateNavigationControls();
    m_statusLabel->copy_label("System: Disconnected");
    m_statusLabel->labelcolor(FL_BLACK);
}

void EssEventTableWidget::onClearClicked() {
    m_table->clearEvents();
    updateNavigationControls();
}

// Static callback functions
void EssEventTableWidget::prevObsCallback(Fl_Widget *w, void *data) {
    EssEventTableWidget *widget = static_cast<EssEventTableWidget*>(data);
    int currentIndex = widget->m_table->getCurrentObsIndex();
    if (currentIndex > 0) {
        widget->m_table->showObservation(currentIndex - 1);
        widget->updateNavigationControls();
    }
}

void EssEventTableWidget::nextObsCallback(Fl_Widget *w, void *data) {
    EssEventTableWidget *widget = static_cast<EssEventTableWidget*>(data);
    int currentIndex = widget->m_table->getCurrentObsIndex();
    int obsCount = widget->m_table->getObservationCount();
    if (currentIndex < obsCount - 1) {
        widget->m_table->showObservation(currentIndex + 1);
        widget->updateNavigationControls();
    }
}

void EssEventTableWidget::clearCallback(Fl_Widget *w, void *data) {
    EssEventTableWidget *widget = static_cast<EssEventTableWidget*>(data);
    widget->onClearClicked();
}
