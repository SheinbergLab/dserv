#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/fl_ask.H>

//////////////////////
// Fl_OpDesk.C
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

#include "Fl_OpDesk.H"

/// Constructor for Fl_OpDesk
///    Creates an instance of an empty desk with scrollbars.
///
Fl_OpDesk::Fl_OpDesk(int X,int Y,int W,int H,const char* L) : Fl_Scroll(X,Y,W,H,L) {
    // Defaults for the desk
    color(0x567b9100);          // sky blue
    opconnect_spacing = 10;
    opconnect_style = FL_OPCONNECT_STYLE_CURVE;
    opconnect_width = 2;
    opconnect_color = Fl_Color(0xf0d0a000);             // whiteish
    opbox_selectedbordersize = 3;
    dragging = 0;
    box(FL_DOWN_BOX);
    end();
}

/// Fl_OpDesk's destructor.
///     Handles disconnecting everything.
///
Fl_OpDesk::~Fl_OpDesk() {
    DisconnectAll();
}

/// INTERNAL: Draws a line between two floating point x/y points.
void Fl_OpDesk::DrawLine(float x1, float y1, float x2, float y2) const {
    fl_line((int)(x1+.5), (int)(y1+.5), (int)(x2+.5), (int)(y2+.5));
}

// Draw a non-diagonal line using the specified "thickness"
//     We have our own code for this to ensure consistent operation
//     on all platforms.
//
void DrawFlatLine(float x1, float y1, float x2, float y2, int thick, int noleft, int noright) {
    if ( thick <= 1 ) {
        fl_line((int)(x1+.5), (int)(y1+.5), (int)(x2+.5), (int)(y2+.5));
    } else {
        // CONVERT X/Y POINTS TO XYWH
        int X = (int)(x1+.5),
            Y = (int)(y1+.5),
            W = (int)(x2+.5)-X,
            H = (int)(y2+.5)-Y;
        // ENSURE POSITIVE WIDTH/HEIGHT
        if ( W < 0 ) { W = -W; X -= W; }
        if ( H < 0 ) { H = -H; Y -= H; }
        // ADJUST FOR THICKNESS (OVERDRAW METHOD FOR SQUARE LINE CAPS)
        X -= noleft ? 0 : ( thick/2 );
        Y -= ( thick/2 );
        W += (noleft&&noright) ? 0 : (noleft||noright) ? (thick/2) : thick;
        H += thick;
        // DRAW FILLED RECTANGLE
        fl_rectf(X,Y,W,H);
    }
}

