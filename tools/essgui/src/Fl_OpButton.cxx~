//////////////////////
// Fl_OpButton.C
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

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include "FL/names.h"
#include "Fl_OpButton.H"
#include "Fl_OpDesk.H"

/// Constructor for the Fl_OpButton.
/// Creates a button whose label is \p name , and who's type is \p io.
/// 'name' can be null, and set later with label() or copy_label().
/// Placement of the button is handled entirely by the parent Fl_OpBox.
///
Fl_OpButton::Fl_OpButton(const char *name, Fl_OpButtonType io) : Fl_Button(0,0,1,1,name) {
    // Tell parent opbox we're a new button
    switch ( io ) {
        case  FL_OP_INPUT_BUTTON: GetOpBox()->_AddInputButton(this);  break;
        case FL_OP_OUTPUT_BUTTON: GetOpBox()->_AddOutputButton(this); break;
    }
    button_type = io;
    connected_to.clear();

    // Inherit parent opbox colors
    color(GetOpBox()->color());
    labelcolor(GetOpBox()->labelcolor());
    copy_label(name);                           // force internal label copy

    // Determine min width/height of button
    int W = GetMinimumWidth();
    int H = GetMinimumHeight();
    resize(x(), y(), W, H);                     // set our width + height
    GetOpBox()->_RecalcButtonSizes();
}

/// Destructor for the Fl_OpButton.
///    Handles disconnecting all connections.
///
Fl_OpButton::~Fl_OpButton() {
    if ( GetOpBox() ) { GetOpBox()->_RemoveButton(this); }
}

/// Fl_OpButton copy constructor. Makes a "copy" of Fl_OpButton \p o.
Fl_OpButton::Fl_OpButton(const Fl_OpButton &o) : Fl_Button(o.x(),o.y(),o.w(),o.h()) {

    // FLTK COPY STUFF
    copy_label(o.label());
    color(o.color());
    labelfont(o.labelfont());
    labelsize(o.labelsize());
    align(o.align());
    box(o.box());
    // TBD: MORE STUFF -- shortcuts, callbacks, etc.

    // OPBUTTON COPY STUFF
    button_type = o.button_type;
    dragging    = o.dragging;

    switch ( button_type ) {
        case  FL_OP_INPUT_BUTTON: GetOpBox()->_AddInputButton(this);  break;
        case FL_OP_OUTPUT_BUTTON: GetOpBox()->_AddOutputButton(this); break;
    }
    // Determine min width/height of button
    int W = GetMinimumWidth();
    int H = GetMinimumHeight();
    resize(x(), y(), W, H);                     // set our width + height
    GetOpBox()->_RecalcButtonSizes();
}

/// Fltk draw() handler.
void Fl_OpButton::draw() {
    Fl_Button::draw();
}

/// Fltk event handler.
int Fl_OpButton::handle(int e) {
    // HANDLE DRAGGING OUT NEW CONNECTIONS
    int ret = Fl_Button::handle(e);
    switch ( e ) {
        case FL_PUSH:
            ret = 1;
            dragging = 0;
            break;
        case FL_DRAG:
            // See if user dragged off our button.
            // If so, start dragging a connection..
            //
            if ( ! Fl::event_inside(this) ) {
                dragging = 1;                           // enable dragging when user leaves button
            }
            if ( dragging ) {
                // Tell Fl_OpDesk to draw connection between our button and current mouse position
                GetOpDesk()->DraggingConnection(this);  // handles dragging..
            }
            break;
        case FL_RELEASE:
            if ( dragging ) {
                GetOpDesk()->DraggingConnection(NULL);  // Tell desk to stop dragging
                Fl_OpButton *but = GetOpDesk()->FindButtonUnderMouse();
                // Connect if:
                //    1) Valid button
                //    2) Buttons being connected are not on same box.
                //
                if ( but && this->GetOpBox() != but->GetOpBox() ) {

                    // RELEASED OVER SOME OTHER BUTTON
                    //    Try to connect the two.
                    //
                    std::string errmsg;
                    switch ( GetOpDesk()->Connect(this, but, errmsg) ) {
                        // TODO: WE NEED A WAY TO ADVERTISE ERRORS BACK TO THE MAIN APP.
                        //       PROBABLY AN ERROR CALLBACK IS NEEDED.
                        //
                        case 0:         // OK
                            // printf("CONNECTED OK\n");        // DEBUGGING
                            break;
                        case 1:         // ALREADY CONNECTED
                            printf("CAN'T CONNECT (ALREADY CONNECTED): '%s'\n", errmsg.c_str());
                            break;
                        case -1:        // CAN'T CONNECT
                            printf("CAN'T CONNECT[2]: '%s'\n", errmsg.c_str());
                            GetOpDesk()->redraw();
                            break;
                    }
                }
            }
            break;
    }
    return(ret);
}

