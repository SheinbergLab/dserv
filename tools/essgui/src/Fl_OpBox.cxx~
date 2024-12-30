//////////////////////
// Fl_OpBox.C
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
#include <FL/Fl_RGB_Image.H>
#include "Fl_OpBox.H"
#include "Fl_OpButton.H"
#include "Fl_OpDesk.H"

/// FLTK-style constructor for an Fl_OpBox.
///    Creates an instance of an Fl_OpBox on the desk.
///    Box will have no buttons, and will use the FLTK defaults
///    for color(), labelfont(), labelsize(), etc.
///
Fl_OpBox::Fl_OpBox(int X,int Y,int W,int H,const char*L) : Fl_Group(X,Y,W,H,L) {
    opdesk = (Fl_OpDesk*)parent();
    GetOpDesk()->_AddBox(this);                                 // register new box with the desk

    min_buttw  = 20;                                            // default button width
    min_boxh   = H;
    min_conlen = 20;
    selected   = 0;
    box(FL_UP_BOX);
    align(FL_ALIGN_INSIDE|FL_ALIGN_CENTER|FL_ALIGN_TOP);        // center title inside box's 'titlebar'
    end();
}

/// Fl_OpBox's destructor.
///     Handles removing any connections.
///
Fl_OpBox::~Fl_OpBox() {
    if ( GetOpDesk() ) {
        GetOpDesk()->Disconnect(this);
    }
    GetOpDesk()->_RemoveBox(this);      // unregister box from desk
}

/// Fl_OpBox copy constructor. Makes a "copy" of Fl_OpBox \p o.
Fl_OpBox::Fl_OpBox(const Fl_OpBox &o) : Fl_Group(o.x(),o.y(),o.w(),o.h()) {
    opdesk = o.opdesk;
    opdesk->_AddBox(this);              // register new box with the desk

    // FLTK COPY STUFF
    copy_label(o.label());
    color(o.color());
    labelfont(o.labelfont());
    labelsize(o.labelsize());
    align(o.align());
    box(o.box());
    // TBD: MORE STUFF -- shortcuts, callbacks, etc.

    // OPBOX COPY STUFF
    eventxy[0] = o.eventxy[0];
    eventxy[1] = o.eventxy[1];
    dragging   = o.dragging;
    min_buttw  = o.min_buttw;
    min_boxh   = o.min_boxh;
    min_conlen = o.min_conlen;
    inbutt_w   = o.inbutt_w;
    outbutt_w  = o.outbutt_w;
    selected   = o.selected;
    opdesk     = o.opdesk;
}

/// Virtual method to handle copying buttons from one box to another.
/// Derived classes can override this to handle copying higher level
/// buttons from one box to another.
///
void Fl_OpBox::CopyButtons(const Fl_OpBox& o) {
    // MAKE COPIES OF INPUT BUTTONS
    for ( int i=0; i<o.GetTotalInputButtons(); i++ ) {
        begin();
            Fl_OpButton *but = o.GetInputButton(i);
            new Fl_OpButton(*but);                      // ctor handles adding it to our inputs[]
        end();
    }
    // MAKE COPIES OF OUTPUT BUTTONS
    for ( int j=0; j<o.GetTotalOutputButtons(); j++ ) {
        begin();
            Fl_OpButton *but = o.GetOutputButton(j);
            new Fl_OpButton(*but);                      // ctor handles adding it to our outputs[]
        end();
    }
}


/// INTERNAL: Used by Fl_OpButton ctor/dtors.
///     NOT INTENDED FOR PUBLIC USE.
///
void Fl_OpBox::_AddInputButton(Fl_OpButton *but) {
    inputs.push_back(but);
}

/// INTERNAL: Used by Fl_OpButton ctor/dtors.
///     NOT INTENDED FOR PUBLIC USE.
///
void Fl_OpBox::_AddOutputButton(Fl_OpButton *but) {
    outputs.push_back(but);
}

