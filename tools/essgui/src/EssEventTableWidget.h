// EssEventTableWidget.h
#pragma once

#include <FL/Fl_Widget.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <cstdint>
#include <vector>
#include <string>
#include <map>

// Event structure matching the JSON format from eventlog/events
struct EssEvent {
    uint8_t type;
    uint8_t subtype;
    uint64_t timestamp;
    uint8_t ptype;  // parameter type
    std::string params;
    
    EssEvent() : type(0), subtype(0), timestamp(0), ptype(0) {}
};

// Event type constants (from Qt6 version)
enum EventType {
    EVT_TRACE = 0,
    EVT_NAMESET = 1,
    EVT_FILEIO = 2,
    EVT_USER = 3,
    EVT_PARAM = 4,
    EVT_SYSTEM_CHANGES = 18,
    EVT_BEGINOBS = 19,
    EVT_ENDOBS = 20,
    EVT_SUBTYPE_NAMES = 6,
    EVT_STATE_DEBUG = 7
};

class EssEventTable : public Fl_Table_Row {
private:
    struct ObservationData {
        uint64_t startTime;
        int obsCount;
        int obsTotal;
        std::vector<EssEvent> events;
    };
    
    std::vector<ObservationData> m_observationHistory;
    int m_currentObsIndex;
    uint64_t m_currentObsStart;
    int m_obsCount;
    int m_obsTotal;
    int m_maxEvents;
    
    // Event type/subtype name mappings
    std::vector<std::string> m_eventTypeNames;
    std::map<std::string, std::string> m_eventSubtypeNames;
    
    void initializeEventNames();
    bool shouldDisplayEvent(const EssEvent &event) const;
    std::string formatEventParams(const EssEvent &event) const;
    std::string getEventTypeName(uint8_t type) const;
    std::string getEventSubtypeName(uint8_t type, uint8_t subtype) const;
    void addEventToCurrentObs(const EssEvent &event);
    
protected:
    void draw_cell(TableContext context, int ROW, int COL, int X, int Y, int W, int H) override;
    
public:
    EssEventTable(int X, int Y, int W, int H, const char *L = 0);
    virtual ~EssEventTable();
    
    void processEvent(const EssEvent &event);
    void processEventlogData(const std::string &jsonData);
    void showObservation(int index);
    void clearEvents();
    void onObservationReset();
    
    int getCurrentObsIndex() const { return m_currentObsIndex; }
    int getObservationCount() const { return m_observationHistory.size(); }
    bool hasObservations() const { return !m_observationHistory.empty(); }
};

class EssEventTableWidget : public Fl_Group {
private:
    EssEventTable *m_table;
    Fl_Box *m_statusLabel;
    Fl_Box *m_obsLabel;
    Fl_Button *m_prevObsButton;
    Fl_Button *m_nextObsButton;
    Fl_Box *m_obsNavigationLabel;
    Fl_Button *m_clearButton;
    
    void setupUI();
    void updateNavigationControls();
    void updateObservationLabel();
    
    static void prevObsCallback(Fl_Widget *w, void *data);
    static void nextObsCallback(Fl_Widget *w, void *data);
    static void clearCallback(Fl_Widget *w, void *data);
    
public:
    EssEventTableWidget(int X, int Y, int W, int H, const char *L = 0);
    virtual ~EssEventTableWidget();
    
    void processEventlogData(const std::string &jsonData);
    void onSystemStateChanged(bool running);
    void onExperimentStateChanged(const std::string &newstate);
    void onHostConnected();
    void onHostDisconnected();
    void onClearClicked();
};
