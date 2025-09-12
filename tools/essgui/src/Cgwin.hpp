#ifndef CGWIN_HPP
#define CGWIN_HPP

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Group.H>

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <jansson.h>

class CGWin : public Fl_Group {
private:
    std::string m_lastJsonData;
    float m_currentPosX = 0;
    float m_currentPosY = 0;
    int m_currentColor = 0;
    int m_textOrientation = 0;
    int m_textJustification = 0;  // -1=left, 0=center, 1=right
    std::string m_currentFont = "Helvetica";
    int m_currentFontSize = 10;
    int m_lineWidth = 1;
    Fl_Color m_backgroundColor = FL_WHITE;
    
    // Window bounds for coordinate transformation
    float m_windowLLX = 0, m_windowLLY = 0;
    float m_windowURX = 640, m_windowURY = 480;
    float m_scaleX = 1.0, m_scaleY = 1.0;
	json_t *m_cachedCommands = nullptr;  // Cache parsed commands
	
public:
    CGWin(int X, int Y, int W, int H, const char*L=0) : Fl_Group(X,Y,W,H,L) {
        color(FL_WHITE);
        end();  // Important for Fl_Group
    }

   void dumpCachedCommands() const {
        if (!m_cachedCommands) {
            std::cout << "No cached commands to dump" << std::endl;
            return;
        }
        
        std::cout << "=== DUMPING CACHED COMMANDS ===" << std::endl;
        std::cout << "Total commands: " << json_array_size(m_cachedCommands) << std::endl;
        std::cout << "Widget size: " << w() << "x" << h() << std::endl;
        std::cout << "Window bounds: (" << m_windowLLX << "," << m_windowLLY << ") to (" 
                  << m_windowURX << "," << m_windowURY << ")" << std::endl;
        std::cout << "Scale factors: " << m_scaleX << "x, " << m_scaleY << "y" << std::endl;
        std::cout << "Background color index: " << m_backgroundColor << std::endl;
        std::cout << std::endl;
        
        size_t index;
        json_t *command;
        
        json_array_foreach(m_cachedCommands, index, command) {
            json_t *cmd_obj = json_object_get(command, "cmd");
            json_t *args_obj = json_object_get(command, "args");
            
            if (json_is_string(cmd_obj) && json_is_array(args_obj)) {
                const char *cmd = json_string_value(cmd_obj);
                std::cout << "[" << index << "] " << cmd << "(";
                
                // Print arguments with types
                for (size_t i = 0; i < json_array_size(args_obj); i++) {
                    json_t *arg = json_array_get(args_obj, i);
                    if (json_is_string(arg)) {
                        std::cout << "\"" << json_string_value(arg) << "\"";
                    } else if (json_is_real(arg)) {
                        std::cout << json_real_value(arg);
                    } else if (json_is_integer(arg)) {
                        std::cout << json_integer_value(arg);
                    } else {
                        std::cout << "unknown_type";
                    }
                    if (i < json_array_size(args_obj) - 1) std::cout << ", ";
                }
                std::cout << ")";
                
                // Add coordinate transformation preview for drawing commands
                if (strcmp(cmd, "line") == 0 && json_array_size(args_obj) >= 4) {
                    float x1 = transformX(json_number_value(json_array_get(args_obj, 0)));
                    float y1 = transformY(json_number_value(json_array_get(args_obj, 1)));
                    float x2 = transformX(json_number_value(json_array_get(args_obj, 2)));
                    float y2 = transformY(json_number_value(json_array_get(args_obj, 3)));
                    std::cout << " -> screen coords: (" << x1 << "," << y1 << ") to (" << x2 << "," << y2 << ")";
                }
                else if (strcmp(cmd, "moveto") == 0 && json_array_size(args_obj) >= 2) {
                    float x = transformX(json_number_value(json_array_get(args_obj, 0)));
                    float y = transformY(json_number_value(json_array_get(args_obj, 1)));
                    std::cout << " -> screen coords: (" << x << "," << y << ")";
                }
                
                std::cout << std::endl;
            } else {
                std::cout << "[" << index << "] MALFORMED COMMAND" << std::endl;
            }
        }
        std::cout << "=== END DUMP ===" << std::endl;
    }