/// INTERNAL: Used by Fl_OpButton ctor/dtors.
///     NOT INTENDED FOR PUBLIC USE.
///
void Fl_OpBox::_RemoveButton(Fl_OpButton *but) {
    size_t t;
    for ( t=0; t<inputs.size(); t++ ) {
        if ( inputs[t] == but ) {
            inputs.erase(inputs.begin() + t);           // delete item from vector
            --t;
        }
    }
    for ( t=0; t<outputs.size(); t++ ) {
        if ( outputs[t] == but ) {
            outputs.erase(outputs.begin() + t);         // delete item from vector
            --t;
        }
    }
}

/// FLTK event handler.
///     Handles dragging the box around, box selection, etc.
///
int Fl_OpBox::handle(int e) {
    int ret = Fl_Group::handle(e);
    if ( Fl::event_button() == 1 ) {                    // left button? handle it
        switch ( e ) {
            case FL_PUSH: {
                eventxy[0] = Fl::event_x();             // save where user clicked for dragging
                eventxy[1] = Fl::event_y();

                // Change selection state
                if ( Fl::event_ctrl() ) {               // CTRL? toggle
                    if ( GetSelected() ) {
                        SetSelected(0);
                    } else {
                        SetSelected(1);
                        BringToFront();
                    }
                } else if ( Fl::event_shift() ) {       // SHIFT? append to selection
                    SetSelected(1);
                    BringToFront();
                } else {                                // No SHIFT/CTRL? single select
                    if ( !GetSelected() ) {             // not already selected? select only this
                        GetOpDesk()->DeselectAll();
                        SetSelected(1);
                        BringToFront();
                    }
                }
                GetOpDesk()->redraw();

                dragging = 0;
                ret = 1;
                break;
            }
            case FL_DRAG: {
                int ex = Fl::event_x();                 // current x/y pos
                int ey = Fl::event_y();
                int xdiff = ex - eventxy[0];            // how far we moved since last event
                int ydiff = ey - eventxy[1];
                eventxy[0] = ex;                        // save event x/y for next time
                eventxy[1] = ey;
                GetOpDesk()->DraggingBoxes(xdiff, ydiff);
                dragging++;                             // keep track of how much dragging is done
                ret = 1;
                break;
            }
            case FL_RELEASE: {
                ret = 1;
                break;
            }
        }
    }
    return(ret);
}

/// FLTK draw() method for the Fl_OpBox.
void Fl_OpBox::draw() {
    int title_h = GetTitleHeight();

    // Tell box to draw itself
    Fl_Group::draw();   // call Fltk base widget to draw children (Fl_OpButtons)

    // Draw titlebar upbox
    fl_draw_box(FL_UP_FRAME, x(), y(), w(), title_h, color());

    // Draw image area frame
    fl_draw_box(FL_UP_FRAME, x()+inbutt_w,
                             y()+title_h,
                             w()-inbutt_w-outbutt_w,
                             h()-title_h, 
                             color());

    // Selection box?
    if ( selected ) {
        std::vector<uint32_t> imgBuf(w() * GetTitleHeight(), 0X88FF0000);
        Fl_RGB_Image((const uchar *)(imgBuf.data()), w(), GetTitleHeight(), 4, 0).draw(x(), y());
    }
}