/// Draw the connect specified by \p con.
void Fl_OpDesk::DrawConnect(Fl_OpConnect *con) const {
    Fl_OpButton *src = con->GetDstButton();             // in
    Fl_OpButton *dst = con->GetSrcButton();             // out
    Fl_OpBox *srcbox = src->GetOpBox();
    Fl_OpBox *dstbox = dst->GetOpBox();

    const int connectwidth = con->GetWidth();

    float srcx = src->x();
    float srcy = src->y()+src->h()/2;
    float dstx = dst->x()+dst->w();
    float dsty = dst->y()+dst->h()/2;

    // Draw a line based on the line type
    switch (opconnect_style) {
        case FL_OPCONNECT_STYLE_STRAIGHT: {
            DrawLine(srcx, srcy, dstx, dsty);
            return;
        }
        case FL_OPCONNECT_STYLE_TRACE:
        case FL_OPCONNECT_STYLE_CURVE: {
            int nsrc = srcbox->GetTotalInputButtons();
            if (nsrc <= 0) return;

            int srcidx = srcbox->GetIndexForInputButton(src);
            if (srcidx < 0) return;

            int ndst = dstbox->GetTotalOutputButtons();
            if (ndst <= 0) return;

            int dstidx = dstbox->GetIndexForOutputButton(dst);
            if (dstidx < 0) return;

            if (srcy < dsty) { 
                // Input box "lower" than output box
                float sX = srcx-(nsrc-srcidx)*opconnect_spacing;
                if (sX > dstx+opconnect_spacing) { 
                    // Draw the "S" trace as three line segments
                    //
                    //            -----o dst
                    //            |
                    //   src o-----
                    //
                    switch ( opconnect_style ) {
                        case FL_OPCONNECT_STYLE_CURVE:
                            fl_begin_line();
                            fl_curve(srcx,srcy, dstx,srcy, srcx,dsty, dstx,dsty);
                            fl_end_line();
                            break;
                        case FL_OPCONNECT_STYLE_TRACE:
                            DrawFlatLine(dstx, dsty, sX, dsty, connectwidth, 1, 0);
                            DrawFlatLine(sX, dsty, sX, srcy, connectwidth, 0, 0);
                            DrawFlatLine(sX, srcy, srcx, srcy, connectwidth, 0, 1);
                            break;
                        default:
                            break;
                    }
                } else { 
                    // Draw the "S" trace as five line segments
                    //       E    F
                    //       o----o src
                    //       |
                    //     D o----o C
                    //            |
                    //   dst o----o
                    //       A    B
                    //
                    float Ax = dstx;
                    float Ay = dsty;
                    float Bx = dstx + dstbox->GetMinimumConnectLength() + (dstidx * opconnect_spacing);
                    float By = dsty;
                    float Cx = Bx;
                    float Cy = dstbox->y() - (opconnect_spacing * (dstidx+1));
                    float Dx = srcx - srcbox->GetMinimumConnectLength() - ((nsrc-srcidx) * opconnect_spacing);
                    float Dy = Cy;
                    float Ex = Dx;
                    float Ey = srcy;
                    float Fx = srcx;
                    float Fy = srcy;
                    switch ( opconnect_style ) {
                        case FL_OPCONNECT_STYLE_CURVE: {
                            float BZx = Bx + 20;
                            float BZy = By;
                            float CZx = Cx + 20;
                            float CZy = Cy;
                            float EZx = Ex - 20;
                            float EZy = Ey;
                            float DZx = Dx - 20;
                            float DZy = Dy;
                            DrawFlatLine(Ax,Ay, Bx,By, connectwidth, 0, 0);
                            fl_polygon(Ax, Ay, Ax + 4, Ay - 2, Ax + 4, Ay + 2);
                            fl_begin_line();
                            fl_curve(Bx,By, BZx,BZy, CZx, CZy, Cx,Cy);
                            fl_end_line();
                            DrawFlatLine(Cx,Cy, Dx,Dy, connectwidth, 0, 0);
                            fl_begin_line();
                            fl_curve(Dx,Dy, DZx,DZy, EZx,EZy, Ex,Ey);
                            fl_end_line();
                            DrawFlatLine(Ex,Ey, Fx,Fy, connectwidth, 0, 0);
                            break;
                        }
                        case FL_OPCONNECT_STYLE_TRACE:
                            //         ---o dst
                            //         |
                            //         -------------
                            //                     |
                            //              src o---
                            //
                            DrawFlatLine(Ax,Ay, Bx,By, connectwidth, 0, 0);
                            DrawFlatLine(Bx,By, Cx,Cy, connectwidth, 0, 0);
                            DrawFlatLine(Cx,Cy, Dx,Dy, connectwidth, 0, 0);
                            DrawFlatLine(Dx,Dy, Ex,Ey, connectwidth, 0, 0);
                            DrawFlatLine(Ex,Ey, Fx,Fy, connectwidth, 0, 0);
                            break;
                            break;
                        default:                        // shouldn't happen
                            break;
                    }
                }
            } else {
                // Input box lower than output box
                float sX = srcx-(srcidx+1)*opconnect_spacing;
                if (sX > dstx+opconnect_spacing) { 
                    // Draw the "S" trace as three line segments
                    //
                    //   src o-----
                    //            |
                    //            -----o dst
                    //
                    switch ( opconnect_style ) {
                        case FL_OPCONNECT_STYLE_CURVE:
                            fl_begin_line();
                            fl_curve(srcx,srcy, dstx,srcy, srcx,dsty, dstx,dsty);
                            fl_end_line();
                            break;
                        case FL_OPCONNECT_STYLE_TRACE:
                            DrawFlatLine(dstx, dsty, sX, dsty, connectwidth, 1, 0);
                            DrawFlatLine(sX, dsty, sX, srcy, connectwidth, 0, 0);
                            DrawFlatLine(sX, srcy, srcx, srcy, connectwidth, 0, 1);
                            break;
                        default:
                            break;
                    }
                } else { 
                    // Draw the backwards-"S" trace as five line segments
                    //                                
                    //        A     B   
                    //    src o----o    
                    //             |
                    //      D o----o C  
                    //        |     
                    //        o----o dst
                    //        E    F
                    //  
                    float Ax = dstx;
                    float Ay = dsty;
                    float Bx = dstx + dstbox->GetMinimumConnectLength() + (dstidx * opconnect_spacing);
                    float By = dsty;
                    float Cx = Bx;
                    float Cy = dstbox->y()+dstbox->h() + (opconnect_spacing * (dstidx+1));
                    float Dx = srcx - srcbox->GetMinimumConnectLength() - (srcidx * opconnect_spacing);
                    float Dy = Cy;
                    float Ex = Dx;
                    float Ey = srcy;
                    float Fx = srcx;
                    float Fy = srcy;
                    switch ( opconnect_style ) {
                        case FL_OPCONNECT_STYLE_CURVE: {
                            //                                 bezier curve
                            //                                  ctl points
                            //                                   ___|__
                            //                      A     B     |      |
                            //                  src o----o       x   BZ
                            //                            )
                            //       DZ   x       D o----o C     x   CZ
                            //                     (
                            //       EZ   x         o----o dst
                            //      |______|        E    F
                            //         |
                            //    bezier curve
                            //     ctl points
                            //
                            float BZx = Bx + 20;
                            float BZy = By;
                            float CZx = Cx + 20;
                            float CZy = Cy;
                            float EZx = Ex - 20;
                            float EZy = Ey;
                            float DZx = Dx - 20;
                            float DZy = Dy;
                            DrawFlatLine(Ax,Ay, Bx,By, connectwidth, 0, 0);
                            fl_begin_line();
                            fl_curve(Bx,By, BZx,BZy, CZx, CZy, Cx,Cy);
                            fl_end_line();
                            DrawFlatLine(Cx,Cy, Dx,Dy, connectwidth, 0, 0);
                            fl_begin_line();
                            fl_curve(Dx,Dy, DZx,DZy, EZx,EZy, Ex,Ey);
                            fl_end_line();
                            DrawFlatLine(Ex,Ey, Fx,Fy, connectwidth, 0, 0);
                            break;
                        }
                        case FL_OPCONNECT_STYLE_TRACE: {
                            //                  A
                            //              src o--o B
                            //                     |
                            //       D o-----------o C
                            //         |
                            //       E o--o dst
                            //            F
                            //
                            DrawFlatLine(Ax,Ay, Bx,By, connectwidth, 0, 0);
                            DrawFlatLine(Bx,By, Cx,Cy, connectwidth, 0, 0);
                            DrawFlatLine(Cx,Cy, Dx,Dy, connectwidth, 0, 0);
                            DrawFlatLine(Dx,Dy, Ex,Ey, connectwidth, 0, 0);
                            DrawFlatLine(Ex,Ey, Fx,Fy, connectwidth, 0, 0);
                            break;
                        }
                        default:                        // shouldn't happen
                            break;
                    }
                }
            }
            return;
        }
    }
}