    void processGraphicsData(const std::string& jsonData) {
        m_lastJsonData = jsonData;
        
        // Free previous cached commands
        if (m_cachedCommands) {
            json_decref(m_cachedCommands);
        }
        
        json_error_t error;
        json_t *root = json_loads(jsonData.c_str(), 0, &error);
        
        if (!root) {
            std::cerr << "JSON parse error: " << error.text << std::endl;
            return;
        }
        
        // Cache the commands for drawing
        json_t *commands = json_object_get(root, "commands");
        if (json_is_array(commands)) {
            m_cachedCommands = json_incref(commands);  // Keep reference
        }
        
        json_decref(root);

	if (m_cachedCommands) {
	  extractPersistentSettings(m_cachedCommands);
	}
    	
        redraw();  // Trigger FLTK redraw
    }

  virtual void draw() FL_OVERRIDE {
    
    // Clear background
    fl_color(m_backgroundColor);
    fl_rectf(x(), y(), w(), h());
    
    if (m_cachedCommands) {
      //dumpCachedCommands();
    }
    
    // Now execute drawing commands in proper FLTK drawing context
    if (m_cachedCommands) {
      executeJsonCommands(m_cachedCommands);
    }
    
    // Draw any child widgets
    Fl_Group::draw();
  }
    
     ~CGWin() {
        if (m_cachedCommands) {
            json_decref(m_cachedCommands);
        }
    }

private:
  void extractPersistentSettings(json_t *commands) {
    size_t index;
    json_t *command;
    
    json_array_foreach(commands, index, command) {
      json_t *cmd_obj = json_object_get(command, "cmd");
      json_t *args_obj = json_object_get(command, "args");
      
      if (json_is_string(cmd_obj) && json_is_array(args_obj)) {
	const char *cmd = json_string_value(cmd_obj);
        
	if (strcmp(cmd, "setbackground") == 0 && json_array_size(args_obj) >= 1) {
	  m_backgroundColor = cgraphColorToFL(json_integer_value(json_array_get(args_obj, 0)));
	  break; // Found it, can stop looking
	}
      }
    }
  }
  
  void executeJsonCommands(json_t *commands) {
        size_t index;
        json_t *command;
        
        json_array_foreach(commands, index, command) {
            executeJsonCommand(command);
        }
    }