/// INTERNAL: Recalculates button sizes, eg. when a new button is added.
///     NOT INTENDED FOR PUBLIC USE.
///
void Fl_OpBox::_RecalcButtonSizes() {
    int title_h = GetTitleHeight();
    inbutt_w = GetMinimumButtonWidth();
    outbutt_w = GetMinimumButtonWidth();
    int new_boxh = GetMinimumBoxHeight();

    // PASS 1: FIND MAX WIDTHS + TOTAL HEIGHT FOR INPUT/OUTPUT BUTTONS
    int in_total = 0;           // how many input buttons
    int out_total = 0;          // how many output buttons
    int inbutt_totalh = 0;      // tally input height totals
    int outbutt_totalh = 0;     // tally output height totals
    int t;
    for ( t=0; t<children(); t++ ) {
        Fl_OpButton *b = (Fl_OpButton*)child(t);
        switch ( b->GetButtonType() ) {
            case FL_OP_INPUT_BUTTON:
                in_total++;     
                if ( inbutt_w < b->w() ) inbutt_w = b->w();
                inbutt_totalh += b->GetMinimumHeight();
                break;
            case FL_OP_OUTPUT_BUTTON:
                out_total++;
                if ( outbutt_w < b->w() ) outbutt_w = b->w();
                outbutt_totalh += b->GetMinimumHeight();
                break;
        }
    }
    // SEE IF WE NEED TO ENLARGE BOX'S HEIGHT BEYOND MINIMUM
    //    If button's minimum sizes too big for box, enlarge its height
    //
    if ( ( inbutt_totalh + title_h) > new_boxh ) {
        new_boxh = (inbutt_totalh + title_h);
    }
    if ( (outbutt_totalh + title_h) > new_boxh ) {
        new_boxh = (outbutt_totalh + title_h);
    }
    resize(x(), y(), w(), new_boxh);            // enlarge box if needed

    // PASS #2: REPOSITION BUTTONS
    //    Enlarge buttons:
    //          > horizontally to match widest button in each column (i/o).
    //          > vertically to fit box perfectly.
    //
    int inx = x();
    int iny = y() + title_h;
    int inh = (in_total > 0) ? ((h()-title_h) / in_total) : 0;          // try to fill entire box's height
    int outx = x()+w()-outbutt_w;
    int outy = y() + title_h;
    int outh = (out_total > 0) ? ((h()-title_h) / out_total) : 0;       // try to fill entire box's height

    Fl_OpButton *last_inbutt = 0;
    Fl_OpButton *last_outbutt = 0;
    for ( t=0; t<children(); t++ ) {
        Fl_OpButton *b = (Fl_OpButton*)child(t);
        switch ( b->GetButtonType() ) {
            case FL_OP_INPUT_BUTTON: {
                last_inbutt = b;
                b->resize(inx, iny, inbutt_w, inh);
                iny += inh;
                break;
            }
            case FL_OP_OUTPUT_BUTTON: {
                last_outbutt = b;
                b->resize(outx, outy, outbutt_w, outh);
                outy += outh;
                break;
            }
        }
    }

    // MAKE SURE LAST BUTTONS REACH BOTTOM OF WIDGET
    //    Since we're doing integer math above, need to lock it to the edge.
    //
    if ( last_inbutt ) {
        int H = (y()+h()) - last_inbutt->y();
        last_inbutt->resize(last_inbutt->x(), last_inbutt->y(), last_inbutt->w(), H);
    }
    if ( last_outbutt ) {
        int H = (y()+h()) - last_outbutt->y();
        last_outbutt->resize(last_outbutt->x(), last_outbutt->y(), last_outbutt->w(), H);
    }
    init_sizes();
}

/// Return Fl_OpButton given a button's label.
/// eg. "A1", as opposed to using its 'full name', "add_123_A1"
/// \returns The button specified, or 0 if an occurred, \p errmsg has reason.
///
Fl_OpButton* Fl_OpBox::FindButtonByLabel(const std::string& lname, std::string& errmsg) {
    for ( int t=0; t<children(); t++ ) {
        Fl_OpButton *b = (Fl_OpButton*)child(t);
        if ( b->label() ) {
            if ( lname == b->label() ) {
                return(b);
            }
        }
    }
    errmsg = std::string("box '") + 
             std::string(label()) +
             std::string("' has no button labeled '") +
             std::string(lname) + std::string("'");
    return(0);
}

/// Bring this box above all the others.
/// Selecting a box should do this, so that when the box(s)
/// are moved, they don't move behind other boxes.
///
void Fl_OpBox::BringToFront() {
    GetOpDesk()->BringToFront(this);
}