/// FLTK draw() method for the Fl_OpDesk.
void Fl_OpDesk::draw() {
    Fl_Scroll::draw();          // call fltk's base widget to draw children

    // DEFINE CLIP REGION
    //    Must do this to avoid overdrawing scrollbars, borders, etc.
    //
    int XC = x() + Fl::box_dx(box());
    int YC = y() + Fl::box_dy(box());
    int WC = ( scrollbar.visible() ? w()- scrollbar.w() : w()) - Fl::box_dw(box());
    int HC = (hscrollbar.visible() ? h()-hscrollbar.h() : h()) - Fl::box_dh(box());
    int lastwidth = 1;
    fl_push_clip(XC,YC,WC,HC);
    {
        // DRAW CONNECTION LINES
        for ( int t=0; t<GetConnectionsTotal(); t++ ) {
            Fl_OpConnect *con = GetConnection(t);

            // Connection color
            fl_color(con->GetColor());

            // Connection width
            if ( con->GetWidth() != lastwidth ) {
                fl_line_style(FL_SOLID, con->GetWidth());
                lastwidth = con->GetWidth();
            }

            // Draw the connection
            DrawConnect(GetConnection(t));
        }
        if ( lastwidth != 1 ) {         // width changed?
            fl_line_style(0);           // return to default
        }

        // ANY CONNECTIONS BEING ACTIVELY DRAGGED? DRAW IT
        if ( dragging ) {
            fl_color(FL_WHITE);
            fl_line_style(FL_SOLID, 3);         // thick line
            DrawLine(dragging_x1, dragging_y1, dragging_x2, dragging_y2);
            fl_line_style(0);
        }
    }
    fl_pop_clip();
}

/// FLTK event handler for the Fl_OpDesk.
int Fl_OpDesk::handle(int e) {
    //extern char *fl_eventnames[]; fprintf(stderr, "EVENT=%d (%s)\n", e, fl_eventnames[e]);
    int ret = Fl_Scroll::handle(e);
    int isleft = Fl::event_button1();
    switch ( e ) {
        case FL_PUSH:
            if ( ! ret && isleft ) {
                DeselectAll();
                redraw();
                ret = 1;
            }
            break;
        case FL_DRAG:
            if ( isleft ) {
                // TODO: DRAG SELECTION
                ret = 1;
            }
            break;
        case FL_RELEASE:
            if ( isleft ) {
                // TODO: DRAG SELECTION
                ret = 1;
            }
            break;
        case FL_KEYDOWN:
            if ( Fl::event_key() == FL_Delete ) {
                DeleteSelected();
                ret = 1;
            }
            break;
        case FL_KEYUP:
            if ( Fl::event_key() == FL_Delete ) {
                ret = 1;
            }
            break;
        case FL_FOCUS:
        case FL_UNFOCUS:
            ret = 1;
            break;
    }
    return(ret);
}

/// Return the index for the given Fl_OpBox \p box.
///    Returns -1 if not found.
///
int Fl_OpDesk::GetOpBoxIndex(const Fl_OpBox *box) const {
    for ( size_t t=0; t<boxes.size(); t++ ) {
        if ( GetOpBox(t) == box ) return(t);
    }
    return(-1);
}

