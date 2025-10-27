#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Pack.H>
#include <FL/Enumerations.H>
#include <string>

class EssguiFileDialog {
private:
    Fl_Window* window;
    Fl_Input* filename_input;
    Fl_Pack* button_pack;    
    Fl_Return_Button* ok_button;
    Fl_Button* cancel_button;
    Fl_Button* suggest_button;
    
    std::string selected_file;
    int dialog_result; // 0=cancel, 1=ok
    
    // Function pointer for suggest callback
    void (*suggest_callback_func)(EssguiFileDialog* dialog, void* data);
    void* suggest_callback_data;
    
    static void ok_callback(Fl_Widget*, void* data) {
        EssguiFileDialog* dialog = (EssguiFileDialog*)data;
        dialog->dialog_result = 1;
        dialog->window->hide();
    }
    
    static void cancel_callback(Fl_Widget*, void* data) {
        EssguiFileDialog* dialog = (EssguiFileDialog*)data;
        dialog->dialog_result = 0;
        dialog->window->hide();
    }
    
    static void suggest_callback(Fl_Widget*, void* data) {
        EssguiFileDialog* dialog = (EssguiFileDialog*)data;
        
        // Call user-defined suggest function if set
        if (dialog->suggest_callback_func) {
            dialog->suggest_callback_func(dialog,
					  dialog->suggest_callback_data);
        } 
    }
    
    static void input_callback(Fl_Widget*, void* data) {
        EssguiFileDialog* dialog = (EssguiFileDialog*)data;
        // Enter key in input field acts like OK
        dialog->dialog_result = 1;
        dialog->window->hide();
    }

public:
    EssguiFileDialog(const char* title = "Open Remote File", const char* initial_path = "") {
        // Initialize callback pointers
        suggest_callback_func = nullptr;
        suggest_callback_data = nullptr;
        
        // Create window
        window = new Fl_Window(400, 120, title);
        window->set_modal();
        
        // Filename input
        filename_input = new Fl_Input(80, 20, 300, 25, "File Path:");
        filename_input->callback(input_callback, this);
        filename_input->when(FL_WHEN_ENTER_KEY);
        if (initial_path && strlen(initial_path) > 0) {
            filename_input->value(initial_path);
        }

	// Create horizontal pack for buttons (centered)
        int button_pack_width = 252; // 80 + 80 + 80 + 2*6 spacing
        int button_pack_x = (400 - button_pack_width) / 2; // Center in 400px window
        button_pack = new Fl_Pack(button_pack_x, 60, button_pack_width, 25);
        button_pack->type(FL_HORIZONTAL);
        button_pack->spacing(6);
        
        // Buttons
        ok_button = new Fl_Return_Button(80, 60, 80, 25, "OK");
        ok_button->callback(ok_callback, this);
        
        cancel_button = new Fl_Button(150, 60, 80, 25, "Cancel");
        cancel_button->callback(cancel_callback, this);
        
        suggest_button = new Fl_Button(220, 60, 80, 25, "Suggest");
        suggest_button->callback(suggest_callback, this);
        suggest_button->when(FL_WHEN_RELEASE);

	button_pack->end();
        window->end();
        
        dialog_result = 0;
    }
    
    ~EssguiFileDialog() {
        delete window;
    }
    
    // Show dialog and return result
    // Returns: 0=cancel, 1=ok
    int show() {
        window->show();
        
        // Run modal loop
        while (window->shown()) {
            Fl::wait();
        }
        
        // Get selected filename if OK was pressed
        if (dialog_result == 1) {
            const char* input_text = filename_input->value();
            if (input_text && strlen(input_text) > 0) {
                selected_file = input_text;
            } else {
                selected_file = "";
            }
        }
        
        return dialog_result;
    }
    
    // Get the selected filename (valid after show() returns 1)
    const char* filename() const {
        return selected_file.c_str();
    }
    
    // Set a suggested filename
    void set_suggested_filename(const char* filename) {
        filename_input->value(filename);
        filename_input->redraw();
        window->redraw();
    }
    
    // Get current input value (useful for suggest callback)
    const char* get_input_text() const {
        return filename_input->value();
    }
    
    // Set the input field value programmatically
    void set_filename(const char* filename) {
        filename_input->value(filename);
        filename_input->redraw();
        window->redraw();
    }
    
    // Set callback for suggest button
    void set_suggest_callback(void (*callback)(EssguiFileDialog* dialog, void* data), void* data = nullptr) {
        printf("Setting suggest callback: %p\n", callback);
        suggest_callback_func = callback;
        suggest_callback_data = data;
    }
    
    // Clear the input field
    void clear_filename() {
        filename_input->value("");
    }
};
