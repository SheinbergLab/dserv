#ifndef _FL_OPDESK_H
#define _FL_OPDESK_H

//////////////////////
// Fl_OpDesk.H
//////////////////////
//
// OpDesk (Version 0.82)
// This file is part of the OpDesk node graph FLTK widget.
// Copyright (C) 2009,2011 by Greg Ercolano.
//
// This library is free software. Distribution and use rights are outlined in
// the file "COPYING.txt" which should have been included with this file.
// If this file is missing or damaged, see the FLTK library license at:
//
//     http://www.fltk.org/COPYING.php
//
// Please report all bugs and problems to:
//
//     erco (at) seriss.com
//

/// \mainpage Fl_OpDesk documentation
///
///     \image html opdesk-test-app.png "Fl_OpDesk example program"
///
/// <H2>Description</H2>
///
/// Fl_OpDesk can be used for just about anything that fits the paradigm
/// of 'connecting boxes together', such as making electronic circuit
/// simulators, modular music tools (ie. connecting an 'oscillator' to
/// an 'amplifier'), industrial control, programmable assembly lines,
/// etc. The 'boxes' on the 'desk' can represent anything, and what happens
/// when the user connects them is all up to you.
///
/// In 'widget' terminology, this is a 'node graph' or 'node tree' widget,
/// where the user can interactive connect 'operation boxes' (Fl_OpBox)
/// together to form interconnections (Fl_OpConnect) between 'input'
/// and 'output' buttons on each box (Fl_OpButton) all of which
/// is laid out on a 'desk' (Fl_OpDesk).
///
/// The purpose of this widget is to allow users to make modular 'layouts'
/// that can be visual constructions similar to the real world technique
/// of connecting black boxes together to form working systems. 
///
/// <H2>Implementation Synopsis</H2>
///
/// Each box has a row of 'inputs' (on the left) and 'outputs'
/// (on the right). Each input and output is an instance of an Fl_OpButton.
/// Multiple boxes can be created inside the 'Fl_OpDesk', which
/// is an FLTK 'group' widget that manages all the boxs' positions
/// on the desk, and their interconnections. Scrollbars appear if
/// the boxes go off the edges of the screen.
///
/// The idea is you derive one or more classes from Fl_OpBox to define
/// what each box does. You derive classes from Fl_OpButton to define
/// what each possible 'input' and 'output' button does. Each box makes
/// instances of these buttons to define the box's inputs and outputs.
/// You define which buttons can be connected to which other buttons.
/// You define callbacks that are triggered when the user makes 
/// connections, so you can warn them if they try to do disallowed
/// things, like connecting outputs to outputs. You can also derive
/// from Fl_OpDesk to take over control of how the desk works,
/// and implement functions like 'Load' and 'Save', and what these
/// do. You can make methods that walk the tree asking each box 
/// what's in it, and what it's connected to such as generate
/// code that the connections represent and pass them to a compiler
/// or script interpreter (the 'test-app' example shows all this),
/// or have it run a simulation or control hardware.
///
/// <H2>Is there a real world analogy?</H2>
///
/// A real world analogy would be connecting up a home stereo
/// system. For instance, and Fl_OpBox might represent the FM Tuner,
/// Tape Deck, Equalizer, Amplifier, Speakers. Each wire connecting the boxes
/// would be an 'Fl_OpConnect'. The input/output jacks would each be an
/// instance of an 'Fl_OpButton'. The entire system would be laid out
/// on a desk, which would be the 'Fl_OpDesk'.
///
/// <H2>What are some example applications?</H2>
///
/// Fl_OpDesk is good at representing just about any kind of modular system.
///
/// An example might be a Buchla modular sound system,
/// which lets one connect modular devices like oscillators, sequencers,
/// sound effects boxes, mixers, keyboards, ramp generators, amplifiers, etc.
/// Like the home stereo system above, only more types of black boxes.
///
/// Another example would be configuring the layout of an
/// industrial electro-mechanical assembly line's relays/switches/sensors/motors.
/// Or a residential lighting control / irrigation system. Or a digital
/// phone switch to manage adding/removing extensions, voice mail features,
/// paging systems, etc.
///
///     \image html light-control.png "Lighting control"
///
/// It could also be used as a graphic layout to demonstrate how to build
/// simple programs, where each box represents a function, and the connections
/// represent variables.
///
/// Another example could be modules for generating simple realtime graphics;
/// for instance separate 'sine' and 'cosine' generator boxes that feed into
/// the X/Y inputs of an oscilloscope to draw a circle, or by changing
/// the input frequencies, a 'spirograph'. Or perhaps a 3-D oscilloscope
/// so that 3 dimensional patterns can be represented with simple input patterns.
/// This could be nice for educational purposes to simulate an oscilloscope.
///
///     \image html oscilloscope.png "Oscilloscope simulator"
///
/// The example program included with this widget shows a full on application
/// of how to use Fl_OpDesk to generate simple perl scripts. Perhaps other
/// examples will be included.
///
/// <H2>External Dependencies</H2>
///
/// This widget assumes you have FLTK 1.x and the std C++ library
/// (eg. std::string, std::vector, etc)
///
/// The Fl_OpDesk project was designed so as to either be merged into
/// the FLTK 1.x library, or to be installed as a subdirectory of your
/// own project as a separate lib. (The latter is recommended. )
///
/// <H2>Where do I start?</H2>
///
/// For examples on how to use the Fl_OpDesk class, see 'test-OpDesk.cxx'
/// for a simple (but non-practical) use of it. For an actual practical
/// 'working' application, see the test-app/ directory, which shows a
/// fully working application that lets users generate Perl scripts
/// visually, and save + load the OpDesk layouts.
///