/// PROTECTED: Connect this button only in our internal list.
/// Doesn't tell other button or the Fl_OpDesk about the connection;
/// this is a local change only. NOT INTENDED FOR PUBLIC USE.
///
int Fl_OpButton::_ConnectToOnly(Fl_OpButton *but, std::string &errmsg) {
    // Other button same type as ours? (input<->input or output<->output)
    if ( but->GetButtonType() == button_type ) {
        errmsg = "can't connect "; errmsg += this->GetFullName(); 
        errmsg += " to ";          errmsg +=  but->GetFullName();
        errmsg += ": tried to connect two ";
        errmsg += (button_type==FL_OP_INPUT_BUTTON?"in":"out");
        errmsg += "puts together";
        return(-1);
    }

    // Let app know we're making a connection
    if ( Connecting(but, errmsg) < 0 ) {
        return(-1);
    }

    // Input? Only one connection allowed, disconnect previous
    if ( button_type == FL_OP_INPUT_BUTTON ) DisconnectAll();

    // Add to list
    connected_to.push_back(but);
    return(0);
}

/// PRIVATE: Remove all connections to \p but from our connected_to[] list.
/// That's all this does.. no notifications are made to other button;
/// that's handled by the public methods.
///
void Fl_OpButton::_RemoveConnect(Fl_OpButton *but) {
    for ( size_t t=0; t<connected_to.size(); t++ ) {
        if ( connected_to[t] == but ) {
            connected_to.erase(connected_to.begin()+t);
            --t;
        }
    }
}

/// PROTECTED: Disconnect a button only from our internal list.
/// Doesn't tell other button or the Fl_OpDesk about the disconnect;
/// this is a local change only. NOT INTENDED FOR PUBLIC USE.
//
void Fl_OpButton::_DisconnectOnly(Fl_OpButton *but) {
    // Let app know we're disconnecting
    Disconnecting(but);

    // Remove the connection
    _RemoveConnect(but);
}

/// Connect this button to another one.
/// Handles a) telling other button about the connection, and
/// b) avoids illegal "output-to-output" or "input-to-input" connections.
///
/// If we're an input button, any previous connection is dropped first.
/// If we're an output button, drops any existing connections *to dstbut*,
/// then recreates.
///
/// \returns
///    0 if OK. Otherwise, -1 on error, \p errmsg has reason.
///
int Fl_OpButton::ConnectTo(Fl_OpButton *but, std::string &errmsg) {

    // Handle internal connections
    if ( _ConnectToOnly(but, errmsg) < 0 ) return(-1);

    // Tell other button about us
    if ( but->_ConnectToOnly(this, errmsg) < 0 ) {
        _DisconnectOnly(but);
        return(-1);
    }

    // Add connection to desk
    if ( GetOpDesk()->_ConnectOnly(this, but, errmsg) < 0 ) {
        _DisconnectOnly(but);
        but->_DisconnectOnly(this);
        return(-1);
    }

    return(0);
}

/// Disconnect a connection to a specified button
///     The correct way to cleanly disconnect two buttons.
///
void Fl_OpButton::Disconnect(Fl_OpButton *but) {

    // Remove from internal array
    _DisconnectOnly(but);

    // Tell other button to disconnect us
    but->_DisconnectOnly(this);

    // Tell desk to remove us
    GetOpDesk()->_DisconnectOnly(this, but);
    GetOpDesk()->redraw();
}

/// Disconnect all buttons connected to this one.
///     Handles telling the other buttons and the desk about the disconnect.
///
void Fl_OpButton::DisconnectAll() {
    while ( connected_to.size() > 0 ) {
        Disconnect(connected_to[0]);
    }
}

/// See if this button has any connections.
int Fl_OpButton::IsConnected() const {
    return(GetTotalConnectedButtons() > 0 ? 1 : 0);
}