/// Return the total number of boxes.
int Fl_OpDesk::GetOpBoxTotal() const {
    return(boxes.size());
}

/// Return a particular box specified by \p index.
Fl_OpBox* Fl_OpDesk::GetOpBox(int index) {
    return(boxes[index]);
}

/// Returns a const box specified by \p index.
const Fl_OpBox* Fl_OpDesk::GetOpBox(int index) const {
    return((const Fl_OpBox*)boxes[index]);
}

/// Get spacing between connection lines (in pixels).
int Fl_OpDesk::GetOpConnectSpacing() const {
    return(opconnect_spacing);
}

/// Set spacing between connection lines to \p val (in pixels).
void Fl_OpDesk::SetOpConnectSpacing(int val) {
    opconnect_spacing = val;
}

/// Get default width of connection lines (in pixels).
/// This is the default width used for all new connections created
/// with Fl_OpDesk::Connect().
///
int Fl_OpDesk::GetOpConnectWidth() const {
    return(opconnect_width);
}

/// Set default width of connection lines to \p val (in pixels).
/// This is the default width used for all new connections created
/// with Fl_OpDesk::Connect().
///
void Fl_OpDesk::SetOpConnectWidth(int val) {
    opconnect_width = val;
}

/// Get default color of connection lines.
Fl_Color Fl_OpDesk::GetOpConnectColor() const {
    return(opconnect_color);
}

/// Set default color of connection lines to \p val.
void Fl_OpDesk::SetOpConnectColor(Fl_Color val) {
    opconnect_color = val;
}

/// Deselect all boxes.
/// Marks desk for redraw if any changed.
/// \returns Number of boxes whose selection was actually changed, 
///          or zero if none.
///
int Fl_OpDesk::DeselectAll() {
    int changed = 0;
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *b = GetOpBox(t);
        if ( b->GetSelected() ) {
            b->SetSelected(0);
            ++changed;
        }
    }
    if ( changed ) redraw(); 
    return(changed);
}

/// Select all boxes
/// \returns Number of boxes whose selection was actually changed, 
///          or zero if none. Handles redraw if any changes occurred.
///
int Fl_OpDesk::SelectAll() {
    int changed = 0;
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *b = GetOpBox(t);
        if ( b->label() == 0 ) continue;
        if ( ! b->GetSelected() ) {
            b->SetSelected(1);
            ++changed;
        }
    }
    if ( changed ) redraw(); 
    return(changed);
}

/// INTERNAL: Add connect of two buttons \p srcbut and \p dstbut
///    to our internal array.
///    Doesn't recurse to telling other buttons, local only.
///    FOR INTERNAL USE ONLY.
///    \returns 0 if OK. 
///    \returns 1 if already exists.
///    \returns -1 on error, \p errmsg has reason.
///
int Fl_OpDesk::_ConnectOnly(Fl_OpButton *srcbut, Fl_OpButton *dstbut, std::string &errmsg) {
    // See if we already have the connection listed
    // If so, ignore request to avoid recursion.
    //
    for ( int t=0; t<GetConnectionsTotal(); t++ )
        if ( GetConnection(t)->AreConnected(srcbut, dstbut) )
            return(1);

    // Add connection to connects[] (to avoid recursion)
    Fl_OpConnect op(srcbut, dstbut);
    op.SetWidth(opconnect_width);       // use default, user can change later
    op.SetColor(opconnect_color);       // use default, user can change later
    connects.push_back(op);

    return(0);
}

/// Connect two buttons: given two button pointers \p srcbut, \p dstbut.
/// Handles adding the connection to the connects[] array,
/// and telling the buttons about each other.
/// \returns 0 if OK.
/// \returns 1 if connection already exists.
/// \returns -1 if an error occurred, \p errmsg has reason.
///
int Fl_OpDesk::Connect(Fl_OpButton *srcbut, Fl_OpButton *dstbut, std::string &errmsg) {
    if ( srcbut->GetButtonType() == FL_OP_INPUT_BUTTON ) {
        // SWAP -- SRCBUT SHOULD BE OUTPUT
        Fl_OpButton *tmp = srcbut;
        srcbut = dstbut;
        dstbut = tmp;
    }

    // Handle internal connections
    switch ( _ConnectOnly(srcbut, dstbut, errmsg) ) {
      case -1: return(-1);      // ERROR
      case  1: return(1);       // ALREADY CONNECTED
      case  0: break;           // SUCCESS
    }

    // Now tell the buttons about each other
    //     Do AFTER adding to the connects[] array to avoid recursion;
    //     Fl_OpButton::ConnectTo() will try to tell us about the connection.
    //
    if ( srcbut->ConnectTo(dstbut, errmsg) < 0 ) {
        ConnectionError(srcbut, dstbut, errmsg);        // show error to user
        connects.pop_back();                            // undo add to connects[]
        return(-1);
    }

    return(0);
}