/// \file
/// \brief This file contains the definitions for the Fl_OpDesk widget.
///

#include <FL/Fl.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <vector>               // STL vector array
#include <cstdio>               // stderr

#include "Fl_OpBox.H"
#include "Fl_OpConnect.H"

/// \enum Fl_OpConnectStyle
/// Style for the connection lines between box buttons
///
enum Fl_OpConnectStyle {
    FL_OPCONNECT_STYLE_STRAIGHT,        ///< -- 'Single' style line connections between buttons
    FL_OPCONNECT_STYLE_TRACE,           ///< -- 'Solder trace' style line connections
    FL_OPCONNECT_STYLE_CURVE,           ///< -- 'Curved' style line connections (bezier)
};

/// \class Fl_OpDesk
///
/// \brief The main Fl_OpDesk widget, parent to the following widgets:
///      Fl_OpBox -- the boxes that can be connected together
///      Fl_OpButton -- the buttons (input and output) that can be added to an Fl_OpBox
///      Fl_OpConnect -- the connections between input/output buttons of different boxes.
///
/// \code
///
///     Fl_OpDesk                     // Desk -- top level widget that contains all boxes
///        |___ Fl_OpBox              // Boxes -- the operation boxes
///               |___ Fl_OpButton    // Buttons -- the buttons in the boxes
///
/// \endcode
///
/// This widget encapsulates the graphics elements of the desk + nodes.
/// Normally you would derive your own classes from each of the above classes
/// (Fl_OpDesk, Fl_OpBox, and Fl_OpButton) to implement management of your data.
///
/// It's the goal of this widget to not be tied to any particular data sets;
/// and only implements the graphic representation of the widget. You 
/// subclass these widgets to implement your data sets.
///
/// For instance, if you want the widget to be a graphic representation
/// of a program, then you'd add your own code to manage each box's code generation.
/// (See the 'test-app' example for how to do this, where MyDesk/MyBox/MyButton
/// are example subclasses showing how to implement the 'perl' programming language)
///
/// Or, if you want the widget to represent a data set, then you'd derive
/// classes so that each box represents a node in your data set. Examples of
/// this might be showing any kind of data in a 'tree', such as a file system,
/// a binary tree, or even hardware representations, such as an irrigation system
/// with each Fl_OpBox representing valves, flow control meters.
/// or an electronics component simulator where each box represents 
/// resistors,diodes,capacitors,relays,lights. Or in a sound application,
/// where each component represents different effects generators such
/// as oscillators and tone generators, reverb, echo, equalizers..
///
class Fl_OpDesk : public Fl_Scroll {

    int opconnect_spacing;              // desk's spacing between opconnect lines
    Fl_OpConnectStyle opconnect_style;  // desk's line style (straight,trace) for opconnect lines
    int opconnect_width;                // default line width for new opconnect's
    Fl_Color opconnect_color;           // default color used for new opconnect's
    int opbox_selectedbordersize;       // thickness of selected OpBox's border
    std::vector<Fl_OpConnect> connects; // our array of all button interconnects
    std::vector<Fl_OpBox*> boxes;       // boxes on desk

    // Clipboard
    std::vector<Fl_OpBox*> box_clipboard;               // copy/paste clipboard for Fl_OpBox's

    // Connection dragging
    char dragging;                      // on/off flag: 1=dragging, 0=not
    int dragging_x1, dragging_y1,       // when dragging=1, these contain the two
        dragging_x2, dragging_y2;       // points to draw a connection line between.

protected:
    // PROTECTED METHODS
    // Drawing..
    void DrawLine(float x1, float y1, float x2, float y2) const;
    void DrawConnect(Fl_OpConnect *con) const;

