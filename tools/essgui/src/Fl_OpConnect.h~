#ifndef _FL_OPCONNECT_H_
#define _FL_OPCONNECT_H_

//////////////////////
// Fl_OpConnect.H
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
/// \brief This class handles connections between two Fl_OpButton's
///

#include "Fl_OpButton.H"

class Fl_OpButton;

/// \class Fl_OpConnect
///
/// \brief This encapsulates a single connection between two buttons.
///
/// This implements a connection beween one output button
/// of an opbox, and one input button of another opbox.
/// For connections from one button to many, use multiple
/// instances of Fl_OpConnect for each separate connection.
///
/// The Fl_OpDesk keeps an array of these, so it can keep track
/// of all connections for the purposes of drawing, and for
/// the application to be able to easily loop through for code
/// generation purposes.
///
class Fl_OpConnect {
    Fl_OpButton *srcbut;                // source button
    Fl_OpButton *dstbut;                // destination button
    Fl_Color  color;            // connection color
    int       width;            // connection width

public:
    /// Fl_OpConnect constructor. Establishes a connection between buttons \p a and \p b.
    Fl_OpConnect(Fl_OpButton *a, Fl_OpButton *b) {
        srcbut = a;
        dstbut = b;
        color = Fl_Color(0xf0d0a000);           // whiteish
        width = 2;
    }

    /// Assignment operator, assigning one Fl_OpConnect to another.
    void operator=(const Fl_OpConnect& o) {
        srcbut = o.srcbut;
        dstbut = o.dstbut;
        color = o.color;
        width = o.width;
    }

    /// Copy constructor.
    Fl_OpConnect(const Fl_OpConnect& o) {
        *this = o;      // let assignment handle copy
    }

    /// See if button \p find is one of the buttons we're connected to.
    /// \returns 1 if it's connected, 0 if not.
    ///
    int IsButtonConnected(Fl_OpButton *find) {
        return((srcbut==find || dstbut==find) ? 1 : 0);
    }

    /// Are the two buttons \p a and \p b the ones we're connecting?
    /// \returns 1 if we show them connected, 0 if not.
    ///
    int AreConnected(Fl_OpButton *a, Fl_OpButton *b) {
       return( ((srcbut==a && dstbut==b) || (srcbut==b && dstbut==a)) ? 1 : 0);
    }

    /// Return the source button.
    Fl_OpButton *GetSrcButton() const {
        return(srcbut);
    }

    /// Return destination button
    Fl_OpButton *GetDstButton() const {
        return(dstbut);
    }

    /// Get the color of this connection's line(s).
    Fl_Color GetColor() const {
        return(color);
    }

    /// Set the color of this connection's line(s).
    void SetColor(Fl_Color val) {
        color = val;
    }

    /// Get the line width (thickness) of this connection's line(s).
    int GetWidth() const {
        return(width);
    }

    /// Set the line width (thickness) for this connection's line(s).
    void SetWidth(int val) {
        width = val;
    }

    /// Copy the attributes (color, width, etc) from \p old connection to the current.
    void CopyAttributes(Fl_OpConnect *old) {
        SetColor(old->GetColor());
        SetWidth(old->GetWidth());
    }
};
#endif /* _FL_OPCONNECT_H_ */