/// Connect two buttons given box pointers \p srcbox and \p dstbox,
/// and button label names \p srcbut_lname and \p dstbut_lname.
/// Handles adding the connection to the connects[] array,
/// and telling the buttons about each other.
/// \returns 0 if OK.
/// \returns -1 on error, \p errmsg has reason.
///
int Fl_OpDesk::Connect(Fl_OpBox *srcbox, const std::string& srcbut_lname, 
                       Fl_OpBox *dstbox, const std::string& dstbut_lname,
                       std::string &errmsg) {
    Fl_OpButton *srcbut, *dstbut;

    if ( ( srcbut = srcbox->FindButtonByLabel(srcbut_lname, errmsg) ) == 0 ) {
        errmsg = std::string("can't connect ") +
                 std::string(srcbox->label()) +
                 std::string("(") +
                 std::string(srcbut_lname) +
                 std::string(") to ") +
                 std::string(dstbox->label()) +
                 std::string("(") +
                 std::string(dstbut_lname) +
                 std::string("): ") +
                 std::string(errmsg);
        return(-1);
    }
    if ( ( dstbut = dstbox->FindButtonByLabel(dstbut_lname, errmsg) ) == 0 ) {
        errmsg = std::string("can't connect ") +
                 std::string(srcbox->label()) +
                 std::string("(") +
                 std::string(srcbut_lname) +
                 std::string(") to ") +
                 std::string(dstbox->label()) +
                 std::string("(") +
                 std::string(dstbut_lname) +
                 std::string("): ") +
                 std::string(errmsg);
        return(-1);
    }
    if ( Connect(srcbut, dstbut, errmsg) < 0 ) {
        return(-1);
    }
    return(0);
}

/// INTERNAL: Disconnect buttons \p a and \p b from our internal list.
/// Doesn't tell other buttons.
/// FOR INTERNAL USE ONLY.
/// \returns 0 if not found.
/// \returns >0 which is the number of connections that were removed.
///
int Fl_OpDesk::_DisconnectOnly(Fl_OpButton *a, Fl_OpButton *b) {
    int count = 0;
    for ( int t=0; t<GetConnectionsTotal(); t++ ) {
        if ( GetConnection(t)->AreConnected(a,b) ) {
            connects.erase(connects.begin() + t);
            --t;
            ++count;
        }
    }
    return(count);
}

/// Disconnect two buttons \p a and \p b.
/// The correct way to cleanly disconnect two buttons.
/// \returns 0 if not found.
/// \returns >0 which is the number of connections that were removed.
///
int Fl_OpDesk::Disconnect(Fl_OpButton *a, Fl_OpButton *b) {
    // Remove connection from our array
    int count = _DisconnectOnly(a,b);
    // Tell buttons to disconnect each other
    if ( a && b ) {
        a->_DisconnectOnly(b);
        b->_DisconnectOnly(a);
    }
    return(count);
}

/// Disconnect all connections to/from \p box.
/// \returns 0 if not found.
/// \returns >0, which is the the number of connections that were removed.
///
int Fl_OpDesk::Disconnect(Fl_OpBox *box) {
    int count = 0;
    for ( int t=0; t<GetConnectionsTotal(); t++ ) {
        Fl_OpButton *src = connects[t].GetSrcButton();
        Fl_OpButton *dst = connects[t].GetDstButton();
        if ( !src || !dst ) continue;
        if ( src->GetOpBox() == box || dst->GetOpBox() == box ) {
            Disconnect(src,dst);
            --t;
        }
    }
    return(count);
}

/// Disconnect all connections.
void Fl_OpDesk::DisconnectAll() {
    while ( GetConnectionsTotal() > 0 ) {
        int t = 0;
        Fl_OpButton *src = connects[t].GetSrcButton();
        Fl_OpButton *dst = connects[t].GetDstButton();
        Disconnect(src,dst);
    }
}

/// Return total number of button connections.
int Fl_OpDesk::GetConnectionsTotal() const {
    return(connects.size());
}

/// Return a particular button connection specified by \p index.
Fl_OpConnect* Fl_OpDesk::GetConnection(int index) {
    return(&(connects[index]));
}

/// Return the "last" button connection in the connection array.
///    Returns 0 if empty list.
///
Fl_OpConnect* Fl_OpDesk::GetLastOpConnect() {
    if ( connects.size() < 1 ) return(0);
    return(&(connects[connects.size()-1]));
}

/// Bring the specified box 'to the front', above the other boxes.
/// This changes the widget order in fltk so that the box is drawn last.
///
void Fl_OpDesk::BringToFront(Fl_OpBox *box) {
    Fl_Widget *w = (Fl_Widget*)box;
    int index = children() - 2;         // -2: skip Fl_Scroll's h+v scrollbars
    if ( index < 0 ) return;            // should never happen
    insert(*w, index);                  // move box to end of Fl_Group (but before scrollbars)
}