/// Returns the parent Fl_OpDesk for this Fl_OpBox.
Fl_OpDesk *Fl_OpBox::GetOpDesk() {
    return(opdesk);
}

/// Returns a const version of the parent Fl_OpDesk for this Fl_OpBox.
///  Const methods can use this to access the parent desk.
///
const Fl_OpDesk *Fl_OpBox::GetOpDesk() const {
    return(opdesk);
}

/// INTERNAL: See if any of this box's buttons are currently under the mouse.
Fl_OpButton* Fl_OpBox::FindButtonUnderMouse() {
    for ( int t=0; t<children(); t++ ) {
        Fl_OpButton *but = (Fl_OpButton*)child(t);
        if ( Fl::event_inside(but) ) return(but);
    }
    return(0);
}

/// Return the total number of input buttons.
/// To loop through all the input buttons, you can use:
/// \code
///    for ( int t=0; t<GetTotalInputButtons(); t++ ) {
///        Fl_OpButton *inbut = GetInputButton(t);
///        // ..your code here..
///    }
/// \endcode
///
int Fl_OpBox::GetTotalInputButtons() const {
    return(inputs.size());
}

/// Return input button for \p index.
Fl_OpButton* Fl_OpBox::GetInputButton(int index) const {
    return(inputs[index]);
}

/// Return index for an input button, or -1 if not found.
int Fl_OpBox::GetIndexForInputButton(Fl_OpButton *but) const {
    for ( size_t t=0; t<inputs.size(); t++ ) {
        if ( but == inputs[t] ) return(t);
    }
    return(-1);
}

/// Return the total number of output buttons.
/// To loop through all the output buttons, you can use:
/// \code
///    for ( int t=0; t<GetTotalOutputButtons(); t++ ) {
///        Fl_OpButton *outbut = GetOutputButton(t);
///        // ..your code here..
///    }
/// \endcode
///
int Fl_OpBox::GetTotalOutputButtons() const {
    return(outputs.size());
}

/// Return output button for \p index.
Fl_OpButton* Fl_OpBox::GetOutputButton(int index) const {
    return(outputs[index]);
}

/// Return index for an output button, or -1 if not found.
int Fl_OpBox::GetIndexForOutputButton(Fl_OpButton *but) const {
    for ( size_t t=0; t<outputs.size(); t++ ) {
        if ( but == outputs[t] ) return(t);
    }
    return(-1);
}

/// Return the total number of buttons.
/// To loop through all the buttons, you can use:
/// \code
///    for ( int t=0; t<GetTotalButtons(); t++ ) {
///        Fl_OpButton *but = GetButton(t);
///        // ..your code here..
///    }
/// \endcode
///
int Fl_OpBox::GetTotalButtons() const {
    return(inputs.size() + outputs.size());
}

/// Return input button for \p index.
Fl_OpButton* Fl_OpBox::GetButton(int index) const {
    if ( index < (int)inputs.size() ) {
        return(inputs[index]);
    } else {
        return(outputs[index-inputs.size()]);
    }
}

/// Return index for an input button, or -1 if not found.
int Fl_OpBox::GetIndexForButton(Fl_OpButton *but) const {
    for ( size_t t=0; t<inputs.size(); t++ ) {
        if ( but == inputs[t] ) return(t);
    }
    for ( size_t t=0; t<inputs.size(); t++ ) {
        if ( but == outputs[t] ) return(t+inputs.size());
    }
    return(-1);
}

/// Returns 1 if specified button pointer exists in this box, otherwise 0.
int Fl_OpBox::FindButtonByPtr(Fl_OpButton *but) const {
    for ( size_t t=0; t<inputs.size(); t++ ) {
        if ( but == inputs[t] ) return(1);
    }
    for ( size_t t=0; t<inputs.size(); t++ ) {
        if ( but == outputs[t] ) return(1);
    }
    return(0);
}
