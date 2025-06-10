#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Spinner.H>

class Wheel_Spinner : public Fl_Spinner {
public:
    Wheel_Spinner(int x, int y, int w, int h, const char* label = 0) 
        : Fl_Spinner(x, y, w, h, label) {}
    
    int handle(int event) override {
        switch (event) {
            case FL_MOUSEWHEEL: {
                // Debug: Check what modifiers are detected
                int state = Fl::event_state();
                
                bool ctrl_held = (state & FL_CTRL) || Fl::event_ctrl();
                bool alt_held = (state & FL_ALT) || Fl::event_alt();
                
                // Calculate step multiplier based on modifiers
                double multiplier = 1.0;
		
                if (alt_held) multiplier = 10.0;
                if (ctrl_held) multiplier = 0.1;  // Fine adjustment with Ctrl
                
                double current = value();
                double step_size = step() * multiplier;
                
                if (Fl::event_dy() > 0) {
                    // Wheel up - increment
                    double new_val = current + step_size;
                    if (new_val <= maximum()) {
                        value(new_val);
                        do_callback();  // Trigger callback if needed
                    }
                } else if (Fl::event_dy() < 0) {
                    // Wheel down - decrement
                    double new_val = current - step_size;
                    if (new_val >= minimum()) {
                        value(new_val);
                        do_callback();  // Trigger callback if needed
                    }
                }
                return 1;  // Event handled
            }
            default:
                return Fl_Spinner::handle(event);  // Let parent handle other events
        }
    }
};