/// Delete the \p box from the desk.
/// Cuts all connections, and FLTK's event loop handles scheduling 
/// the actual destruction of the widget.
///
void Fl_OpDesk::DeleteBox(Fl_OpBox *box) {
    Disconnect(box);            // disconnect all connections
    remove(box);                // fltk: remove from group
    Fl::delete_widget(box);     // fltk: schedule delete of widget
    _RemoveBox(box);            // remove from boxes and clipboard
    redraw();
}

/// Delete all the selected boxes.
/// Returns how many boxes were deleted.
///
int Fl_OpDesk::DeleteSelected() {
    int count = 0;
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *b = GetOpBox(t);
        if ( b->GetSelected() ) {
            DeleteBox(b);
            --t;
            ++count;
        }
    }
    return(count);
}

/// Cut the selected boxes to the paste buffer.
/// Returns how many boxes were cut.
///
/// TBD: Paste buffer not yet implemented.
///
int Fl_OpDesk::CutSelected() {
    box_clipboard.clear();
    fl_alert("CutSelected() not implemented -- using DeleteSelected() instead");        //HACK
    return(DeleteSelected());                   // TBD: for now, use delete
}

/// Copy the selected boxes to the paste buffer.
/// Returns how many boxes were copied.
///
int Fl_OpDesk::CopySelected() {
    box_clipboard.clear();
    int count = 0;
    // Copy selected boxes
    for ( int t=0; t<GetOpBoxTotal(); t++ ) { 
        Fl_OpBox *b = GetOpBox(t);
        if ( b->GetSelected() ) {
            box_clipboard.push_back(b);
            ++count;
        }
    }
    return(count);
}

/// Virtual method for handling 'paste' operations.
///     Pastes the boxes/connections from the clipboard.
///     Returns how many new boxes were pasted.
///
int Fl_OpDesk::PasteSelected() {

    // Deselect all boxes first.
    //    This way if we do two pastes in a row (with no intermediate copy)
    //    only the last paste remains selected.
    //
    DeselectAll();

    // Copy the boxes
    //    Keep track of original vs. copy, so when we copy
    //    the connections, we know which to make for which.
    //
    std::vector<Fl_OpBox*> copyboxes;
    int count = 0;
    for ( size_t t=0; t<box_clipboard.size(); t++ ) {
        Fl_OpBox *orig = box_clipboard[t];
        begin();
            Fl_OpBox *copy = new Fl_OpBox(*orig);
            copy->position(copy->x()+100, copy->y()+100);
            copy->CopyButtons(*orig);
            copy->SetSelected(1);               // leave copy selected
            // Keep track of original vs. copy
            copyboxes.push_back(copy);
        end();
        ++count;
    }

    // Make copies of connections
    CopyConnections(box_clipboard, copyboxes);

    // Update fltk
    redraw();
    return(count);
}