    // Connections..
    friend class Fl_OpButton;
    friend class Fl_OpBox;
    int _ConnectOnly(Fl_OpButton *srcbut, Fl_OpButton *dstbut, std::string &errmsg);
    int _DisconnectOnly(Fl_OpButton *a, Fl_OpButton *b);
    void CopyConnections(std::vector<Fl_OpBox*> origboxes, std::vector<Fl_OpBox*> copyboxes);

    // Dragging
    void DraggingConnection(Fl_OpButton *b);
    void DraggingBoxes(int xdiff, int ydiff);

    void _AddBox(Fl_OpBox *b);
    void _RemoveBox(Fl_OpBox *b);

public:
    // CTOR
    Fl_OpDesk(int X,int Y,int W,int H,const char *L=0);
    ~Fl_OpDesk();

    // FLTK
    void draw();
    int handle(int e);

    // OPBOX
    void DeleteBox(Fl_OpBox *box);
    int GetOpBoxIndex(const Fl_OpBox*) const;
    int GetOpBoxTotal() const;
    Fl_OpBox* GetOpBox(int index);
    const Fl_OpBox* GetOpBox(int t) const;

    // SELECTION
    int DeselectAll();
    int SelectAll();

    // COPY/PASTE
    virtual int DeleteSelected();
    virtual int CutSelected();
    virtual int CopySelected();
    virtual int PasteSelected();

    // SEARCH FUNCTIONS
    Fl_OpBox*    FindBoxForFullName(const std::string& fullname, std::string &errmsg);
    Fl_OpButton* FindButtonForFullName(const std::string& fullname, std::string &errmsg);
    int          FindButtonByPtr(Fl_OpButton *b) const;
    Fl_OpBox*    FindBoxByLabel(std::string &lname, std::string &errmsg);
    Fl_OpButton* FindButtonUnderMouse();

    // MAKE/DELETE CONNECTIONS, CONNECTION ATTRIBUTES
    int GetOpConnectSpacing() const;
    void SetOpConnectSpacing(int val);
    int GetOpConnectWidth() const;
    void SetOpConnectWidth(int val);
    Fl_Color GetOpConnectColor() const;
    void SetOpConnectColor(Fl_Color val);
    int GetConnectionsTotal() const;
    Fl_OpConnect* GetConnection(int index);
    Fl_OpConnect* GetConnection(Fl_OpButton *srcbut, Fl_OpButton *dstbut);
    Fl_OpConnect* GetLastOpConnect();
    int Connect(Fl_OpButton *srcbut, Fl_OpButton *dstbut, std::string &errmsg);
    int Connect(Fl_OpBox *srcbox, const std::string& srcbut_lname, 
                Fl_OpBox *dstbox, const std::string& dstbut_lname,
                std::string &errmsg);
    int Connect(const std::string& src_name, 
                const std::string& dst_name,
                      std::string& errmsg);
    int Disconnect(Fl_OpButton *a, Fl_OpButton *b);
    int Disconnect(Fl_OpBox *box);
    void DisconnectAll();
    void BringToFront(Fl_OpBox *box);

    // DISPLAY STYLE
    Fl_OpConnectStyle GetConnectStyle(void) const;
    void              SetConnectStyle(Fl_OpConnectStyle val);
    int GetOpBoxSelectedBorderSize() const;
    void SetOpBoxSelectedBorderSize(int val);

    // MISC
    void Clear();
    int ParseFullButtonName(const std::string &fullname,
                                  std::string &boxname,
                                  std::string &butname,
                                  std::string &errmsg);

    // VIRTUALS

    /// This virtual method can be defined by the app to define what to do
    /// when a connection error is detected. Default behavior is to print
    /// the \p errmsg to stderr, but app may want to post a dialog instead.
    /// The \p srcbut and \p dstbut are the buttons whose connection attempt had failed.
    ///
    /// Example:
    /// \code
    /// class MyDesk : public Fl_OpDesk {
    ///     // Handle displaying connection errors
    ///     void ConnectionError(Fl_OpButton *src, Fl_OpButton *dst, std::string &errmsg) {
    ///         std::string msg;
    ///         msg = std::string("    INPUT: ") + src->GetFullName() + std::string("\n") +
    ///               std::string(   "OUTPUT: ") + dst->GetFullName() + std::string("\n") +
    ///               errmsg;
    ///         fl_alert(msg.c_str());          // post a dialog
    ///     }
    /// [..]
    /// \endcode
    ///
    virtual void ConnectionError(Fl_OpButton* /*srcbut*/, Fl_OpButton* /*dstbut*/, std::string &errmsg) {
        fprintf(stderr, "CONNECTION ERROR: %s\n", errmsg.c_str());
    }
};

#endif /*_FL_OPDESK_H*/
