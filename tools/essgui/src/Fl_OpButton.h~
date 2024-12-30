#ifndef _FL_OPBUTTON_H_
#define _FL_OPBUTTON_H_

//////////////////////
// Fl_OpButton.H
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

///
/// \file
/// \brief This file contains the definitions for the Fl_OpButton widget.
///

#include <vector>
#include <string>

#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/fl_draw.H>

#include "Fl_OpBox.H"

class Fl_OpDesk;
class Fl_OpBox;
class Fl_OpConnect;

/// \enum Fl_OpButtonType
/// The types of buttons; input or output.
///
enum Fl_OpButtonType {
    FL_OP_INPUT_BUTTON,         ///< -- an input button
    FL_OP_OUTPUT_BUTTON,        ///< -- an output button
};

/// \class Fl_OpButton
///
/// \brief The Fl_OpButton widget, an FLTK widget derived from Fl_Button
///        that manages just the buttons of an Fl_OpBox. These become
///        children of the Fl_OpBox in the usual FLTK fashion, where
///        opbox->begin() and opbox->end() are used to bracket the
///        creation of buttons, a simple example being:
/// \code
///         // Create box
///         Fl_OpBox *box = new Fl_OpBox(X,Y,150,80);
///         box->copy_label("add");
///         box->begin();
///             // Create two input buttons ("A", "B") and one output button ("OUT")
///             Fl_OpButton *a   = new Fl_OpButton("A", FL_OP_INPUT_BUTTON);
///             Fl_OpButton *b   = new Fl_OpButton("B", FL_OP_INPUT_BUTTON);
///             Fl_OpButton *out = new Fl_OpButton("OUT", FL_OP_OUTPUT_BUTTON);
///         box->end();
/// \endcode
///

/// The connection callback type.
///
///    The connection callback will be invoked whenever a connection between two
///    buttons is made or removed. 
///
///    The callback is invoked at both ends of the connection (for both buttons).
///    On input the opposing button is passed in, and the callback should determine
///    if the buttons can be connected or not, based on its return value:
///    the callback returns 0 to allow the connection, -1 to disallow.
///
///    WHEN A CONNECTION IS BEING MADE
///
///    On input:
///
///       button   -- the opposing button
///       userdata -- the optional userdata
///       type     -- will be Fl_OpButton::MAKE_CONNECT
///       errmsg   -- to be filled out if connection should not be allowed
///
///    On return:
///
///       Return value should be 0 if OK to connect, -1 on error with errmsg set to the reason.
///
///    WHEN A DISCONNECT IS BEING MADE
///
///    On input:
///
///       button   -- the opposing button being disconnected
///       userdata -- the optional userdata
///       type     -- will be Fl_OpButton::DISCONNECT
///       errmsg   -- not used
///
///    On return:
///
///       Return value should always be zero.
///
class Fl_OpButton;

// Operation Buttons
//    Manages all the input buttons for Fl_OpBoxes
//
class Fl_OpButton : public Fl_Button {
    Fl_OpButtonType           button_type;      // type of this button (input|output)
    std::vector<Fl_OpButton*> connected_to;     // array of button(s) we're connected to
    int                       dragging;         // used for connection dragging

    // PRIVATE METHODS
    friend class Fl_OpDesk;
    void _RemoveConnect(Fl_OpButton *but);
    int  _ConnectToOnly(Fl_OpButton *but, std::string &errmsg);
    void _DisconnectOnly(Fl_OpButton *but);

public:
    // CTORS
    Fl_OpButton(const char *L, Fl_OpButtonType io);
    ~Fl_OpButton();
    Fl_OpButton(const Fl_OpButton&);

    // FLTK
    void draw();
    int handle(int e);

    // MISC
    Fl_OpButtonType GetButtonType() const;
    int GetMinimumHeight() const;
    int GetMinimumWidth() const;
    std::string GetFullName() const;

    // PARENT ACCESS
    Fl_OpBox *GetOpBox();
    const Fl_OpBox* GetOpBox() const;
    Fl_OpDesk *GetOpDesk();
    const Fl_OpDesk* GetOpDesk() const;

    // CONNECTIONS
    int IsConnected() const;
    int IsConnected(const Fl_OpButton *but) const;
    int ConnectTo(Fl_OpButton *but, std::string &errmsg);
    void Disconnect(Fl_OpButton *but);
    void DisconnectAll();

    Fl_OpButton* GetConnectedButton(size_t index=0);
    const Fl_OpButton* GetConnectedButton(size_t index=0) const;
    size_t GetTotalConnectedButtons() const;
    void GetOutputBoxes(std::vector<Fl_OpBox*> &outboxes);
    Fl_OpConnect* GetConnection(size_t index);

    // VIRTUALS

    /// This virtual method can be defined by the app to detect when a connection to \p button is being made.
    ///
    /// The app can 'OK' the connection by returning 0, 
    /// or 'FAIL' the connection by returning -1, and setting \p errmsg to the reason why.
    ///
    /// Example:
    /// \code
    /// class MyButton : public Fl_OpButton {
    ///     // Handle checking connections
    ///     int Connecting(Fl_OpButton *to, std::string &errmsg) {
    ///         if ( to->OKToConnect(this) ) {
    ///             return(0);      // OK
    ///         } else {
    ///             errmsg = "It's not OK to connect";
    ///             return(-1);     // FAIL
    ///         }
    ///     }
    /// [..]
    /// \endcode
    ///
    ///    
    virtual int Connecting(Fl_OpButton*, std::string& errmsg) {
        return(0);
    }

    /// This virtual method can be defined by the app to detect when connections are being deleted.
    virtual void Disconnecting(Fl_OpButton*) {
        return;
    }

};
#endif /* _FL_OPBUTTON_H_ */