/// Copy connections in \p origboxes array to \p copyboxes array.
///     The arrays must be equal in size; the 'copyboxes' array should
///     contain mirror copies of the 'origboxes' array.
///
///     Look at the connections in the original boxes,
///     and make similar connections (connect same buttons)
///     in the copies.
///
///     Only make copies of connections that are between
///     boxes we're copying. (Don't copy connections to
///     boxes that aren't part of the copy)
///
void Fl_OpDesk::CopyConnections(std::vector<Fl_OpBox*> origboxes, std::vector<Fl_OpBox*> copyboxes) {
    if ( origboxes.size() != copyboxes.size() ) return;

    // Loop through all boxes that were copied
    //     We can't copy the connections while making the boxes
    //     because they're not all there yet, so we have to wait
    //     until all the copies are made first.
    //
    std::string errmsg;
    for ( size_t ibox=0; ibox<origboxes.size(); ibox++ ) {
        Fl_OpBox *box = origboxes[ibox];
        // Loop through box's buttons to make copy of all connections for each
        for ( int ibut=0; ibut<box->GetTotalButtons(); ibut++ ) {
            Fl_OpButton *srcbut_orig = box->GetButton(ibut); 
            for ( size_t ci=0; ci<srcbut_orig->GetTotalConnectedButtons(); ci++ ) {
                Fl_OpButton *dstbut_orig = srcbut_orig->GetConnectedButton(ci); // original's dst button
                Fl_OpConnect *origcon = srcbut_orig->GetConnection(ci);         // original's connection
                Fl_OpBox *srcbox_orig = srcbut_orig->GetOpBox();                // original's src box
                Fl_OpBox *dstbox_orig = dstbut_orig->GetOpBox();                // original's dst box

                // Copy the connection IF the dst box is one of the ones we're copying
                int found = 0;
                for ( size_t iob=0; iob<origboxes.size(); iob++ ) {
                    if ( dstbox_orig == origboxes[iob] ) {
                        found = 1;
                        break;
                    }
                }
                if ( !found ) continue;

                // Find the corresponding copy for original src/dst *boxes*
                Fl_OpBox *srcbox_copy = 0;
                Fl_OpBox *dstbox_copy = 0;
    for ( size_t i=0; i<origboxes.size(); i++ ) {
                    if ( srcbox_orig == origboxes[i] ) { srcbox_copy = copyboxes[i]; }
                    if ( dstbox_orig == origboxes[i] ) { dstbox_copy = copyboxes[i]; }
                }
                if ( !srcbox_copy || !dstbox_copy) continue;                    // not found? shouldn't happen

                // Find corresponding copy for original src/dst *buttons*
                Fl_OpButton *srcbut_copy = srcbox_copy->FindButtonByLabel(srcbut_orig->label(), errmsg);
                Fl_OpButton *dstbut_copy = dstbox_copy->FindButtonByLabel(dstbut_orig->label(), errmsg);
                if ( !srcbut_copy || !dstbut_copy ) continue;                   // not found? shouldn't happen

                // Now that we know which two buttons in the copy we need to connect, make the connection!
                switch ( Connect(srcbut_copy, dstbut_copy, errmsg) ) {
                    case 0:     // SUCCESS
                    {
                        // Copy in the attributes of the orig connection to the new one
                        Fl_OpConnect *copycon = GetLastOpConnect();
                        if ( copycon ) copycon->CopyAttributes(origcon);
                        break;
                    }

                    case 1:     // CONNECTION EXISTS
                        break;  // We should expect these and ignore them

                    case -1:    // CONNECTION FAILED
                        // Complain to stderr.
                        // Arguably, we should accumulate and return such errors.
                        //
                        fprintf(stderr, "WARNING: Could not copy connects for '%s' <-> '%s': %s\n",
                            srcbut_copy->GetFullName().c_str(),
                            dstbut_copy->GetFullName().c_str(),
                            errmsg.c_str());
                        break;
                }
            }
        }
    }
}

/// INTERNAL: Handles a user interactively dragging out a connection.
///           Assumes Fl::event_x() and Fl::event_y() have current mouse position.
///           If \p b is NULL, stop any existing dragging operations.
///
void Fl_OpDesk::DraggingConnection(Fl_OpButton *srcbut) {
    if ( srcbut ) {
        // ENABLE DRAGGING, TELL DESK TWO CONNECTION POINTS TO DRAW
        dragging = 1;
        dragging_x1 = srcbut->x() + srcbut->w()/2;
        dragging_y1 = srcbut->y() + srcbut->h()/2;
        dragging_x2 = Fl::event_x();
        dragging_y2 = Fl::event_y();
    } else {
        // DISABLE DRAGGING
        dragging = 0;
    }
    redraw();
}

/// INTERNAL: Handles a user interactively dragging selected boxes around.
void Fl_OpDesk::DraggingBoxes(int xdiff, int ydiff) {
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *b = GetOpBox(t);
        if ( b->GetSelected() ) {
            b->position(b->x() + xdiff, b->y() + ydiff);
        }
    }
    init_sizes();
    redraw();
}

/// INTERNAL: Find a button that is under the mouse
Fl_OpButton* Fl_OpDesk::FindButtonUnderMouse() {
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *box = GetOpBox(t);
        Fl_OpButton *but = box->FindButtonUnderMouse();
        if ( but ) return(but);
    }
    return(0);
}

/// Clear the entire desk; delete all boxes/connections.
void Fl_OpDesk::Clear() {
    while ( GetOpBoxTotal() > 0 ) {
        Fl_OpBox *box = GetOpBox(0);
        DeleteBox(box);
    }
    // SelectAll();
    // DeleteSelected();
    // boxes.clear();
    // connects.clear();
    window()->redraw();
}

/// Find a box given its label name, \p lname.
/// \returns
///     valid box pointer if found, or NULL if not found (errmsg has reason).
///
Fl_OpBox* Fl_OpDesk::FindBoxByLabel(std::string& lname, std::string& errmsg) {
    for ( int t=0; t<GetOpBoxTotal(); t++ ) {
        Fl_OpBox *box = GetOpBox(t);
        if ( box->label() && lname == box->label() ) {
            return(box);
        }
    }
    errmsg = std::string("box '") + std::string(lname) +
             std::string("' not found");
    return(NULL);
}