    void executeJsonCommand(json_t *command) {
        json_t *cmd_obj = json_object_get(command, "cmd");
        json_t *args_obj = json_object_get(command, "args");
        
        if (!json_is_string(cmd_obj) || !json_is_array(args_obj)) {
            return;
        }
        
        const char *cmd = json_string_value(cmd_obj);
        
        if (strcmp(cmd, "setwindow") == 0 && json_array_size(args_obj) >= 4) {
            m_windowLLX = json_number_value(json_array_get(args_obj, 0));
            m_windowLLY = json_number_value(json_array_get(args_obj, 1));
            m_windowURX = json_number_value(json_array_get(args_obj, 2));
            m_windowURY = json_number_value(json_array_get(args_obj, 3));
            
            // Calculate scaling
            float sourceWidth = m_windowURX - m_windowLLX;
            float sourceHeight = m_windowURY - m_windowLLY;
            if (sourceWidth > 0 && sourceHeight > 0) {
                m_scaleX = w() / sourceWidth;
                m_scaleY = h() / sourceHeight;
            }
        }
        else if (strcmp(cmd, "setcolor") == 0 && json_array_size(args_obj) >= 1) {
            m_currentColor = json_integer_value(json_array_get(args_obj, 0));
            fl_color(cgraphColorToFL(m_currentColor));
        }
        else if (strcmp(cmd, "setbackground") == 0 && json_array_size(args_obj) >= 1) {
            m_backgroundColor = cgraphColorToFL(json_integer_value(json_array_get(args_obj, 0)));
        }
        else if (strcmp(cmd, "setfont") == 0 && json_array_size(args_obj) >= 2) {
            const char *fontName = json_string_value(json_array_get(args_obj, 0));
            if (fontName) {
                m_currentFont = fontName;
            }
            m_currentFontSize = static_cast<int>(json_number_value(json_array_get(args_obj, 1)) * std::min(m_scaleX, m_scaleY));
            fl_font(getFLFont(m_currentFont), m_currentFontSize);
        }
        else if (strcmp(cmd, "setjust") == 0 && json_array_size(args_obj) >= 1) {
            m_textJustification = json_integer_value(json_array_get(args_obj, 0));
        }
        else if (strcmp(cmd, "setorientation") == 0 && json_array_size(args_obj) >= 1) {
            m_textOrientation = json_integer_value(json_array_get(args_obj, 0));
        }
        else if (strcmp(cmd, "setlwidth") == 0 && json_array_size(args_obj) >= 1) {
            m_lineWidth = std::max(1, static_cast<int>(json_integer_value(json_array_get(args_obj, 0))) / 100);
            fl_line_style(FL_SOLID, m_lineWidth);
        }
        else if (strcmp(cmd, "gsave") == 0) {
            fl_push_matrix();
        }
        else if (strcmp(cmd, "grestore") == 0) {
            fl_pop_matrix();
            fl_color(cgraphColorToFL(m_currentColor));
            fl_font(getFLFont(m_currentFont), m_currentFontSize);
            fl_line_style(FL_SOLID, m_lineWidth);
        }
        else if (strcmp(cmd, "circle") == 0 && json_array_size(args_obj) >= 3) {
            float cx = transformX(json_number_value(json_array_get(args_obj, 0)));
            float cy = transformY(json_number_value(json_array_get(args_obj, 1)));
            float radius = transformWidth(json_number_value(json_array_get(args_obj, 2)) / 2);
            
            fl_arc(cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
        }
        else if (strcmp(cmd, "fcircle") == 0 && json_array_size(args_obj) >= 3) {
            float cx = transformX(json_number_value(json_array_get(args_obj, 0)));
            float cy = transformY(json_number_value(json_array_get(args_obj, 1)));
            float radius = transformWidth(json_number_value(json_array_get(args_obj, 2)) / 2);
            
            fl_pie(cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
        }
        else if (strcmp(cmd, "line") == 0 && json_array_size(args_obj) >= 4) {
            float x1 = transformX(json_number_value(json_array_get(args_obj, 0)));
            float y1 = transformY(json_number_value(json_array_get(args_obj, 1)));
            float x2 = transformX(json_number_value(json_array_get(args_obj, 2)));
            float y2 = transformY(json_number_value(json_array_get(args_obj, 3)));
            
            fl_line(x1, y1, x2, y2);
        }
        else if (strcmp(cmd, "moveto") == 0 && json_array_size(args_obj) >= 2) {
            m_currentPosX = json_number_value(json_array_get(args_obj, 0));
            m_currentPosY = json_number_value(json_array_get(args_obj, 1));
        }
        else if (strcmp(cmd, "lineto") == 0 && json_array_size(args_obj) >= 2) {
            float x1 = transformX(m_currentPosX);
            float y1 = transformY(m_currentPosY);
            float x2 = transformX(json_number_value(json_array_get(args_obj, 0)));
            float y2 = transformY(json_number_value(json_array_get(args_obj, 1)));
            
            fl_line(x1, y1, x2, y2);
            m_currentPosX = json_number_value(json_array_get(args_obj, 0));
            m_currentPosY = json_number_value(json_array_get(args_obj, 1));
        }
		else if (strcmp(cmd, "filledrect") == 0 && json_array_size(args_obj) >= 4) {  // Add this handler
			float x1 = transformX(json_number_value(json_array_get(args_obj, 0)));
			float y1 = transformY(json_number_value(json_array_get(args_obj, 1)));
			float x2 = transformX(json_number_value(json_array_get(args_obj, 2)));
			float y2 = transformY(json_number_value(json_array_get(args_obj, 3)));
			
			fl_rectf(std::min(x1, x2), std::min(y1, y2), 
					std::abs(x2 - x1), std::abs(y2 - y1));
		}
        else if (strcmp(cmd, "poly") == 0) {
            fl_begin_line();
            for (size_t i = 0; i < json_array_size(args_obj); i += 2) {
                if (i + 1 < json_array_size(args_obj)) {
                    float x = transformX(json_number_value(json_array_get(args_obj, i)));
                    float y = transformY(json_number_value(json_array_get(args_obj, i + 1)));
                    fl_vertex(x, y);
                }
            }
            fl_end_line();
        }
        else if (strcmp(cmd, "fpoly") == 0) {
            fl_begin_polygon();
            for (size_t i = 0; i < json_array_size(args_obj); i += 2) {
                if (i + 1 < json_array_size(args_obj)) {
                    float x = transformX(json_number_value(json_array_get(args_obj, i)));
                    float y = transformY(json_number_value(json_array_get(args_obj, i + 1)));
                    fl_vertex(x, y);
                }
            }
            fl_end_polygon();
        }
        else if (strcmp(cmd, "drawtext") == 0 && json_array_size(args_obj) >= 1) {
            const char *text = json_string_value(json_array_get(args_obj, 0));
            if (!text) return;
            
            float tx = transformX(m_currentPosX);
            float ty = transformY(m_currentPosY);
            
            // Handle text orientation and justification
            fl_push_matrix();
            
            if (m_textOrientation != 0) {
                fl_translate(tx, ty);
                fl_rotate(-m_textOrientation * 90.0);
                tx = 0;
                ty = 0;
            }
            
            // Adjust position based on justification
            int textWidth = 0, textHeight = 0;
            fl_measure(text, textWidth, textHeight);
            
            switch (m_textJustification) {
                case -1: // left
                    break;
                case 0:  // center
                    tx -= textWidth / 2.0;
                    break;
                case 1:  // right
                    tx -= textWidth;
                    break;
            }
            fl_color(cgraphColorToFL(m_currentColor));
            fl_draw(text, tx, ty);
            fl_pop_matrix();
        }
        else if (strcmp(cmd, "point") == 0 && json_array_size(args_obj) >= 2) {
            float x = transformX(json_number_value(json_array_get(args_obj, 0)));
            float y = transformY(json_number_value(json_array_get(args_obj, 1)));
            fl_point(x, y);
        }
    }

    // Transform helpers (same as before)
    float transformX(float xx) const { return x() + (xx * m_scaleX); }
    float transformY(float yy) const { return y() + (h() - (yy * m_scaleY)); }
    float transformWidth(float w) const { return w * m_scaleX; }
    float transformHeight(float h) const { return h * m_scaleY; }

    // Color and font helpers (same as before)
	Fl_Color cgraphColorToFL(int colorIndex) {
		static const Fl_Color colors[] = {
			FL_BLACK, FL_BLUE, FL_DARK_GREEN, FL_CYAN, FL_RED,
			FL_MAGENTA, FL_DARK_YELLOW, FL_WHITE, FL_GRAY, FL_DARK_BLUE,
			FL_GREEN, FL_DARK_CYAN, FL_DARK_RED, FL_DARK_MAGENTA, FL_YELLOW,
		};
	
		if (colorIndex >= 0 && colorIndex < 15) {
			return colors[colorIndex];
		}
	
		// Handle RGB colors (these are the big numbers you're seeing)
		if (colorIndex > 31) {  // Changed from 18 to catch more RGB colors
			unsigned int shifted = colorIndex >> 5;
			int r = (shifted & 0xff0000) >> 16;
			int g = (shifted & 0xff00) >> 8;
			int b = (shifted & 0xff);
			
			return fl_rgb_color(r, g, b);
		}
	
		return FL_BLACK;
	}

    Fl_Font getFLFont(const std::string& fontName) {
        if (fontName == "TIMES") return FL_TIMES;
        if (fontName == "COURIER") return FL_COURIER;
        if (fontName == "SCREEN") return FL_SCREEN;
        if (fontName == "SYMBOL") return FL_SYMBOL;
        if (fontName == "ZAPF") return FL_ZAPF_DINGBATS;
        return FL_HELVETICA;
    }
};

#endif
