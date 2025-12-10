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

// Local base64 decode for image data
static int decodeBase64(const char* in, size_t inLen, 
                        unsigned char* out, size_t* outLen) {
    static const int lookup[] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    
    size_t len = 0;
    int buf = 0, bits = 0;
    
    for (size_t i = 0; i < inLen; i++) {
        unsigned char c = in[i];
        if (c == '=') break;
        if (c >= 128 || lookup[c] < 0) continue;  // skip invalid/whitespace
        
        buf = (buf << 6) | lookup[c];
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            out[len++] = (buf >> bits) & 0xFF;
        }
    }
    
    *outLen = len;
    return 0;
}

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

    // Nearest-neighbor image scaling
    void scaleImageNearest(const unsigned char* src, int src_w, int src_h, int depth,
                           unsigned char* dst, int dst_w, int dst_h) {
        for (int y = 0; y < dst_h; y++) {
            int src_y = y * src_h / dst_h;
            for (int x = 0; x < dst_w; x++) {
                int src_x = x * src_w / dst_w;
                const unsigned char* sp = src + (src_y * src_w + src_x) * depth;
                unsigned char* dp = dst + (y * dst_w + x) * depth;
                for (int c = 0; c < depth; c++) {
                    dp[c] = sp[c];
                }
            }
        }
    }

    // Bilinear image scaling
    void scaleImageBilinear(const unsigned char* src, int src_w, int src_h, int depth,
                            unsigned char* dst, int dst_w, int dst_h) {
        for (int y = 0; y < dst_h; y++) {
            float src_yf = (y + 0.5f) * src_h / dst_h - 0.5f;
            int y0 = static_cast<int>(std::floor(src_yf));
            int y1 = y0 + 1;
            float fy = src_yf - y0;
            
            // Clamp to valid range
            if (y0 < 0) { y0 = 0; fy = 0; }
            if (y1 >= src_h) { y1 = src_h - 1; }
            
            for (int x = 0; x < dst_w; x++) {
                float src_xf = (x + 0.5f) * src_w / dst_w - 0.5f;
                int x0 = static_cast<int>(std::floor(src_xf));
                int x1 = x0 + 1;
                float fx = src_xf - x0;
                
                // Clamp to valid range
                if (x0 < 0) { x0 = 0; fx = 0; }
                if (x1 >= src_w) { x1 = src_w - 1; }
                
                // Get four neighboring pixels
                const unsigned char* p00 = src + (y0 * src_w + x0) * depth;
                const unsigned char* p01 = src + (y0 * src_w + x1) * depth;
                const unsigned char* p10 = src + (y1 * src_w + x0) * depth;
                const unsigned char* p11 = src + (y1 * src_w + x1) * depth;
                
                unsigned char* dp = dst + (y * dst_w + x) * depth;
                
                for (int c = 0; c < depth; c++) {
                    // Bilinear interpolation
                    float top = p00[c] * (1 - fx) + p01[c] * fx;
                    float bot = p10[c] * (1 - fx) + p11[c] * fx;
                    float val = top * (1 - fy) + bot * fy;
                    dp[c] = static_cast<unsigned char>(std::min(255.0f, std::max(0.0f, val)));
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
            float radius =
	      transformWidth(json_number_value(json_array_get(args_obj, 2)));
            
            fl_arc(cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
        }
        else if (strcmp(cmd, "fcircle") == 0 && json_array_size(args_obj) >= 3) {
            float cx = transformX(json_number_value(json_array_get(args_obj, 0)));
            float cy = transformY(json_number_value(json_array_get(args_obj, 1)));
            float radius =
	      transformWidth(json_number_value(json_array_get(args_obj, 2)));
            
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
        else if (strcmp(cmd, "drawimage") == 0) {
            // Get image_data object
            json_t *img_data = json_object_get(command, "image_data");
            if (!img_data) return;
            
            int src_w = json_integer_value(json_object_get(img_data, "width"));
            int src_h = json_integer_value(json_object_get(img_data, "height"));
            int depth = json_integer_value(json_object_get(img_data, "depth"));
            json_t *data_obj = json_object_get(img_data, "data");
            if (!data_obj || !json_is_string(data_obj)) return;
            const char *b64_data = json_string_value(data_obj);
            
            // Get args: [x1, y1, x2, y2, image_id, (optional) interp] - corner coordinates
            if (json_array_size(args_obj) < 4) return;
            float x1 = json_number_value(json_array_get(args_obj, 0));
            float y1 = json_number_value(json_array_get(args_obj, 1));
            float x2 = json_number_value(json_array_get(args_obj, 2));
            float y2 = json_number_value(json_array_get(args_obj, 3));
            // args[4] is image_id; args[5] would be interp if added later
            int interp = (json_array_size(args_obj) >= 6) ? 
                         json_integer_value(json_array_get(args_obj, 5)) : 1;  // default to bilinear
            
            // Transform corner coordinates
            int screen_x = static_cast<int>(transformX(x1));
            int screen_y = static_cast<int>(transformY(y2));  // y2 is top in screen coords
            int screen_w = static_cast<int>(transformX(x2) - transformX(x1));
            int screen_h = static_cast<int>(transformY(y1) - transformY(y2));
            
            if (src_w <= 0 || src_h <= 0 || depth <= 0) return;
            if (screen_w <= 0 || screen_h <= 0) return;
            
            // Decode base64
            size_t expected_size = src_w * src_h * depth;
            std::vector<unsigned char> pixels(expected_size);
            size_t out_len = 0;
            decodeBase64(b64_data, strlen(b64_data), pixels.data(), &out_len);
            
            if (out_len != expected_size) {
                std::cerr << "drawimage: decoded size mismatch (got " 
                          << out_len << ", expected " << expected_size << ")" << std::endl;
                return;
            }
            
            // Scale if needed
            if (screen_w == src_w && screen_h == src_h) {
                // No scaling needed
                fl_draw_image(pixels.data(), screen_x, screen_y, src_w, src_h, depth);
            } else {
                // Scale to destination size
                std::vector<unsigned char> scaled(screen_w * screen_h * depth);
                if (interp == 0) {
                    // Nearest-neighbor
                    scaleImageNearest(pixels.data(), src_w, src_h, depth, 
                                      scaled.data(), screen_w, screen_h);
                } else {
                    // Bilinear (default)
                    scaleImageBilinear(pixels.data(), src_w, src_h, depth, 
                                       scaled.data(), screen_w, screen_h);
                }
                fl_draw_image(scaled.data(), screen_x, screen_y, screen_w, screen_h, depth);
            }
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