/// See if the specified button \p but is in our connection list.
/// \returns 1 if button is in connection list
/// \returns 0 if not.
///
int Fl_OpButton::IsConnected(const Fl_OpButton *but) const {
    for ( size_t t=0; t<connected_to.size(); t++ ) {
        if ( connected_to[t] == but ) {
            return(1);
        }
    }
    return(0);
}

/// Return the 'full instance name' for this button.
///    This name uniquely identifies this button from all the other
///    buttons on the Fl_OpDesk, the form being "BOX(BUT)", where "BOX" is
///    the parent Fl_OpBox's unique name, and "BUT" is this button's unique label.
///
std::string Fl_OpButton::GetFullName() const {
    std::string name = std::string(GetOpBox()->label()) +
                       std::string("(") +
                       std::string(label()) +
                       std::string(")");
    return(name);
}

/// Return the type of button; input or output.
Fl_OpButtonType Fl_OpButton::GetButtonType() const {
    return(button_type);
}

/// Get the button's 'minimum height', given its label size, and adjusting for margins.
int Fl_OpButton::GetMinimumHeight() const {
    return(labelsize() + 6);
}

/// Get the button's 'minimum width', given its current font size and label contents.
int Fl_OpButton::GetMinimumWidth() const {
    if ( label() ) {
        fl_font(labelfont(), labelsize());      // fl_width() needs to know font
        return((int)fl_width(label()) + 6);     // 6 pixel margin on either side
    } else return(6);
}

/// Return the pointer this button's parent Fl_OpBox.
Fl_OpBox *Fl_OpButton::GetOpBox() {
    return((Fl_OpBox*)parent());
}

/// Return a constant pointer this button's parent Fl_OpBox.
const Fl_OpBox* Fl_OpButton::GetOpBox() const {
    return((const Fl_OpBox*)parent());
}

/// Return a pointer to the parent OpDesk.
Fl_OpDesk *Fl_OpButton::GetOpDesk() {
    return(GetOpBox()->GetOpDesk());
}

/// Return a constnat pointer to the parent OpDesk.
const Fl_OpDesk *Fl_OpButton::GetOpDesk() const {
    return(GetOpBox()->GetOpDesk());
}

/// Return the total number of other buttons we're connected to.
/// To loop through all the buttons we're connected to, use:
/// \code
///    for ( int t=0; t<GetTotalConnectedButtons(); t++ ) {
///        Fl_OpButton *otherbut = GetConnectedButton(t);
///        // ..your code here..
///    }
/// \endcode
/// \returns The total number of buttons we're connected to.
///          Input buttons can only be connected to one other button.
///          Output buttons can be connected to several.
///
size_t Fl_OpButton::GetTotalConnectedButtons() const {
    return(connected_to.size());
}

/// Return one of the buttons we're connected to, given \p index.
///
/// \p index is NOT range checked; use either IsConnected()
/// to see if there's at least one connection, or use
/// GetTotalConnectedButtons() to determine the max value
/// for \p index.
///
/// To loop through all the connected buttons, you can use:
/// \code
///    for ( int t=0; t<GetTotalConnectedButtons(); t++ ) {
///        Fl_OpButton *otherbut = GetConnectedButton(t);
///        // ..your code here..
///    }
/// \endcode
///
Fl_OpButton* Fl_OpButton::GetConnectedButton(size_t index) {
    return(connected_to[index]);
}

/// Const version of GetConnectedButton().
const Fl_OpButton* Fl_OpButton::GetConnectedButton(size_t index) const {
    return(connected_to[index]);
}

/// Return the Fl_OpConnect connection for the given connected button \p index.
///
/// \p index is NOT range checked; use either IsConnected()
/// to see if there's at least one connection, or use
/// GetTotalConnectedButtons() to determine the max value
/// for \p index.
///
Fl_OpConnect* Fl_OpButton::GetConnection(size_t index) {
    return(GetOpDesk()->GetConnection(this, connected_to[index]));
}

/// Returns array of \p outboxes connected to this button.
void Fl_OpButton::GetOutputBoxes(std::vector<Fl_OpBox*> &outboxes) {
    for ( size_t t=0; t<connected_to.size(); t++ ) {
        outboxes.push_back(connected_to[t]->GetOpBox());
    }
}
