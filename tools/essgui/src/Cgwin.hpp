#ifndef CGWIN_HPP
#define CGWIN_HPP

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>

#include <iostream>
#include <vector>
#include <sstream>
#include <string>
#include <cmath>

class CGWin : public Fl_Box {
private:
    std::string m_lastCommands;
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

public:
    CGWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L) {
        // Initialize with white background
        color(FL_WHITE);
    }

    void processDrawingCommands(const std::string& commands) {
        m_lastCommands = commands;
        redraw();
    }

    virtual void draw() FL_OVERRIDE {
        // Clear background
        fl_color(m_backgroundColor);
        fl_rectf(x(), y(), w(), h());
        
        if (!m_lastCommands.empty()) {
            executeCommands(m_lastCommands);
        }
    }

private:
    void executeCommands(const std::string& commands) {
        std::istringstream stream(commands);
        std::string line;
        
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#') continue;
            
            std::vector<std::string> parts = splitString(line, '\t');
            executeCommand(parts);
        }
    }

    void executeCommand(const std::vector<std::string>& parts) {
        if (parts.empty()) return;
        
        std::string cmd = parts[0];
        
        if (cmd == "setwindow" && parts.size() >= 5) {
            m_windowLLX = std::stof(parts[1]);
            m_windowLLY = std::stof(parts[2]);
            m_windowURX = std::stof(parts[3]);
            m_windowURY = std::stof(parts[4]);
            
            // Calculate scaling
            float sourceWidth = m_windowURX - m_windowLLX;
            float sourceHeight = m_windowURY - m_windowLLY;
            if (sourceWidth > 0 && sourceHeight > 0) {
                m_scaleX = w() / sourceWidth;
                m_scaleY = h() / sourceHeight;
            }
        }
        else if (cmd == "setcolor" && parts.size() >= 2) {
            m_currentColor = std::stoi(parts[1]);
            fl_color(cgraphColorToFL(m_currentColor));
        }
        else if (cmd == "setbackground" && parts.size() >= 2) {
            m_backgroundColor = cgraphColorToFL(std::stoi(parts[1]));
        }
        else if (cmd == "setfont" && parts.size() >= 3) {
            m_currentFont = parts[1];
            m_currentFontSize = static_cast<int>(std::stof(parts[2]) * std::min(m_scaleX, m_scaleY));
            fl_font(getFLFont(m_currentFont), m_currentFontSize);
        }
        else if (cmd == "setjust" && parts.size() >= 2) {
            m_textJustification = std::stoi(parts[1]);
        }
        else if (cmd == "setorientation" && parts.size() >= 2) {
            m_textOrientation = std::stoi(parts[1]);
        }
        else if (cmd == "setlwidth" && parts.size() >= 2) {
            m_lineWidth = std::max(1, std::stoi(parts[1]) / 100);
            fl_line_style(FL_SOLID, m_lineWidth);
        }
        else if (cmd == "gsave") {
            fl_push_matrix();
        }
        else if (cmd == "grestore") {
            fl_pop_matrix();
            fl_color(cgraphColorToFL(m_currentColor));
            fl_font(getFLFont(m_currentFont), m_currentFontSize);
            fl_line_style(FL_SOLID, m_lineWidth);
        }
        else if (cmd == "setclipregion" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 4) {
                float x1 = transformX(std::stof(coords[0]));
                float y1 = transformY(std::stof(coords[1]));
                float x2 = transformX(std::stof(coords[2]));
                float y2 = transformY(std::stof(coords[3]));
                
                fl_push_clip(static_cast<int>(std::min(x1, x2)), 
                           static_cast<int>(std::min(y1, y2)),
                           static_cast<int>(std::abs(x2 - x1)), 
                           static_cast<int>(std::abs(y2 - y1)));
            }
        }
        else if (cmd == "circle" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 3) {
                float cx = transformX(std::stof(coords[0]));
                float cy = transformY(std::stof(coords[1]));
                float radius = transformWidth(std::stof(coords[2]) / 2);
                
                fl_arc(cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
            }
        }
        else if (cmd == "fcircle" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 3) {
                float cx = transformX(std::stof(coords[0]));
                float cy = transformY(std::stof(coords[1]));
                float radius = transformWidth(std::stof(coords[2]) / 2);
                
                fl_pie(cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
            }
        }
        else if (cmd == "line" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 4) {
                float x1 = transformX(std::stof(coords[0]));
                float y1 = transformY(std::stof(coords[1]));
                float x2 = transformX(std::stof(coords[2]));
                float y2 = transformY(std::stof(coords[3]));
                
                fl_line(x1, y1, x2, y2);
            }
        }
        else if (cmd == "moveto" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 2) {
                m_currentPosX = std::stof(coords[0]);
                m_currentPosY = std::stof(coords[1]);
            }
        }
        else if (cmd == "lineto" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 2) {
                float x1 = transformX(m_currentPosX);
                float y1 = transformY(m_currentPosY);
                float x2 = transformX(std::stof(coords[0]));
                float y2 = transformY(std::stof(coords[1]));
                
                fl_line(x1, y1, x2, y2);
                m_currentPosX = std::stof(coords[0]);
                m_currentPosY = std::stof(coords[1]);
            }
        }
        else if (cmd == "filledrect" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 4) {
                float x1 = transformX(std::stof(coords[0]));
                float y1 = transformY(std::stof(coords[1]));
                float x2 = transformX(std::stof(coords[2]));
                float y2 = transformY(std::stof(coords[3]));
                
                fl_rectf(std::min(x1, x2), std::min(y1, y2), 
                        std::abs(x2 - x1), std::abs(y2 - y1));
            }
        }
        else if (cmd == "poly" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 6 && coords.size() % 2 == 0) {
                fl_begin_line();
                for (size_t i = 0; i < coords.size(); i += 2) {
                    float x = transformX(std::stof(coords[i]));
                    float y = transformY(std::stof(coords[i + 1]));
                    fl_vertex(x, y);
                }
                fl_end_line();
            }
        }
        else if (cmd == "fpoly" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 6 && coords.size() % 2 == 0) {
                fl_begin_polygon();
                for (size_t i = 0; i < coords.size(); i += 2) {
                    float x = transformX(std::stof(coords[i]));
                    float y = transformY(std::stof(coords[i + 1]));
                    fl_vertex(x, y);
                }
                fl_end_polygon();
            }
        }
        else if (cmd == "drawtext" && parts.size() >= 2) {
            std::string text = parts[1];
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                text = text.substr(1, text.size() - 2);
            }
            
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
            fl_measure(text.c_str(), textWidth, textHeight);
            
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
            
            fl_draw(text.c_str(), tx, ty);
            fl_pop_matrix();
        }
        else if (cmd == "point" && parts.size() >= 2) {
            auto coords = splitString(parts[1], ' ');
            if (coords.size() >= 2) {
                float x = transformX(std::stof(coords[0]));
                float y = transformY(std::stof(coords[1]));
                fl_point(x, y);
            }
        }
    }

    // Coordinate transformation helpers
    float transformX(float x) const {
        return x * m_scaleX;
    }
    
    float transformY(float y) const {
        return h() - (y * m_scaleY);  // Flip Y coordinate
    }
    
    float transformWidth(float w) const {
        return w * m_scaleX;
    }
    
    float transformHeight(float h) const {
        return h * m_scaleY;
    }

    // Utility methods
    std::vector<std::string> splitString(const std::string& str, char delimiter) {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    Fl_Color cgraphColorToFL(int colorIndex) {
        static const Fl_Color colors[] = {
            FL_BLACK,      // 0
            FL_BLUE,       // 1
            FL_DARK_GREEN, // 2
            FL_CYAN,       // 3
            FL_RED,        // 4
            FL_MAGENTA,    // 5
            FL_DARK_YELLOW,// 6 - brown
            FL_WHITE,      // 7
            FL_GRAY,       // 8
            FL_DARK_BLUE,  // 9 - light blue (approximation)
            FL_GREEN,      // 10
            FL_DARK_CYAN,  // 11 - light cyan (approximation)
            FL_DARK_RED,   // 12 - deep pink (approximation)
            FL_DARK_MAGENTA, // 13 - medium purple (approximation)
            FL_YELLOW,     // 14
        };

        if (colorIndex >= 0 && colorIndex < 15) {
            return colors[colorIndex];
        }

        // Handle RGB colors (packed format)
        if (colorIndex > 18) {
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
        return FL_HELVETICA; // default
    }
};

#endif