/// Parse separate box and button names from a 'full button name', eg. "boxname(butname)".
///
/// \b Example:
/// \code
///    IN: fullname = "add(A)"
///   OUT: boxname = "add";
///        butname = "A";
///    }
/// \endcode
/// \returns 0 if OK, with \p boxname and \p butname set to the parsed names.
/// \returns -1 if \p fullname could not be parsed.
///
int Fl_OpDesk::ParseFullButtonName(const std::string &fullname,
                                   std::string &boxname,
                                   std::string &butname,
                                   std::string &errmsg) {
    char a[80], b[80];          // TODO: use std::string + split
    if ( sscanf(fullname.c_str(), "%79[^(](%79[^)])", a, b) == 2 ) {
        boxname = a;
        butname = b;
        return(0);
    }
    errmsg = std::string("'") +
             std::string(fullname) +
             std::string("': bad button name (expected 'box(but)' style name)");
    return(-1);
}

/// Return the box pointer for the given \p fullname.
/// \returns
///     valid box pointer if found, or NULL if not found (errmsg has reason).
///
Fl_OpBox* Fl_OpDesk::FindBoxForFullName(const std::string& fullname, std::string& errmsg) {
    std::string boxname, butname;
    if ( ParseFullButtonName(fullname, boxname, butname, errmsg) < 0 ) return(0);
    return(FindBoxByLabel(boxname, errmsg));
}

/// Return the button pointer for the given \p fullname.
/// \returns
///     valid button pointer if found, or NULL if not found (errmsg has reason).
///
Fl_OpButton* Fl_OpDesk::FindButtonForFullName(const std::string& fullname, std::string &errmsg) {
    std::string boxname, butname;
    if ( ParseFullButtonName(fullname, boxname, butname, errmsg) < 0 ) {
        return(NULL);
    }
    Fl_OpBox *box = FindBoxByLabel(boxname, errmsg);
    if ( !box ) { return(NULL); }
    return(box->FindButtonByLabel(butname, errmsg));
}

/// Connect two buttons given their full names ("box(butt)") for \p src_name and \p dst_name.
/// Handles adding the connection to the connects[] array,
/// and telling the buttons about each other.
/// \returns 0 if OK
/// \returns 1 if connection already exists.
/// \returns -1 if an error occurred, \p errmsg has reason.
///
int Fl_OpDesk::Connect(const std::string& src_name, 
                    const std::string& dst_name,
                    std::string &errmsg) {
    Fl_OpButton *srcbut = FindButtonForFullName(src_name, errmsg);
    if ( !srcbut ) return(-1);

    Fl_OpButton *dstbut = FindButtonForFullName(dst_name, errmsg);
    if ( !dstbut ) return(-1);

    return(Connect(srcbut, dstbut, errmsg));
}

/// INTERNAL: Add a box to the boxes array. (Used by Fl_OpBox ctor/dtors)
void Fl_OpDesk::_AddBox(Fl_OpBox *b) {
    boxes.push_back(b);
}

/// INTERNAL: Remove a box from internal boxes and clipboard arrays. (Used by Fl_OpBox ctor/dtors)
void Fl_OpDesk::_RemoveBox(Fl_OpBox *b) {
    // Remove box from boxes array
    for ( size_t t=0; t<boxes.size(); t++ ) {
        if ( boxes[t] == b ) {
            boxes.erase(boxes.begin() + t);
            break;
        }
    }
    // Remove box from clipboard
    for ( size_t t=0; t<box_clipboard.size(); t++ ) {
        if ( box_clipboard[t] == b ) {
            box_clipboard.erase(box_clipboard.begin() + t);
            break;
        }
    }
}

/// Return the Fl_OpConnect* connection between \p srcbut and \p dstbut.
///
/// \returns
///    Valid Fl_OpConnect* on success,
///    or NULL if there's no existing connection between the two buttons.
///
Fl_OpConnect* Fl_OpDesk::GetConnection(Fl_OpButton *srcbut, Fl_OpButton *dstbut) {
    for ( size_t t=0; t<connects.size(); t++ ) {
        Fl_OpConnect &conn = connects[t];
        if ( (conn.GetSrcButton() == srcbut && conn.GetDstButton() == dstbut) ||        // normal, or..
             (conn.GetSrcButton() == dstbut && conn.GetDstButton() == srcbut) ) {       // ..swapped?
            return(&conn);
        }
    }
    return(0);
}

/// Get the current connection line drawing style.
Fl_OpConnectStyle Fl_OpDesk::GetConnectStyle(void) const {
    return(opconnect_style);
}

/// Set the current connection line drawing style to \p val.
///    Handles redraw()ing the desktop.
///
void Fl_OpDesk::SetConnectStyle(Fl_OpConnectStyle val) {
    opconnect_style = val;
    redraw();
}

/// Get the OpBox selected border size.
///    This is the thickness of the border drawn around boxes
///    when they're selected.
///
int Fl_OpDesk::GetOpBoxSelectedBorderSize() const {
    return(opbox_selectedbordersize);
}

/// Set the OpBox selected border size.
///    This is the thickness of the border drawn around boxes
///    when they're selected.
///
void Fl_OpDesk::SetOpBoxSelectedBorderSize(int val) {
    opbox_selectedbordersize = val;
}
