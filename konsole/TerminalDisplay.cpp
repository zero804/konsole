/*
    This file is part of Konsole, a terminal emulator for KDE.
    
    Copyright (C) 2006 by Robert Knight <robertknight@gmail.com>
    Copyright (C) 1997,1998 by Lars Doelle <lars.doelle@on-line.de>
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

/*! \class TerminalDisplay

    \brief Visible screen contents

   This class is responsible to map the `image' of a terminal emulation to the
   display. All the dependency of the emulation to a specific GUI or toolkit is
   localized here. Further, this widget has no knowledge about being part of an
   emulation, it simply work within the terminal emulation framework by exposing
   size and key events and by being ordered to show a new image.

   <ul>
   <li> The internal image has the size of the widget (evtl. rounded up)
   <li> The external image used in setImage can have any size.
   <li> (internally) the external image is simply copied to the internal
        when a setImage happens. During a resizeEvent no painting is done
        a paintEvent is expected to follow anyway.
   </ul>

   \sa Screen \sa Emulation
*/

// System
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// Qt
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegExp>
#include <QResizeEvent>
#include <QSpacerItem>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

// KDE
#include <KRun>
#include <KCursor>
#include <kdebug.h>
#include <KLocale>
#include <KNotification>
#include <KGlobalSettings>
#include <KShortcut>
#include <KIO/NetAccess>

// Konsole
#include "config.h"
#include "Filter.h"
#include "TerminalDisplay.h"
#include "konsole_wcwidth.h"
#include "ScreenWindow.h"

using namespace Konsole;

#ifndef loc
#define loc(X,Y) ((Y)*columns+(X))
#endif

#define SCRWIDTH 16 // width of the scrollbar

#define yMouseScroll 1

#define REPCHAR   "ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
                  "abcdefgjijklmnopqrstuvwxyz" \
                  "0123456789./+@"

extern bool true_transparency; // declared in main.characterpp and konsole_part.characterpp

// scroll increment used when dragging selection at top/bottom of window.

// static
bool TerminalDisplay::s_antialias = true;
bool TerminalDisplay::s_standalone = false;

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Colors                                     */
/*                                                                           */
/* ------------------------------------------------------------------------- */

//FIXME: the default color table is in session.C now.
//       We need a way to get rid of this one, here.


/* Note that we use ANSI color order (bgr), while IBMPC color order is (rgb)

   Code        0       1       2       3       4       5       6       7
   ----------- ------- ------- ------- ------- ------- ------- ------- -------
   ANSI  (bgr) Black   Red     Green   Yellow  Blue    Magenta Cyan    White
   IBMPC (rgb) Black   Blue    Green   Cyan    Red     Magenta Yellow  White
*/

ScreenWindow* TerminalDisplay::screenWindow() const
{
    return _screenWindow;
}
void TerminalDisplay::setScreenWindow(ScreenWindow* window)
{
    // disconnect existing screen window if any
    if ( _screenWindow )
    {
        disconnect( _screenWindow , 0 , this , 0 );
    }

    _screenWindow = window;

#warning "The order here is not specified - does it matter whether updateImage or updateLineProperties comes first?"
    connect( _screenWindow , SIGNAL(outputChanged()) , this , SLOT(updateLineProperties()) );
    connect( _screenWindow , SIGNAL(outputChanged()) , this , SLOT(updateImage()) );
}

void TerminalDisplay::setDefaultBackColor(const QColor& color)
{
  defaultBgColor = color;
  
  QPalette p = palette();
  p.setColor( backgroundRole(), getDefaultBackColor() );
  setPalette( p );
}

QColor TerminalDisplay::getDefaultBackColor()
{
  if (defaultBgColor.isValid())
    return defaultBgColor;
  return color_table[DEFAULT_BACK_COLOR].color;
}

const ColorEntry* TerminalDisplay::colorTable() const
{
  return color_table;
}

void TerminalDisplay::setColorTable(const ColorEntry table[])
{
  for (int i = 0; i < TABLE_COLORS; i++) color_table[i] = table[i];
 
  const QPixmap* pm = 0; 
  if (!pm)
  {
    if (!true_transparency || (qAlpha(blend_color) == 0xff))
    {
        QPalette p = palette();
        p.setColor( backgroundRole(), getDefaultBackColor() );
        setPalette( p );
    } else {

        //### probably buggy
        QPalette p = palette();
        p.setColor( backgroundRole(), blend_color );
        setPalette( p );
    }
  }
  update();
}

//FIXME: add backgroundPixmapChanged.

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Font                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*
   The VT100 has 32 special graphical characters. The usual vt100 extended
   xterm fonts have these at 0x00..0x1f.

   QT's iso mapping leaves 0x00..0x7f without any changes. But the graphicals
   come in here as proper unicode characters.

   We treat non-iso10646 fonts as VT100 extended and do the requiered mapping
   from unicode to 0x00..0x1f. The remaining translation is then left to the
   QCodec.
*/

static inline bool isLineChar(Q_UINT16 c) { return ((c & 0xFF80) == 0x2500);}
static inline bool isLineCharString(const QString& string)
{
		return (string.length() > 0) && (isLineChar(string.at(0).unicode()));
}
						

// assert for i in [0..31] : vt100extended(vt100_graphics[i]) == i.

unsigned short Konsole::vt100_graphics[32] =
{ // 0/8     1/9    2/10    3/11    4/12    5/13    6/14    7/15
  0x0020, 0x25C6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
  0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
  0xF800, 0xF801, 0x2500, 0xF803, 0xF804, 0x251c, 0x2524, 0x2534,
  0x252c, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00b7
};

/*
static QChar vt100extended(QChar c)
{
  switch (c.unicode())
  {
    case 0x25c6 : return  1;
    case 0x2592 : return  2;
    case 0x2409 : return  3;
    case 0x240c : return  4;
    case 0x240d : return  5;
    case 0x240a : return  6;
    case 0x00b0 : return  7;
    case 0x00b1 : return  8;
    case 0x2424 : return  9;
    case 0x240b : return 10;
    case 0x2518 : return 11;
    case 0x2510 : return 12;
    case 0x250c : return 13;
    case 0x2514 : return 14;
    case 0x253c : return 15;
    case 0xf800 : return 16;
    case 0xf801 : return 17;
    case 0x2500 : return 18;
    case 0xf803 : return 19;
    case 0xf804 : return 20;
    case 0x251c : return 21;
    case 0x2524 : return 22;
    case 0x2534 : return 23;
    case 0x252c : return 24;
    case 0x2502 : return 25;
    case 0x2264 : return 26;
    case 0x2265 : return 27;
    case 0x03c0 : return 28;
    case 0x2260 : return 29;
    case 0x00a3 : return 30;
    case 0x00b7 : return 31;
  }
  return c;
}

static QChar identicalMap(QChar c)
{
  return c;
}
*/

void TerminalDisplay::fontChange(const QFont &)
{
  QFontMetrics fm(font());
  font_h = fm.height() + m_lineSpacing;

  // waba TerminalDisplay 1.123:
  // "Base character width on widest ASCII character. This prevents too wide
  //  characters in the presence of double wide (e.g. Japanese) characters."
  // Get the width from representative normal width characters
  font_w = qRound((double)fm.width(REPCHAR)/(double)strlen(REPCHAR));

  fixed_font = true;
  int fw = fm.width(REPCHAR[0]);
  for(unsigned int i=1; i< strlen(REPCHAR); i++){
    if (fw != fm.width(REPCHAR[i])){
      fixed_font = false;
      break;
  }
  }

  if (font_w>200) // don't trust unrealistic value, fallback to QFontMetrics::maxWidth()
    font_w=fm.maxWidth();
  if (font_w<1)
    font_w=1;

  font_a = fm.ascent();
//printf("font: %s\n", font().toString().toLatin1().constData());
//printf("fixed: %s\n", font().fixedPitch() ? "yes" : "no");
//printf("fixed_font: %d\n", fixed_font);
//printf("font_h: %d\n",font_h);
//printf("font_w: %d\n",font_w);
//printf("fw: %d\n",fw);
//printf("font_a: %d\n",font_a);
//printf("rawname: %s\n",font().renditionawName().toAscii().constData());

/*
#if defined(Q_CC_GNU)
#warning TODO: Review/fix vt100 extended font-mapping
#endif
*/

//  fontMap = identicalMap;
  emit changedFontMetricSignal( font_h, font_w );
  propagateSize();
  update();
}

void TerminalDisplay::setVTFont(const QFont& f)
{
  QFont font = f;

  QFontMetrics metrics(font);

  if ( metrics.height() < height() && metrics.maxWidth() < width() )
  {
    if (!s_antialias)
        font.setStyleStrategy( QFont::NoAntialias );
  
    QFrame::setFont(font);
    fontChange(font);
  }
}

void TerminalDisplay::setFont(const QFont &)
{
  // ignore font change request if not coming from konsole itself
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                         Constructor / Destructor                          */
/*                                                                           */
/* ------------------------------------------------------------------------- */

TerminalDisplay::TerminalDisplay(QWidget *parent)
:QFrame(parent)
,_screenWindow(0)
,allowBell(true)
,gridLayout(0)
,font_h(1)
,font_w(1)
,font_a(1)
,lines(1)
,columns(1)
,usedLines(1)
,usedColumns(1)
,contentHeight(1)
,contentWidth(1)
,image(0)
,resizing(false)
,terminalSizeHint(false)
,terminalSizeStartup(true)
,bidiEnabled(false)
,actSel(0)
,word_selection_mode(false)
,line_selection_mode(false)
,preserve_line_breaks(true)
,column_selection_mode(false)
,scrollLoc(SCROLLBAR_NONE)
,word_characters(":@-./_~")
,m_bellMode(BELLSYSTEM)
,blinking(false)
,cursorBlinking(false)
,hasBlinkingCursor(false)
,ctrldrag(false)
,cuttobeginningofline(false)
,isPrinting(false)
,printerFriendly(false)
,printerBold(false)
,isFixedSize(false)
,m_drop(0)
,possibleTripleClick(false)
,mResizeWidget(0)
,mResizeLabel(0)
,mResizeTimer(0)
,outputSuspendedLabel(0)
,m_lineSpacing(0)
,colorsSwapped(false)
,rimX(1)
,rimY(1)
,m_imPreeditText(QString())
,m_imPreeditLength(0)
,m_imStart(0)
,m_imStartLine(0)
,m_imEnd(0)
,m_imSelStart(0)
,m_imSelEnd(0)
,m_cursorLine(0)
,m_cursorCol(0)
,m_isIMEdit(false)
,blend_color(qRgba(0,0,0,0xff))
,_filterChain(new TerminalImageFilterChain())
{
  // The offsets are not yet calculated.
  // Do not calculate these too often to be more smoothly when resizing
  // konsole in opaque mode.
  bY = bX = 1;

  //Selection is no longer cleared automatically 
  //when the selection changes
  //
  //cb = QApplication::clipboard();
  //QObject::connect( (QObject*)cb, SIGNAL(selectionChanged()),
  //                  this, SLOT(onClearSelection()) );

  scrollbar = new QScrollBar(this);
  scrollbar->setCursor( Qt::ArrowCursor );
  connect(scrollbar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));

  blinkT   = new QTimer(this);
  connect(blinkT, SIGNAL(timeout()), this, SLOT(blinkEvent()));
  blinkCursorT   = new QTimer(this);
  connect(blinkCursorT, SIGNAL(timeout()), this, SLOT(blinkCursorEvent()));

  setUsesMouse(true);
  setColorTable(base_color_table); // init color table

  qApp->installEventFilter( this ); //FIXME: see below
  KCursor::setAutoHideCursor( this, true );

  setMouseTracking(true);

  // Init DnD ////////////////////////////////////////////////////////////////
  setAcceptDrops(true); // attempt
  dragInfo.state = diNone;

  setFocusPolicy( Qt::WheelFocus );
  // im
  setAttribute(Qt::WA_InputMethodEnabled, true);

  //tell Qt to automatically fill the widget with the current background colour when
  //repainting.
  //the widget may then need to repaint over some of the area in a different colour
  //but because of the double buffering there won't be any flicker
  setAutoFillBackground(true);

  gridLayout = new QGridLayout(this);
  gridLayout->setMargin(0);

  setLayout( gridLayout ); 
  setLineWidth(0);

  //set up a warning message when the user presses Ctrl+S to avoid confusion
  connect( this,SIGNAL(flowControlKeyPressed(bool)),this,SLOT(outputSuspended(bool)) );
}

TerminalDisplay::~TerminalDisplay()
{
  qApp->removeEventFilter( this );
  if (image) free(image);

  delete gridLayout;
  delete outputSuspendedLabel;
  delete _filterChain;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                             Display Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/**
 A table for emulating the simple (single width) unicode drawing chars.
 It represents the 250x - 257x glyphs. If it's zero, we can't use it.
 if it's not, it's encoded as follows: imagine a 5x5 grid where the points are numbered
 0 to 24 left to top, top to bottom. Each point is represented by the corresponding bit.

 Then, the pixels basically have the following interpretation:
 _|||_
 -...-
 -...-
 -...-
 _|||_

where _ = none
      | = vertical line.
      - = horizontal line.
 */


enum LineEncode
{
    TopL  = (1<<1),
    TopC  = (1<<2),
    TopR  = (1<<3),

    LeftT = (1<<5),
    Int11 = (1<<6),
    Int12 = (1<<7),
    Int13 = (1<<8),
    RightT = (1<<9),

    LeftC = (1<<10),
    Int21 = (1<<11),
    Int22 = (1<<12),
    Int23 = (1<<13),
    RightC = (1<<14),

    LeftB = (1<<15),
    Int31 = (1<<16),
    Int32 = (1<<17),
    Int33 = (1<<18),
    RightB = (1<<19),

    BotL  = (1<<21),
    BotC  = (1<<22),
    BotR  = (1<<23)
};

#include "linefont.h"

static void drawLineChar(QPainter& paint, int x, int y, int w, int h, uchar code)
{
    //Calculate cell midpoints, end points.
    int cx = x + w/2;
    int cy = y + h/2;
    int ex = x + w - 1;
    int ey = y + h - 1;

    Q_UINT32 toDraw = LineChars[code];

    //Top lines:
    if (toDraw & TopL)
        paint.drawLine(cx-1, y, cx-1, cy-2);
    if (toDraw & TopC)
        paint.drawLine(cx, y, cx, cy-2);
    if (toDraw & TopR)
        paint.drawLine(cx+1, y, cx+1, cy-2);

    //Bot lines:
    if (toDraw & BotL)
        paint.drawLine(cx-1, cy+2, cx-1, ey);
    if (toDraw & BotC)
        paint.drawLine(cx, cy+2, cx, ey);
    if (toDraw & BotR)
        paint.drawLine(cx+1, cy+2, cx+1, ey);

    //Left lines:
    if (toDraw & LeftT)
        paint.drawLine(x, cy-1, cx-2, cy-1);
    if (toDraw & LeftC)
        paint.drawLine(x, cy, cx-2, cy);
    if (toDraw & LeftB)
        paint.drawLine(x, cy+1, cx-2, cy+1);

    //Right lines:
    if (toDraw & RightT)
        paint.drawLine(cx+2, cy-1, ex, cy-1);
    if (toDraw & RightC)
        paint.drawLine(cx+2, cy, ex, cy);
    if (toDraw & RightB)
        paint.drawLine(cx+2, cy+1, ex, cy+1);

    //Intersection points.
    if (toDraw & Int11)
        paint.drawPoint(cx-1, cy-1);
    if (toDraw & Int12)
        paint.drawPoint(cx, cy-1);
    if (toDraw & Int13)
        paint.drawPoint(cx+1, cy-1);

    if (toDraw & Int21)
        paint.drawPoint(cx-1, cy);
    if (toDraw & Int22)
        paint.drawPoint(cx, cy);
    if (toDraw & Int23)
        paint.drawPoint(cx+1, cy);

    if (toDraw & Int31)
        paint.drawPoint(cx-1, cy+1);
    if (toDraw & Int32)
        paint.drawPoint(cx, cy+1);
    if (toDraw & Int33)
        paint.drawPoint(cx+1, cy+1);

}

void TerminalDisplay::drawLineCharString(	QPainter& painter, int x, int y, const QString& str, 
									const Character* attributes)
{
		const QPen& currentPen = painter.pen();
		
		if ( attributes->rendition & RE_BOLD )
		{
			QPen boldPen(currentPen);
			boldPen.setWidth(3);
			painter.setPen( boldPen );
		}	
		
		for (int i=0 ; i < str.length(); i++)
		{
			uchar code = str[i].cell();
        	if (LineChars[code])
            	drawLineChar(painter, x + (font_w*i), y, font_w, font_h, code);
		}

		painter.setPen( currentPen );
}

//TODO
//The old version painted the text on a character-by-character basis, this is slow and should be 
//avoided if at all possible.
//
//Investigate:
//	- Why did the old version allow double the width for characters at column 0?  I cannot
//	see any obvious visual differences
// 
// -- Robert Knight <robertknight@gmail.com>

void TerminalDisplay::drawTextFixed(QPainter& painter, int x, int y, QString& str, const Character* /*attributes*/)
{
	if ( str.length() == 0 )
			return;
		
    painter.drawText( QRect( x, y, font_w*str.length(), font_h ),  Qt::TextDontClip , str );
}

//OLD VERSION
//

/*void TerminalDisplay::drawTextFixed(QPainter &paint, int x, int y,
                             QString& str, const Character *attr)
{
  QString drawstr;
  unsigned int nc=0;
  int w;
  for(int i=0;i<str.length();i++)
  {
    drawstr = str.at(i);
    // Add double of the width if next c is 0;
    if ((attr+nc+1)->c) // This may access image[image_size] See makeImage()
    {
      w = font_w;
      nc++;
    }
    else
    {
      w = font_w*2;
      nc+=2;
    }

    //Check for line-drawing char
    if (isLineChar(drawstr[0].unicode()))
    {
        uchar code = drawstr[0].characterell();
        if (LineChars[code])
        {
            drawLineChar(paint, x, y, w, font_h, code);
            x += w;
            continue;
        }
    }

    paint.drawText( QRect( x, y, w, font_h ), Qt::AlignHCenter | Qt::TextDontClip, drawstr );
    x += w;
  }
}*/


/*!
    attributed string draw primitive
*/

void TerminalDisplay::drawAttrStr(QPainter &paint, const QRect& rect,
                           QString& str, const Character *attr, bool pm, bool clear)
{

  //draw text fragment.
  //the basic process is:
  //	1.  save current state of painter
  //	2.  set painter properties and draw text
  //	3.  restore state of painter
  paint.save();

  int a = font_a + m_lineSpacing / 2;
  QColor fColor = printerFriendly ? Qt::black : attr->foregroundColor.color(color_table);
  QColor bColor = attr->backgroundColor.color(color_table);
  QString drawstr;

  if ((attr->rendition & RE_CURSOR) && !isPrinting)
    cursorRect = rect;

  // Paint background
  if (!printerFriendly)
  {
    if (attr->isTransparent(color_table))
    {
      if (pm)
        paint.setBackgroundMode( Qt::TransparentMode );
    }
    else
    {
      if (pm || clear || (blinking && (attr->rendition & RE_BLINK)) ||
          attr->backgroundColor == CharacterColor(COLOR_SPACE_DEFAULT, colorsSwapped ? DEFAULT_FORE_COLOR : DEFAULT_BACK_COLOR) )

        // draw background colors with 75% opacity
        if ( true_transparency && qAlpha(blend_color) < 0xff ) {
          QRgb col = bColor.rgb();

          Q_UINT8 salpha = 192;
          Q_UINT8 dalpha = 255 - salpha;

          int a, r, g, b;
          a = qMin( (qAlpha (col) * salpha) / 255 + (qAlpha (blend_color) * dalpha) / 255, 255 );
          r = qMin( (qRed   (col) * salpha) / 255 + (qRed   (blend_color) * dalpha) / 255, 255 );
          g = qMin( (qGreen (col) * salpha) / 255 + (qGreen (blend_color) * dalpha) / 255, 255 );
          b = qMin( (qBlue  (col) * salpha) / 255 + (qBlue  (blend_color) * dalpha) / 255, 255 );

          col = a << 24 | r << 16 | g << 8 | b;
          //int pixel = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);

          paint.fillRect(rect, QColor(col));
        } else
          paint.fillRect(rect, bColor);
    }

    QString tmpStr = str.simplified();
    if ( m_isIMEdit && !tmpStr.isEmpty() ) { // imput method edit area background color
      QRect tmpRect = rect;
      if ( str != m_imPreeditText ) {  // ugly hack
        tmpRect.setLeft( tmpRect.left() + font_w );
        tmpRect.setWidth( tmpRect.width() + font_w );
      }

      paint.fillRect( tmpRect, Qt::darkCyan );  // currently use hard code color
    }

    if ( m_isIMSel && !tmpStr.isEmpty() ) { // imput method selection background color
      int x = rect.left() + ( font_w * (m_imSelStart - m_imStart) );
      int y = rect.top();
      int w = font_w * (m_imSelEnd - m_imSelStart);
      int h = font_h;

      QRect tmpRect = QRect( x, y, w, h );
      if ( str != m_imPreeditText ) {  // ugly hack
        tmpRect.setLeft( tmpRect.left() + font_w );
        tmpRect.setWidth( tmpRect.width() + font_w );
      }

      paint.fillRect( tmpRect, Qt::darkGray );   // currently use hard code color
    }
  }

  // Paint cursor
  if ((attr->rendition & RE_CURSOR) && !isPrinting) {
    paint.setBackgroundMode( Qt::TransparentMode );
    int h = font_h - m_lineSpacing;
    QRect r(rect.x(),rect.y()+m_lineSpacing/2,rect.width(),h);
    if (hasFocus())
    {
       if (!cursorBlinking)
       {
          paint.fillRect(r, fColor);
          fColor = bColor;
       }
    }
    else
    {
       paint.setPen(fColor);
       paint.drawRect(rect.x(),rect.y()+m_lineSpacing/2,rect.width()-1,h-1);
    }
  }

  // Paint text
  
  //Check & apply BOLD font
  if (attr->rendition & RE_BOLD)
  {
  		QFont currentFont = paint.font();
		currentFont.setBold(true);
  		paint.setFont( currentFont );
  }

  
  
  if (!(blinking && (attr->rendition & RE_BLINK)))
  {
    // ### Disabled for now, since it causes problems with characters
    // that use the full width and/or height of the character cells.
    //bool shadow = ( !isPrinting && qAlpha(blend_color) < 0xff
    //		    && qGray( fColor.rgb() ) > 64 );
    bool shadow = false;
    paint.setPen(fColor);
    int x = rect.x();
    if (attr->isBold(color_table) && printerBold)
    {
      // When printing we use a bold font for bold
      QFont f = font();
      f.setBold(true);
      paint.setFont(f);
    }
   
    if(!fixed_font)
	{  	
	  int y = rect.y(); // top of rect
		
	  //check whether the string consists of normal text or line drawing
	  //characters.
	  if (isLineCharString( str ))
	  {
	  	drawLineCharString(paint,x,y,str,attr);
	  }
	  else
	  {
      	if ( shadow ) 
		{
        	paint.setPen( Qt::black );
        	drawTextFixed(paint, x+1, y+1, str, attr);
        	paint.setPen(fColor);
      	}
      
      	drawTextFixed(paint, x, y, str, attr);
	  }
    }
    else
    {
      // The meaning of y differs between different versions of QPainter::drawText!!
      int y = rect.y()+a; // baseline

#ifdef __GNUC__
   #warning "BiDi stuff killed, it should force TRL when !bidiEnabled"
#endif

      if ( shadow ) {
        paint.setPen( Qt::black );

        paint.drawText(x+1,y+1, str);
        paint.setPen(fColor);
      }

      paint.drawText(x,y, str);
    }

    if (attr->isBold(color_table) && isPrinting)
    {
      // When printing we use a bold font for bold
      paint.restore();
    }

    if (attr->isBold(color_table) && !printerBold)
    {
      paint.setClipRect(rect);
      // On screen we use overstrike for bold
      paint.setBackgroundMode( Qt::TransparentMode );
      int x = rect.x()+1;
      if(!fixed_font)
      {
        // The meaning of y differs between different versions of QPainter::drawText!!
        int y = rect.y(); // top of rect
        drawTextFixed(paint, x, y, str, attr);
      }
      else
      {
        // The meaning of y differs between different versions of QPainter::drawText!!
        int y = rect.y()+a; // baseline
        //### if (bidiEnabled)
          paint.drawText( QPoint( x, y ), str );
        //else
        //###   paint.drawText(x,y, str, -1, QPainter::LTR);
      }
      paint.setClipping(false);
    }
    if (attr->rendition & RE_UNDERLINE)
      paint.drawLine(rect.left(), rect.y()+a+1,
                     rect.right(),rect.y()+a+1 );
  }

  //restore painter to state prior to drawing text
  paint.restore();
}

/*!
    Set XIM Position
*/
void TerminalDisplay::setCursorPos(const int curx, const int cury)
{
    QPoint tL  = contentsRect().topLeft();
    int    tLx = tL.x();
    int    tLy = tL.y();

    int xpos, ypos;
    ypos = bY + tLy + font_h*(cury-1) + font_a;
    xpos = bX + tLx + font_w*curx;
    //setMicroFocusHint(xpos, ypos, 0, font_h); //### ???
    // fprintf(stderr, "x/y = %d/%d\txpos/ypos = %d/%d\n", curx, cury, xpos, ypos);
    m_cursorLine = cury;
    m_cursorCol = curx;
}

//scrolls the image by 'lines', down if lines > 0 or up otherwise.
//
//the terminal emulation keeps track of the scrolling of the character image as it receives input,
//and when the view is updated, it calls scrollImage() with the final scroll amount.  this improves
//performance because scrolling the display is much cheaper than re-rendering all the text for the part
//of the image which has moved up or down.  instead only new lines have to be drawn
//
//note:  it is important that the area of the display which is scrolled aligns properly with
//the character grid - which has a top left point at (bX,bY) , a cell width of font_w and a cell height
//of font_h).    
void TerminalDisplay::scrollImage(int lines)
{
    if ( lines == 0 || image == 0 || abs(lines) >= this->usedLines ) return;

    QRect scrollRect;

    //scroll internal image
    if ( lines > 0 )
    {
        assert( (lines*this->usedColumns) < image_size ); 

        //scroll internal image down
        memmove( image , &image[lines*this->usedColumns] , ( this->usedLines - lines ) * this->usedColumns * sizeof(Character) );
 
        //set region of display to scroll, making sure that
        //the region aligns correctly to the character grid 
        scrollRect = QRect( bX ,bY, this->usedColumns * font_w , (this->usedLines - lines) * font_h );

        //qDebug() << "scrolled down " << lines << " lines";
    }
    else
    {
        //scroll internal image up
        memmove( &image[ abs(lines)*this->usedColumns] , image , 
                        (this->usedLines - abs(lines) ) * this->usedColumns * sizeof(Character) );

        //set region of the display to scroll, making sure that
        //the region aligns correctly to the character grid
        
        QPoint topPoint( bX , bY + abs(lines)*font_h );

        scrollRect = QRect( topPoint , QSize( this->usedColumns*font_w , (this->usedLines - abs(lines)) * font_h ));
        
        //qDebug() << "scrolled up " << lines << " lines";
    }

    //scroll the display vertically to match internal image
    scroll( 0 , font_h * (-lines) , scrollRect );
}

void TerminalDisplay::processFilters() 
{
    _filterChain->reset();
    _filterChain->addImage(image,lines,columns);
    _filterChain->process();
}

void TerminalDisplay::updateImage() 
{
  // optimization - scroll the existing image where possible and avoid expensive text drawing
  // for parts of the image that can simply be moved up or down
  scrollImage( _screenWindow->scrollCount() );
  _screenWindow->resetScrollCount();

  const Character* const newimg = _screenWindow->getImage();
  int lines = _screenWindow->windowLines();
  int columns = _screenWindow->windowColumns();

  setScroll( _screenWindow->currentLine() , _screenWindow->lineCount() );

  if (!image)
     updateImageSize(); // Create image

  assert( this->usedLines <= this->lines );
  assert( this->usedColumns <= this->columns );

  int y,x,len;

  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();
  hasBlinker = false;

  CharacterColor cf;       // undefined
  CharacterColor cb;       // undefined
  int cr  = -1;   // undefined

  const int linesToUpdate = qMin(this->lines, qMax(0,lines  ));
  const int columnsToUpdate = qMin(this->columns,qMax(0,columns));

  QChar *disstrU = new QChar[columnsToUpdate];
  char *dirtyMask = (char *) malloc(columnsToUpdate+2);
  QRegion dirtyRegion;

  int dirtyLineCount = 0;

  for (y = 0; y < linesToUpdate; y++)
  {
    const Character*       currentLine = &image[y*this->columns];
    const Character* const newLine = &newimg[y*columns];

    bool updateLine = false;
    
    // The dirty mask indicates which characters need repainting. We also
    // mark surrounding neighbours dirty, in case the character exceeds
    // its cell boundaries
    memset(dirtyMask, 0, columnsToUpdate+2);
    // Two extra so that we don't have to have to care about start and end conditions
    for (x = 0; x < columnsToUpdate; x++)
    {
	if ( ( (m_imPreeditLength > 0) && ( ( m_imStartLine == y )
	      && ( ( m_imStart < m_imEnd ) && ( ( x > m_imStart ) ) && ( x < m_imEnd ) )
              || ( ( m_imSelStart < m_imSelEnd ) && ( ( x > m_imSelStart ) ) ) ) )
            || newLine[x] != currentLine[x])
      {
         dirtyMask[x] = dirtyMask[x+1] = dirtyMask[x+2] = 1;
      }
    }
    dirtyMask++; // Position correctly

    if (!resizing) // not while resizing, we're expecting a paintEvent
    for (x = 0; x < columnsToUpdate; x++)
    {
      hasBlinker |= (newLine[x].rendition & RE_BLINK);
    
      // Start drawing if this character or the next one differs.
      // We also take the next one into account to handle the situation
      // where characters exceed their cell width.
      if (dirtyMask[x])
      {
        Q_UINT16 c = newLine[x+0].character;
        if ( !c )
            continue;
        int p = 0;
        disstrU[p++] = c; //fontMap(c);
        bool lineDraw = isLineChar(c);
        bool doubleWidth = (newLine[x+1].character == 0);
        cr = newLine[x].rendition;
        cb = newLine[x].backgroundColor;
        if (newLine[x].foregroundColor != cf) cf = newLine[x].foregroundColor;
        int lln = columnsToUpdate - x;
        for (len = 1; len < lln; len++)
        {
          c = newLine[x+len].character;
          if (!c)
            continue; // Skip trailing part of multi-col chars.

          if (newLine[x+len].foregroundColor != cf || newLine[x+len].backgroundColor != cb || newLine[x+len].rendition != cr ||
              !dirtyMask[x+len] || isLineChar(c) != lineDraw || (newLine[x+len+1].character == 0) != doubleWidth)
            break;

          disstrU[p++] = c; //fontMap(c);
        }

        QString unistr(disstrU, p);

        // for XIM on the spot input style
        m_isIMEdit = m_isIMSel = false;
        if ( m_imStartLine == y ) {
          if ( ( m_imStart < m_imEnd ) && ( x >= m_imStart-1 ) && ( x + int( unistr.length() ) <= m_imEnd ) )
            m_isIMEdit = true;

          if ( ( m_imSelStart < m_imSelEnd ) && ( x >= m_imStart-1 ) && ( x + int( unistr.length() ) <= m_imEnd ) )
            m_isIMSel = true;
	}
        else if ( m_imStartLine < y ) {  // for word worp
          if ( ( m_imStart < m_imEnd ) )
            m_isIMEdit = true;

          if ( ( m_imSelStart < m_imSelEnd ) )
            m_isIMSel = true;
	}

        bool save_fixed_font = fixed_font;
        if (lineDraw)
           fixed_font = false;
        if (doubleWidth)
           fixed_font = false;

		updateLine = true;

		fixed_font = save_fixed_font;
        x += len - 1;
      }
      
    }

	//both the top and bottom halves of double height lines must always be redrawn
	//although both top and bottom halves contain the same characters, only the top one is actually 
	//drawn.
    if (lineProperties.count() > y)
        updateLine |= (lineProperties[y] & LINE_DOUBLEHEIGHT);
	
    if (updateLine)
    {
        dirtyLineCount++;
        QRect dirtyRect = QRect( bX+tLx , bY+tLy+font_h*y , font_w * columnsToUpdate , font_h ); 	
    
        dirtyRegion |= dirtyRect;
    }

    dirtyMask--; // Set back

    // finally, make `image' become `newimg'.
    memcpy((void*)currentLine,(const void*)newLine,columnsToUpdate*sizeof(Character));
  }

  //qDebug() << "dirty line count = " << dirtyLineCount;

  // if the new image is smaller than the previous image, then ensure that the area
  // outside the new image is cleared 
  if ( linesToUpdate < usedLines )
  {
    dirtyRegion |= QRect( bX+tLx , bY+tLy+font_h*linesToUpdate , font_w * this->columns , font_h * (usedLines-linesToUpdate) );
  }
  usedLines = linesToUpdate;
  
  if ( columnsToUpdate < usedColumns )
  {
    dirtyRegion |= QRect( bX+tLx+columnsToUpdate*font_w , bY+tLy , font_w * (usedColumns-columnsToUpdate) , font_h * this->lines );
  }
  usedColumns = columnsToUpdate;

  // redraw the display
  update(dirtyRegion);

  if ( hasBlinker && !blinkT->isActive()) blinkT->start( BLINK_DELAY ); 
  if (!hasBlinker && blinkT->isActive()) { blinkT->stop(); blinking = false; }
  free(dirtyMask);
  delete [] disstrU;

  showResizeNotification();
}

void TerminalDisplay::showResizeNotification()
{
  if (resizing && terminalSizeHint)
  {
     if (terminalSizeStartup) {
       terminalSizeStartup=false;
       return;
     }
     if (!mResizeWidget)
     {
        mResizeWidget = new QFrame(this);

        QFont f = KGlobalSettings::generalFont();
        int fs = f.pointSize();
        if (fs == -1)
           fs = QFontInfo(f).pointSize();
        f.setPointSize((fs*3)/2);
        f.setBold(true);
        mResizeWidget->setFont(f);
        mResizeWidget->setFrameShape((QFrame::Shape) (QFrame::Box|QFrame::Raised));
        mResizeWidget->setMidLineWidth(2);
        QBoxLayout *l = new QVBoxLayout(mResizeWidget);
	l->setMargin(10);
        mResizeLabel = new QLabel(i18n("Size: XXX x XXX"), mResizeWidget);
        l->addWidget(mResizeLabel, 1, Qt::AlignCenter);
        mResizeWidget->setMinimumWidth(mResizeLabel->fontMetrics().width(i18n("Size: XXX x XXX"))+20);
        mResizeWidget->setMinimumHeight(mResizeLabel->sizeHint().height()+20);
        mResizeTimer = new QTimer(this);
	mResizeTimer->setSingleShot(true);
        connect(mResizeTimer, SIGNAL(timeout()), mResizeWidget, SLOT(hide()));
     }
     QString sizeStr = i18n("Size: %1 x %2", columns, lines);
     mResizeLabel->setText(sizeStr);
     mResizeWidget->move((width()-mResizeWidget->width())/2,
                         (height()-mResizeWidget->height())/2+20);
     mResizeWidget->show();
     mResizeTimer->start(3000);
  }

}

void TerminalDisplay::setBlinkingCursor(bool blink)
{
  hasBlinkingCursor=blink;
  if (blink && !blinkCursorT->isActive()) blinkCursorT->start(1000);
  if (!blink && blinkCursorT->isActive()) {
    blinkCursorT->stop();
    if (cursorBlinking)
      blinkCursorEvent();
    else
      cursorBlinking = false;
  }
}

// paint Event ////////////////////////////////////////////////////

/*!
    The difference of this routine vs. the `setImage' is,
    that the drawing does not include a difference analysis
    between the old and the new image. Instead, the internal
    image is used and the painting bound by the PaintEvent box.
*/

void TerminalDisplay::paintEvent( QPaintEvent* pe )
{
  QPainter paint;
  paint.begin( this );
  paint.setBackgroundMode( Qt::TransparentMode );

  foreach (QRect rect, (pe->region() & contentsRect()).rects())
  {
    paintContents(paint, rect);
  }
  paintFilters(paint);

  drawFrame( &paint );

  // We have to make sure every single pixel is painted by the paint event.
  // To do this, we must figure out which pixels are left in the area
  // between the terminal image and the frame border.

  // Calculate the contents rect excluding scroll bar.
  QRect innerRect = contentsRect();
  if( scrollLoc != SCROLLBAR_NONE )
    innerRect.setWidth( innerRect.width() - scrollbar->width() );

  innerRect.setWidth( innerRect.width() + 3 );
  innerRect.setHeight( innerRect.height() );

  // Calculate the emulation rect (area needed for actual terminal contents)
  QRect emurect( contentsRect().topLeft(), QSize( columns * font_w + 2 * rimX, lines * font_h + 2 * rimY ));

  // Now erase the remaining pixels on all sides of the emulation

  // Top
  QRect er( innerRect );
  er.setBottom( emurect.top() );
  paint.eraseRect( er );

  // Bottom
  er.setBottom( innerRect.bottom() );
  er.setTop( emurect.bottom() );
  paint.eraseRect( er );

  // Left
  er.setTop( emurect.top() );
  er.setBottom( emurect.bottom() - 1 );
  er.setRight( emurect.left() );
  paint.eraseRect( er );

  // Right
  er.setRight( innerRect.right() );
  er.setTop( emurect.top() );
  er.setBottom( emurect.bottom() - 1 );
  er.setLeft( emurect.right() );
  paint.eraseRect( er );

  paint.end();
}

void TerminalDisplay::print(QPainter &paint, bool friendly, bool exact)
{
   bool save_fixed_font = fixed_font;
   bool save_blinking = blinking;
   fixed_font = false;
   blinking = false;
   paint.setFont(font());

   isPrinting = true;
   printerFriendly = friendly;
   printerBold = !exact;

   if (exact)
   {
     QPixmap pm(contentsRect().right(), contentsRect().bottom());
     pm.fill();

     QPainter pm_paint;
     pm_paint.begin(&pm);
     paintContents(pm_paint, contentsRect());
     pm_paint.end();
     paint.drawPixmap(0, 0, pm);
   }
   else
   {
     paintContents(paint, contentsRect());
   }

   printerFriendly = false;
   isPrinting = false;
   printerBold = false;

   fixed_font = save_fixed_font;
   blinking = save_blinking;
}

FilterChain* TerminalDisplay::filterChain() const
{
    return _filterChain;
}

void TerminalDisplay::paintFilters(QPainter& painter)
{
    // iterate over hotspots identified by the display's currently active filters 
    // and draw appropriate visuals to indicate the presence of the hotspot

    QList<Filter::HotSpot*> spots = _filterChain->hotSpots();
    QListIterator<Filter::HotSpot*> iter(spots);
    while (iter.hasNext())
    {
        Filter::HotSpot* spot = iter.next();

        for ( int line = spot->startLine() ; line <= spot->endLine() ; line++ )
        {
            int startColumn = 0;
            int endColumn = columns; // TODO use number of columns which are actually occupied
                                     // on this line rather than the width of the display in columns

            if ( line == spot->startLine() )
                startColumn = spot->startColumn();
            if ( line == spot->endLine() )
                endColumn = spot->endColumn();

            QRect r;
            r.setCoords( startColumn*font_w , line*font_h,
                             endColumn*font_w  - 1, (line+1)*font_h - 1 ); // subtract one pixel from
                                                                           // the right and bottom so that
                                                                           // we do not overdraw adjacent
                                                                           // hotspots

            // Links need to be underlined
            if ( spot->type() == Filter::HotSpot::Link )
            {
                QFontMetrics metrics(font());
        
                // find the baseline (which is the invisible line that the characters in the font sit on,
                // with some having tails dangling below)
                int baseline = r.bottom() - metrics.descent();
                // find the position of the underline below that
                int underlinePos = baseline + metrics.underlinePos();

                if ( r.contains( mapFromGlobal(QCursor::pos()) ) )
                    painter.drawLine( r.left() , underlinePos , 
                                      r.right() , underlinePos );
            }
            // Marker hotspots simply have a transparent rectanglular shape
            // drawn on top of them
            else if ( spot->type() == Filter::HotSpot::Marker )
            {
            //TODO - Do not use a hardcoded colour for this
                painter.fillRect(r,QBrush(QColor(255,0,0,120)));
            }
        }
    }
}
void TerminalDisplay::paintContents(QPainter &paint, const QRect &rect)
{
  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();

  int lux = qMin(usedColumns-1, qMax(0,(rect.left()   - tLx - bX ) / font_w));
  int luy = qMin(usedLines-1,  qMax(0,(rect.top()    - tLy - bY  ) / font_h));
  int rlx = qMin(usedColumns-1, qMax(0,(rect.right()  - tLx - bX ) / font_w));
  int rly = qMin(usedLines-1,  qMax(0,(rect.bottom() - tLy - bY  ) / font_h));

  QChar *disstrU = new QChar[usedColumns];
  for (int y = luy; y <= rly; y++)
  {
    Q_UINT16 c = image[loc(lux,y)].character;
    int x = lux;
    if(!c && x)
      x--; // Search for start of multi-column character
    for (; x <= rlx; x++)
    {
      int len = 1;
      int p = 0;
      c = image[loc(x,y)].character;
      if (c)
         disstrU[p++] = c; //fontMap(c);
      bool lineDraw = isLineChar(c);
      bool doubleWidth = (image[ qMin(loc(x,y)+1,image_size) ].character == 0);
      CharacterColor cf = image[loc(x,y)].foregroundColor;
      CharacterColor cb = image[loc(x,y)].backgroundColor;
      UINT8 cr = image[loc(x,y)].rendition;
	  
      while (x+len <= rlx &&
             image[loc(x+len,y)].foregroundColor == cf &&
             image[loc(x+len,y)].backgroundColor == cb &&
             image[loc(x+len,y)].rendition == cr &&
             (image[ qMin(loc(x+len,y)+1,image_size) ].character == 0) == doubleWidth &&
             isLineChar( c = image[loc(x+len,y)].character) == lineDraw) // Assignment!
      {
        if (c)
          disstrU[p++] = c; //fontMap(c);
        if (doubleWidth) // assert((image[loc(x+len,y)+1].character == 0)), see above if condition
          len++; // Skip trailing part of multi-column character
        len++;
      }
      if ((x+len < usedColumns) && (!image[loc(x+len,y)].character))
        len++; // Adjust for trailing part of multi-column character

   	     bool save_fixed_font = fixed_font;
         if (lineDraw)
            fixed_font = false;
         if (doubleWidth)
            fixed_font = false;
         QString unistr(disstrU,p);
		 
		 if (y < lineProperties.size())
		 {
			if (lineProperties[y] & LINE_DOUBLEWIDTH)
				paint.scale(2,1);
			
			if (lineProperties[y] & LINE_DOUBLEHEIGHT)
  		 		paint.scale(1,2);
		 }

		 //calculate the area in which the text will be drawn
		 QRect textArea = QRect( bX+tLx+font_w*x , bY+tLy+font_h*y , font_w*len , font_h);
		
		 //move the calculated area to take account of scaling applied to the painter.
		 //the position of the area from the origin (0,0) is scaled by the opposite of whatever
		 //transformation has been applied to the painter.  this ensures that 
		 //painting does actually start from textArea.topLeft() (instead of textArea.topLeft() * painter-scale)	
		 QMatrix inverted = paint.matrix().inverted();
		 textArea.moveTopLeft( inverted.map(textArea.topLeft()) );
		 
		 //paint text fragment
         drawAttrStr(	paint,
                		textArea,
                		unistr, 
						&image[loc(x,y)], 
						0, 
						!isPrinting );
         
		 fixed_font = save_fixed_font;
     
		 //reset back to single-width, single-height lines 
		 paint.resetMatrix();

		 if (y < lineProperties.size())
		 {
			//double-height lines are represented by two adjacent lines containing the same characters
			//both lines will have the LINE_DOUBLEHEIGHT attribute.  
            //If the current line has the LINE_DOUBLEHEIGHT attribute, we can therefore skip the next line
			if (lineProperties[y] & LINE_DOUBLEHEIGHT)
				y++;
		 }
		 
	    x += len - 1;
    }
  }
  delete [] disstrU;
}

void TerminalDisplay::blinkEvent()
{
  blinking = !blinking;

  //TODO:  Optimise to only repaint the areas of the widget where there is blinking text
  //rather than repainting the whole widget.
  repaint();
}

void TerminalDisplay::blinkCursorEvent()
{
  cursorBlinking = !cursorBlinking;
  repaint(cursorRect);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                  Resizing                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::resizeEvent(QResizeEvent*)
{
  updateImageSize();
}

void TerminalDisplay::propagateSize()
{
  if (isFixedSize)
  {
     setSize(columns, lines);
     QFrame::setFixedSize(sizeHint());
     parentWidget()->adjustSize();
     parentWidget()->setFixedSize(parentWidget()->sizeHint());
     return;
  }
  if (image)
     updateImageSize();
}

void TerminalDisplay::updateImageSize()
{
  Character* oldimg = image;
  int oldlin = lines;
  int oldcol = columns;
  makeImage();
  // we copy the old image to reduce flicker
  int lins = qMin(oldlin,lines);
  int cols = qMin(oldcol,columns);

  if (oldimg)
  {
    for (int lin = 0; lin < lins; lin++)
      memcpy((void*)&image[columns*lin],
             (void*)&oldimg[oldcol*lin],cols*sizeof(Character));
    free(oldimg); //FIXME: try new,delete
  }

  //NOTE: control flows from the back through the chest right into the eye.
  //      `emu' will call back via `setImage'.

  resizing = (oldlin!=lines) || (oldcol!=columns);

  if ( resizing )
  {
    emit changedContentSizeSignal(contentHeight, contentWidth); // expose resizeEvent
  }
  
  resizing = false;
}

//showEvent and hideEvent are reimplemented here so that it appears to other classes that the 
//display has been resized when the display is hidden or shown.
//
//this allows  
//TODO: Perhaps it would be better to have separate signals for show and hide instead of using
//the same signal as the one for a content size change 
void TerminalDisplay::showEvent(QShowEvent*)
{
    emit changedContentSizeSignal(contentHeight,contentWidth);
}
void TerminalDisplay::hideEvent(QHideEvent*)
{
    emit changedContentSizeSignal(contentHeight,contentWidth);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Scrollbar                                  */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::scrollChanged(int)
{
  _screenWindow->scrollTo( scrollbar->value() );

  const bool atEndOfOutput = (scrollbar->value() == scrollbar->maximum());

  _screenWindow->setTrackOutput( atEndOfOutput );

  updateImage();
  //emit changedHistoryCursor(scrollbar->value()); //expose
}

int TerminalDisplay::scrollPosition()
{
    return scrollbar->value();
}
bool TerminalDisplay::scrollAtEnd()
{
    return (scrollbar->value() == scrollbar->maximum());
}

void TerminalDisplay::setScroll(int cursor, int slines)
{
  //kDebug(1211)<<"TerminalDisplay::setScroll() disconnect()"<<endl;
  disconnect(scrollbar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));
  //kDebug(1211)<<"TerminalDisplay::setScroll() setRange()"<<endl;
  scrollbar->setRange(0,slines);
  //kDebug(1211)<<"TerminalDisplay::setScroll() setSteps()"<<endl;
  scrollbar->setSingleStep(1);
  scrollbar->setPageStep(lines);
  scrollbar->setValue(cursor);
  connect(scrollbar, SIGNAL(valueChanged(int)), this, SLOT(scrollChanged(int)));
  //kDebug(1211)<<"TerminalDisplay::setScroll() done"<<endl;
}

void TerminalDisplay::setScrollbarLocation(int loc)
{
  if (scrollLoc == loc) return; // quickly
  bY = bX = 1;
  scrollLoc = loc;
  calcGeometry();
  propagateSize();
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Mouse                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*!
    Three different operations can be performed using the mouse, and the
    routines in this section serve all of them:

    1) The press/release events are exposed to the application
    2) Marking (press and move left button) and Pasting (press middle button)
    3) The right mouse button is used from the configuration menu

    NOTE: During the marking process we attempt to keep the cursor within
    the bounds of the text as being displayed by setting the mouse position
    whenever the mouse has left the text area.

    Two reasons to do so:
    1) QT does not allow the `grabMouse' to confine-to the TerminalDisplay.
       Thus a `XGrapPointer' would have to be used instead.
    2) Even if so, this would not help too much, since the text area
       of the TerminalDisplay is normally not identical with it's bounds.

    The disadvantage of the current handling is, that the mouse can visibly
    leave the bounds of the widget and is then moved back. Because of the
    current construction, and the reasons mentioned above, we cannot do better
    without changing the overall construction.
*/

/*!
*/

void TerminalDisplay::mousePressEvent(QMouseEvent* ev)
{
  if ( possibleTripleClick && (ev->button()==Qt::LeftButton) ) {
    mouseTripleClickEvent(ev);
    return;
  }

  if ( !contentsRect().contains(ev->pos()) ) return;

  int charLine;
  int charColumn;
  characterPosition(ev->pos(),charLine,charColumn);
  QPoint pos = QPoint(charColumn,charLine);

  //kDebug() << " mouse pressed at column = " << pos.x() << " , line = " << pos.y() << endl;

  if ( ev->button() == Qt::LeftButton)
  {
    line_selection_mode = false;
    word_selection_mode = false;

    emit isBusySelecting(true); // Keep it steady...
    // Drag only when the Control key is hold
    bool selected = false;
    
    // The receiver of the testIsSelected() signal will adjust
    // 'selected' accordingly.
    //emit testIsSelected(pos.x(), pos.y(), selected);
    
    selected = _screenWindow->isSelected(pos.x(),pos.y());

    if ((!ctrldrag || ev->modifiers() & Qt::ControlModifier) && selected ) {
      // The user clicked inside selected text
      dragInfo.state = diPending;
      dragInfo.start = ev->pos();
    }
    else {
      // No reason to ever start a drag event
      dragInfo.state = diNone;

      preserve_line_breaks = !( ( ev->modifiers() & Qt::ControlModifier ) && !(ev->modifiers() & Qt::AltModifier) );
      column_selection_mode = (ev->modifiers() & Qt::AltModifier) && (ev->modifiers() & Qt::ControlModifier);

      if (mouse_marks || (ev->modifiers() & Qt::ShiftModifier))
      {
        _screenWindow->clearSelection();

        //emit clearSelectionSignal();
        pos.ry() += scrollbar->value();
        iPntSel = pntSel = pos;
        actSel = 1; // left mouse button pressed but nothing selected yet.
        grabMouse(   /*crossCursor*/  ); // handle with care!
      }
      else
      {
        emit mouseSignal( 0, charColumn + 1, charLine + 1 +scrollbar->value() -scrollbar->maximum() , 0);
      }
    }
  }
  else if ( ev->button() == Qt::MidButton )
  {
    if ( mouse_marks || (!mouse_marks && (ev->modifiers() & Qt::ShiftModifier)) )
      emitSelection(true,ev->modifiers() & Qt::ControlModifier);
    else
      emit mouseSignal( 1, charColumn +1, charLine +1 +scrollbar->value() -scrollbar->maximum() , 0);
  }
  else if ( ev->button() == Qt::RightButton )
  {
    if (mouse_marks || (ev->modifiers() & Qt::ShiftModifier)) {
      configureRequestPoint = QPoint( ev->x(), ev->y() );
      emit configureRequest( this, ev->modifiers()&(Qt::ShiftModifier|Qt::ControlModifier), ev->x(), ev->y() );
    }
    else
      emit mouseSignal( 2, charColumn +1, charLine +1 +scrollbar->value() -scrollbar->maximum() , 0);
  }
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent* ev)
{
  int charLine = 0;
  int charColumn = 0;

  characterPosition(ev->pos(),charLine,charColumn); 

  // handle filters
  // change link hot-spot appearence on mouse-over
  Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine,charColumn);
  if ( spot && spot->type() == Filter::HotSpot::Link)
  {
    _mouseOverHotspotArea.setCoords( qMin(spot->startColumn() , spot->endColumn()) * font_w,
                                     spot->startLine() * font_h,
                                     qMax(spot->startColumn() , spot->endColumn()) * font_h,
                                     (spot->endLine()+1) * font_h );

    setCursor( Qt::PointingHandCursor );

    // display tooltips when mousing over links
    // TODO: Extend this to work with filter types other than links
    const QString& tooltip = spot->tooltip();
    if ( !tooltip.isEmpty() )
    {
        QToolTip::showText( mapToGlobal(ev->pos()) , tooltip , this , _mouseOverHotspotArea );
    }

    update( _mouseOverHotspotArea );
  }
  else if ( _mouseOverHotspotArea.isValid() )
  {
        unsetCursor();

        update( _mouseOverHotspotArea );
        // set hotspot area to an invalid rectangle
        _mouseOverHotspotArea = QRect();
  }
  
  // for auto-hiding the cursor, we need mouseTracking
  if (ev->buttons() == Qt::NoButton ) return;

  // if the terminal is interested in mouse movements 
  // then emit a mouse movement signal, unless the shift
  // key is being held down, which overrides this.
  if (!mouse_marks && !(ev->modifiers() & Qt::ShiftModifier))
  {
	int button = 3;
	if (ev->buttons() & Qt::LeftButton)
		button = 0;
	if (ev->buttons() & Qt::MidButton)
		button = 1;
	if (ev->buttons() & Qt::RightButton)
		button = 2;

        
        emit mouseSignal( button, 
                        charColumn + 1,
                        charLine + 1 +scrollbar->value() -scrollbar->maximum(),
			 1 );
      
	return;
  }

  
      
  if (dragInfo.state == diPending) {
    // we had a mouse down, but haven't confirmed a drag yet
    // if the mouse has moved sufficiently, we will confirm

   int distance = KGlobalSettings::dndEventDelay();
   if ( ev->x() > dragInfo.start.x() + distance || ev->x() < dragInfo.start.x() - distance ||
        ev->y() > dragInfo.start.y() + distance || ev->y() < dragInfo.start.y() - distance) {
      // we've left the drag square, we can start a real drag operation now
      emit isBusySelecting(false); // Ok.. we can breath again.
      _screenWindow->clearSelection();
      //emit clearSelectionSignal();
      doDrag();
    }
    return;
  } else if (dragInfo.state == diDragging) {
    // this isn't technically needed because mouseMoveEvent is suppressed during
    // Qt drag operations, replaced by dragMoveEvent
    return;
  }

  if (actSel == 0) return;

 // don't extend selection while pasting
  if (ev->buttons() & Qt::MidButton) return;

  extendSelection( ev->pos() );
}

void TerminalDisplay::setSelectionEnd()
{
  extendSelection( configureRequestPoint );
}

void TerminalDisplay::extendSelection( QPoint pos )
{
  //if ( !contentsRect().contains(ev->pos()) ) return;
  QPoint tL  = contentsRect().topLeft();
  int    tLx = tL.x();
  int    tLy = tL.y();
  int    scroll = scrollbar->value();

  // we're in the process of moving the mouse with the left button pressed
  // the mouse cursor will kept caught within the bounds of the text in
  // this widget.

  // Adjust position within text area bounds. See FIXME above.
  QPoint oldpos = pos;
  if ( pos.x() < tLx+bX )                  pos.setX( tLx+bX );
  if ( pos.x() > tLx+bX+usedColumns*font_w-1 ) pos.setX( tLx+bX+usedColumns*font_w );
  if ( pos.y() < tLy+bY )                   pos.setY( tLy+bY );
  if ( pos.y() > tLy+bY+usedLines*font_h-1 )    pos.setY( tLy+bY+usedLines*font_h-1 );

  // check if we produce a mouse move event by this
  if ( pos != oldpos ) cursor().setPos(mapToGlobal(pos));

  if ( pos.y() == tLy+bY+usedLines*font_h-1 )
  {
    scrollbar->setValue(scrollbar->value()+yMouseScroll); // scrollforward
  }
  if ( pos.y() == tLy+bY )
  {
    scrollbar->setValue(scrollbar->value()-yMouseScroll); // scrollback
  }

  int charColumn = 0;
  int charLine = 0;
  characterPosition(pos,charLine,charColumn);

  QPoint here = QPoint(charColumn,charLine); //QPoint((pos.x()-tLx-bX+(font_w/2))/font_w,(pos.y()-tLy-bY)/font_h);
  QPoint ohere;
  QPoint iPntSelCorr = iPntSel;
  iPntSelCorr.ry() -= scrollbar->value();
  QPoint pntSelCorr = pntSel;
  pntSelCorr.ry() -= scrollbar->value();
  bool swapping = false;

  if ( word_selection_mode )
  {
    // Extend to word boundaries
    int i;
    int selClass;

    bool left_not_right = ( here.y() < iPntSelCorr.y() ||
	   here.y() == iPntSelCorr.y() && here.x() < iPntSelCorr.x() );
    bool old_left_not_right = ( pntSelCorr.y() < iPntSelCorr.y() ||
	   pntSelCorr.y() == iPntSelCorr.y() && pntSelCorr.x() < iPntSelCorr.x() );
    swapping = left_not_right != old_left_not_right;

    // Find left (left_not_right ? from here : from start)
    QPoint left = left_not_right ? here : iPntSelCorr;
    i = loc(left.x(),left.y());
    if (i>=0 && i<=image_size) {
      selClass = charClass(image[i].character);
      while ( ((left.x()>0) || (left.y()>0 && (lineProperties[left.y()-1] & LINE_WRAPPED) )) 
					  && charClass(image[i-1].character) == selClass )
      { i--; if (left.x()>0) left.rx()--; else {left.rx()=usedColumns-1; left.ry()--;} }
    }

    // Find left (left_not_right ? from start : from here)
    QPoint right = left_not_right ? iPntSelCorr : here;
    i = loc(right.x(),right.y());
    if (i>=0 && i<=image_size) {
      selClass = charClass(image[i].character);
      while( ((right.x()<usedColumns-1) || (right.y()<usedLines-1 && (lineProperties[right.y()] & LINE_WRAPPED) )) 
					  && charClass(image[i+1].character) == selClass )
      { i++; if (right.x()<usedColumns-1) right.rx()++; else {right.rx()=0; right.ry()++; } }
    }

    // Pick which is start (ohere) and which is extension (here)
    if ( left_not_right )
    {
      here = left; ohere = right;
    }
    else
    {
      here = right; ohere = left;
    }
    ohere.rx()++;
  }

  if ( line_selection_mode )
  {
    // Extend to complete line
    bool above_not_below = ( here.y() < iPntSelCorr.y() );

    QPoint above = above_not_below ? here : iPntSelCorr;
    QPoint below = above_not_below ? iPntSelCorr : here;

    while (above.y()>0 && (lineProperties[above.y()-1] & LINE_WRAPPED) )
      above.ry()--;
    while (below.y()<usedLines-1 && (lineProperties[below.y()] & LINE_WRAPPED) )
      below.ry()++;

    above.setX(0);
    below.setX(usedColumns-1);

    // Pick which is start (ohere) and which is extension (here)
    if ( above_not_below )
    {
      here = above; ohere = below;
    }
    else
    {
      here = below; ohere = above;
    }

    QPoint newSelBegin = QPoint( ohere.x(), ohere.y() );
    swapping = !(tripleSelBegin==newSelBegin);
    tripleSelBegin = newSelBegin;

    ohere.rx()++;
  }

  int offset = 0;
  if ( !word_selection_mode && !line_selection_mode )
  {
    int i;
    int selClass;

    bool left_not_right = ( here.y() < iPntSelCorr.y() ||
	   here.y() == iPntSelCorr.y() && here.x() < iPntSelCorr.x() );
    bool old_left_not_right = ( pntSelCorr.y() < iPntSelCorr.y() ||
	   pntSelCorr.y() == iPntSelCorr.y() && pntSelCorr.x() < iPntSelCorr.x() );
    swapping = left_not_right != old_left_not_right;

    // Find left (left_not_right ? from here : from start)
    QPoint left = left_not_right ? here : iPntSelCorr;

    // Find left (left_not_right ? from start : from here)
    QPoint right = left_not_right ? iPntSelCorr : here;
    if ( right.x() > 0 && !column_selection_mode )
    {
      i = loc(right.x(),right.y());
      if (i>=0 && i<=image_size) {
        selClass = charClass(image[i-1].character);
        if (selClass == ' ')
        {
          while ( right.x() < usedColumns-1 && charClass(image[i+1].character) == selClass && (right.y()<usedLines-1) && 
						  !(lineProperties[right.y()] & LINE_WRAPPED))
          { i++; right.rx()++; }
          if (right.x() < usedColumns-1)
            right = left_not_right ? iPntSelCorr : here;
          else
            right.rx()++;  // will be balanced later because of offset=-1;
        }
      }
    }

    // Pick which is start (ohere) and which is extension (here)
    if ( left_not_right )
    {
      here = left; ohere = right; offset = 0;
    }
    else
    {
      here = right; ohere = left; offset = -1;
    }
  }

  if ((here == pntSelCorr) && (scroll == scrollbar->value())) return; // not moved

  if (here == ohere) return; // It's not left, it's not right.

  if ( actSel < 2 || swapping )
  {
    if ( column_selection_mode && !line_selection_mode && !word_selection_mode )
    {
       _screenWindow->setSelectionStart( ohere.x() , ohere.y() , true );
      //emit beginSelectionSignal( ohere.x(), ohere.y(), true );
    }
    else
    {
       _screenWindow->setSelectionStart( ohere.x()-1-offset , ohere.y() , false );
       //emit beginSelectionSignal( ohere.x()-1-offset, ohere.y(), false );
    }

  }

  actSel = 2; // within selection
  pntSel = here;
  pntSel.ry() += scrollbar->value();

  if ( column_selection_mode && !line_selection_mode && !word_selection_mode )
  {
    _screenWindow->setSelectionEnd( here.x() , here.y() );
    //emit extendSelectionSignal( here.x(), here.y() );
  }
  else
  {
    _screenWindow->setSelectionEnd( here.x()+offset , here.y() );
    //emit extendSelectionSignal( here.x()+offset, here.y() );
  }

}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* ev)
{
    int charLine;
    int charColumn;
    characterPosition(ev->pos(),charLine,charColumn);

    // handle filters
    Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine,charColumn);
    if ( spot )
    {
        if ( ev->button() == Qt::LeftButton )
        {
            spot->activate();
        }
        else if ( ev->button() == Qt::RightButton )
        {
            //TODO - Show context menu with appropriate actions for hotspot.
        }
    }

  if ( ev->button() == Qt::LeftButton)
  {
    emit isBusySelecting(false); 
    if(dragInfo.state == diPending)
    {
      // We had a drag event pending but never confirmed.  Kill selection
      _screenWindow->clearSelection();
      //emit clearSelectionSignal();
    }
    else
    {
      if ( actSel > 1 )
      {
          setSelection( _screenWindow->selectedText(preserve_line_breaks) );
          //emit endSelectionSignal(preserve_line_breaks);
      }

      actSel = 0;

      //FIXME: emits a release event even if the mouse is
      //       outside the range. The procedure used in `mouseMoveEvent'
      //       applies here, too.

      if (!mouse_marks && !(ev->modifiers() & Qt::ShiftModifier))
        emit mouseSignal( 3, // release
                        charColumn + 1,
                        charLine + 1 +scrollbar->value() -scrollbar->maximum() , 0);

      releaseMouse();
    }
    dragInfo.state = diNone;
  }
  
  
  if ( !mouse_marks && ((ev->button() == Qt::RightButton && !(ev->modifiers() & Qt::ShiftModifier))
                        || ev->button() == Qt::MidButton) ) 
  {
    emit mouseSignal( 3, charColumn + 1, charLine + 1 +scrollbar->value() -scrollbar->maximum() , 0);
    releaseMouse();
  }
}

void TerminalDisplay::characterPosition(QPoint widgetPoint,int& line,int& column)
{
    column = (widgetPoint.x()-contentsRect().left()-bX) / font_w;
    line = (widgetPoint.y()-contentsRect().top()-bY) / font_h;

    if ( line < 0 )
        line = 0;
    if ( column < 0 )
        column = 0;

    if ( line >= usedLines )
        line = usedLines-1;

    if ( column >= usedColumns )
        column = usedColumns-1;
}

void TerminalDisplay::updateLineProperties()
{
    lineProperties = _screenWindow->getLineProperties();    
}

void TerminalDisplay::mouseDoubleClickEvent(QMouseEvent* ev)
{
  if ( ev->button() != Qt::LeftButton) return;
  
  int charLine = 0;
  int charColumn = 0;

  characterPosition(ev->pos(),charLine,charColumn);

  QPoint pos(charColumn,charLine);

  // pass on double click as two clicks.
  if (!mouse_marks && !(ev->modifiers() & Qt::ShiftModifier))
  {
    // Send just _ONE_ click event, since the first click of the double click
    // was already sent by the click handler!
    emit mouseSignal( 0, pos.x()+1, pos.y()+1 +scrollbar->value() -scrollbar->maximum(),0 ); // left button
    return;
  }

  _screenWindow->clearSelection();
  //emit clearSelectionSignal();
  QPoint bgnSel = pos;
  QPoint endSel = pos;
  int i = loc(bgnSel.x(),bgnSel.y());
  iPntSel = bgnSel;
  iPntSel.ry() += scrollbar->value();

  word_selection_mode = true;

  // find word boundaries...
  int selClass = charClass(image[i].character);
  {
     // find the start of the word
     int x = bgnSel.x();
     while ( ((x>0) || (bgnSel.y()>0 && (lineProperties[bgnSel.y()-1] & LINE_WRAPPED) )) 
					 && charClass(image[i-1].character) == selClass )
     {  
       i--; 
       if (x>0) 
           x--; 
       else 
       {
           x=usedColumns-1; 
           bgnSel.ry()--;
       } 
     }

     bgnSel.setX(x);
     _screenWindow->setSelectionStart( bgnSel.x() , bgnSel.y() , false );
     //emit beginSelectionSignal( bgnSel.x(), bgnSel.y(), false );

     // find the end of the word
     i = loc( endSel.x(), endSel.y() );
     x = endSel.x();
     while( ((x<usedColumns-1) || (endSel.y()<usedLines-1 && (lineProperties[endSel.y()] & LINE_WRAPPED) )) 
					 && charClass(image[i+1].character) == selClass )
     { 
         i++; 
         if (x<usedColumns-1) 
             x++; 
         else 
         {  
             x=0; 
             endSel.ry()++; 
         } 
     }

     endSel.setX(x);

     // In word selection mode don't select @ (64) if at end of word.
     if ( ( QChar( image[i].character ) == '@' ) && ( ( endSel.x() - bgnSel.x() ) > 0 ) )
       endSel.setX( x - 1 );


     actSel = 2; // within selection
     
     _screenWindow->setSelectionEnd( endSel.x() , endSel.y() );
     //emit extendSelectionSignal( endSel.x(), endSel.y() );
    
     setSelection( _screenWindow->selectedText(preserve_line_breaks) ); 
     //emit endSelectionSignal(preserve_line_breaks);
   }

  possibleTripleClick=true;
  QTimer::singleShot(QApplication::doubleClickInterval(),this,SLOT(tripleClickTimeout()));
}

void TerminalDisplay::wheelEvent( QWheelEvent* ev )
{
  if (ev->orientation() != Qt::Vertical)
    return;

  if ( mouse_marks )
    scrollbar->event(ev);
  else
  {
    int charLine;
    int charColumn;
    characterPosition( ev->pos() , charLine , charColumn );
    
    emit mouseSignal( ev->delta() > 0 ? 4 : 5, charColumn + 1, charLine + 1 +scrollbar->value() -scrollbar->maximum() , 0);
  }
}

void TerminalDisplay::tripleClickTimeout()
{
  possibleTripleClick=false;
}

void TerminalDisplay::mouseTripleClickEvent(QMouseEvent* ev)
{
  int charLine;
  int charColumn;
  characterPosition(ev->pos(),charLine,charColumn);
  iPntSel = QPoint(charColumn,charLine);

  _screenWindow->clearSelection();
  //emit clearSelectionSignal();

  line_selection_mode = true;
  word_selection_mode = false;

  actSel = 2; // within selection
  emit isBusySelecting(true); // Keep it steady...

  while (iPntSel.y()>0 && (lineProperties[iPntSel.y()-1] & LINE_WRAPPED) )
    iPntSel.ry()--;
  if (cuttobeginningofline) {
    // find word boundary start
    int i = loc(iPntSel.x(),iPntSel.y());
    int selClass = charClass(image[i].character);
    int x = iPntSel.x();
    while ( ((x>0) || (iPntSel.y()>0 && (lineProperties[iPntSel.y()-1] & LINE_WRAPPED) )) 
					&& charClass(image[i-1].character) == selClass )
    { i--; if (x>0) x--; else {x=columns-1; iPntSel.ry()--;} }

    _screenWindow->setSelectionStart( x , iPntSel.y() , false );
    //emit beginSelectionSignal( x, iPntSel.y(), false );
    tripleSelBegin = QPoint( x, iPntSel.y() );
  }
  else {
    _screenWindow->setSelectionStart( 0 , iPntSel.y() , false );
    //emit beginSelectionSignal( 0, iPntSel.y(), false );
    tripleSelBegin = QPoint( 0, iPntSel.y() );
  }

  while (iPntSel.y()<lines-1 && (lineProperties[iPntSel.y()] & LINE_WRAPPED) )
    iPntSel.ry()++;
  
  _screenWindow->setSelectionEnd( columns - 1 , iPntSel.y() );
  //emit extendSelectionSignal( columns-1, iPntSel.y() );

  setSelection(_screenWindow->selectedText(preserve_line_breaks));
  //emit endSelectionSignal(preserve_line_breaks);

  iPntSel.ry() += scrollbar->value();
}


bool TerminalDisplay::focusNextPrevChild( bool next )
{
  if (next)
    return false; // This disables changing the active part in konqueror
                  // when pressing Tab
  return QFrame::focusNextPrevChild( next );
}


int TerminalDisplay::charClass(UINT16 ch) const
{
    QChar qch=QChar(ch);
    if ( qch.isSpace() ) return ' ';

    if ( qch.isLetterOrNumber() || word_characters.contains(qch, Qt::CaseInsensitive ) )
    return 'a';

    // Everything else is weird
    return 1;
}

void TerminalDisplay::setWordCharacters(QString wc)
{
	word_characters = wc;
}

void TerminalDisplay::setUsesMouse(bool on)
{
  mouse_marks = on;
  setCursor( mouse_marks ? Qt::IBeamCursor : Qt::ArrowCursor );
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                               Clipboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

#undef KeyPress

#if 0
void TerminalDisplay::emitText(QString text)
{
  if (!text.isEmpty()) {
    QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, text);
    emit keyPressedSignal(&e); // expose as a big fat keypress event
  }
}
#endif


void TerminalDisplay::emitSelection(bool useXselection,bool appendReturn)
// Paste Clipboard by simulating keypress events
{
  QString text = QApplication::clipboard()->text(useXselection ? QClipboard::Selection :
                                                                 QClipboard::Clipboard);
  if(appendReturn)
    text.append("\r");
  if ( ! text.isEmpty() )
  {
    text.replace("\n", "\r");
    QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, text);
    emit keyPressedSignal(&e); // expose as a big fat keypress event
    
    _screenWindow->clearSelection();
    //emit clearSelectionSignal();
  }
}

void TerminalDisplay::setSelection(const QString& t)
{
  // Disconnect signal while WE set the clipboard
  QClipboard *cb = QApplication::clipboard();
  
  // We no longer clear the selection when it is changed by another application
  //
  //QObject::disconnect( cb, SIGNAL(selectionChanged()),
  //                   this, SLOT(onClearSelection()) );

  cb->setText(t, QClipboard::Selection);

  //QObject::connect( cb, SIGNAL(selectionChanged()),
  //                   this, SLOT(onClearSelection()) );
}

void TerminalDisplay::copyClipboard()
{
  Q_ASSERT( _screenWindow );

  QString text = _screenWindow->selectedText(true);
  QApplication::clipboard()->setText(text);
}

void TerminalDisplay::pasteClipboard()
{
  emitSelection(false,false);
}

void TerminalDisplay::pasteSelection()
{
  emitSelection(true,false);
}

void TerminalDisplay::onClearSelection()
{
  _screenWindow->clearSelection();
  //emit clearSelectionSignal();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Keyboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

//FIXME: an `eventFilter' has been installed instead of a `keyPressEvent'
//       due to a bug in `QT' or the ignorance of the author to prevent
//       repaint events being emitted to the screen whenever one leaves
//       or reenters the screen to/from another application.
//
//   Troll says one needs to change focusInEvent() and focusOutEvent(),
//   which would also let you have an in-focus cursor and an out-focus
//   cursor like xterm does.

// for the auto-hide cursor feature, I added empty focusInEvent() and
// focusOutEvent() so that update() isn't called.
// For auto-hide, we need to get keypress-events, but we only get them when
// we have focus.

void TerminalDisplay::doScroll(int lines)
{
  scrollbar->setValue(scrollbar->value()+lines);
}

bool TerminalDisplay::eventFilter( QObject *obj, QEvent *e )
{
  if ( (e->type() == QEvent::Accel ||
       e->type() == QEvent::AccelAvailable ) && qApp->focusWidget() == this )
  {

      static_cast<QKeyEvent *>( e )->ignore();
      return false;
  }
  if ( obj != this /* when embedded */ && obj != parent() /* when standalone */ )
      return false; // not us
  if ( e->type() == QEvent::KeyPress )
  {
    QKeyEvent* ke = (QKeyEvent*)e;

	if (ke->modifiers() & Qt::ControlModifier)
	{
		if ( ke->key() == Qt::Key_S )
				emit flowControlKeyPressed(true /*output suspended*/);
		if ( ke->key() == Qt::Key_Q )
				emit flowControlKeyPressed(false /*output enabled*/);
	}
	
    actSel=0; // Key stroke implies a screen update, so TerminalDisplay won't
              // know where the current selection is.

    if (hasBlinkingCursor) {
      blinkCursorT->start(1000);
      if (cursorBlinking)
        blinkCursorEvent();
      else
        cursorBlinking = false;
    }

    emit keyPressedSignal(ke); // expose

    // in Qt2 when key events were propagated up the tree
    // (unhandled? -> parent widget) they passed the event filter only once at
    // the beginning. in qt3 this has changed, that is, the event filter is
    // called each time the event is sent (see loop in QApplication::notify,
    // when internalNotify() is called for KeyPress, whereas internalNotify
    // activates also the global event filter) . That's why we stop propagation
    // here.
    return true;
  }
  
  // We no longer clear the selection when the clipboard changes
  //
  //if ( e->type() == QEvent::Enter )
  //{
  //  QObject::disconnect( (QObject*)cb, SIGNAL(dataChanged()),
  //    this, SLOT(onClearSelection()) );
  //}
  //if ( e->type() == QEvent::Leave )
  //{
  //  QObject::connect( (QObject*)cb, SIGNAL(dataChanged()),
  //    this, SLOT(onClearSelection()) );
  //}*/
  
  return QFrame::eventFilter( obj, e );
}

void TerminalDisplay::inputMethodEvent ( QInputMethodEvent *  )
{
#ifdef __GNUC__
   #warning "FIXME: Port the IM stuff!"
#endif
}

#if 0
void TerminalDisplay::imStartEvent( QIMEvent */*e*/ )
{
  m_imStart = m_cursorCol;
  m_imStartLine = m_cursorLine;
  m_imPreeditLength = 0;

  m_imEnd = m_imSelStart = m_imSelEnd = 0;
  m_isIMEdit = m_isIMSel = false;
}

void TerminalDisplay::imComposeEvent( QIMEvent *e )
{
  QString text.characterlear();
  if ( m_imPreeditLength > 0 ) {
    text.fill( '\010', m_imPreeditLength );
  }

  m_imEnd = m_imStart + string_width( e->text() );

  QString tmpStr = e->text().left( e->cursorPos() );
  m_imSelStart = m_imStart + string_width( tmpStr );

  tmpStr = e->text().mid( e->cursorPos(), e->selectionLength() );
  m_imSelEnd = m_imSelStart + string_width( tmpStr );
  m_imPreeditLength = e->text().length();
  m_imPreeditText = e->text();
  text += e->text();

  if ( text.length() > 0 ) {
    QKeyEvent ke( QEvent::KeyPress, 0, -1, 0, text );
    emit keyPressedSignal( &ke );
  }
}

void TerminalDisplay::imEndEvent( QIMEvent *e )
{
  QString text.characterlear();
  if ( m_imPreeditLength > 0 ) {
      text.fill( '\010', m_imPreeditLength );
  }

  m_imEnd = m_imSelStart = m_imSelEnd = 0;
  text += e->text();
  if ( text.length() > 0 ) {
    QKeyEvent ke( QEvent::KeyPress, 0, -1, 0, text );
    emit keyPressedSignal( &ke );
  }

  QPoint tL  = contentsRect().topLeft();
  int tLx = tL.x();
  int tLy = tL.y();

  QRect repaintRect = QRect( bX+tLx, bY+tLy+font_h*m_imStartLine,
                             contentsRect().width(), contentsRect().height() );
  m_imStart = 0;
  m_imPreeditLength = 0;

  m_isIMEdit = m_isIMSel = false;
  repaint( repaintRect, true );
}
#endif

// Override any Ctrl+<key> accelerator when pressed with the keyboard
// focus in TerminalDisplay, so that the key will be passed to the terminal instead.
bool TerminalDisplay::event( QEvent *e )
{
  if ( e->type() == QEvent::AccelOverride )
  {
    QKeyEvent *ke = static_cast<QKeyEvent *>( e );
    int keyCodeQt = ke->key() | ke->modifiers();

    if ( !standalone() && (ke->modifiers() == Qt::ControlModifier) )
    {
      ke->accept();
      return true;
    }

    // Override any of the following accelerators:
    switch ( keyCodeQt )
    {
      case Qt::Key_Tab:
      case Qt::Key_Delete:
        ke->accept();
        return true;
    }
  }
  return QFrame::event( e );
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                  Frame                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::frameChanged()
{
  propagateSize();
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Sound                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::setBellMode(int mode)
{
  m_bellMode=mode;
}

void TerminalDisplay::enableBell()
{
    allowBell = true;
}



void TerminalDisplay::bell(const QString& message)
{
  if (m_bellMode==BELLNONE) return;

  //limit Bell sounds / visuals etc. to max 1 per second.
  //...mainly for sound effects where rapid bells in sequence produce a horrible noise
  if ( allowBell )
  {
    allowBell = false;
    QTimer::singleShot(500,this,SLOT(enableBell()));
 
    kDebug(1211) << __FUNCTION__ << endl;

    if (m_bellMode==BELLSYSTEM) {
                KNotification::beep();
    } else if (m_bellMode==BELLNOTIFY) {
            KNotification::event("BellVisible", message,QPixmap(),this);
      } else if (m_bellMode==BELLVISUAL) {
        swapColorTable();
        QTimer::singleShot(200,this,SLOT(swapColorTable()));
    }
  }
}

void TerminalDisplay::swapColorTable()
{
  ColorEntry color = color_table[1];
  color_table[1]=color_table[0];
  color_table[0]= color;
  colorsSwapped = !colorsSwapped;
  update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                 Auxiluary                                 */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::clearImage()
// initialize the image
// for internal use only
{
  // We initialize image[image_size] too. See makeImage()
  for (int i = 0; i <= image_size; i++)
  {
    image[i].character = ' ';
    image[i].foregroundColor = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_FORE_COLOR);
    image[i].backgroundColor = CharacterColor(COLOR_SPACE_DEFAULT,DEFAULT_BACK_COLOR);
    image[i].rendition = DEFAULT_RENDITION;
  }
}

// Create Image ///////////////////////////////////////////////////////

void TerminalDisplay::calcGeometry()
{
  scrollbar->resize(QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent),
                    contentsRect().height());
  switch(scrollLoc)
  {
    case SCROLLBAR_NONE :
     bX = rimX;
     contentWidth = contentsRect().width() - 2 * rimX;
     scrollbar->hide();
     break;
    case SCROLLBAR_LEFT :
     bX = rimX+scrollbar->width();
     contentWidth = contentsRect().width() - 2 * rimX - scrollbar->width();
     scrollbar->move(contentsRect().topLeft());
     scrollbar->show();
     break;
    case SCROLLBAR_RIGHT:
     bX = rimX;
     contentWidth = contentsRect().width()  - 2 * rimX - scrollbar->width();
     scrollbar->move(contentsRect().topRight() - QPoint(scrollbar->width()-1,0));
     scrollbar->show();
     break;
  }

  //FIXME: support 'rounding' styles
  bY = rimY;
  contentHeight = contentsRect().height() - 2 * rimY + /* mysterious */ 1;

  if (!isFixedSize)
  {
     // ensure that display is always at least one column wide
     columns = qMax(1,contentWidth / font_w);
     usedColumns = qMin(usedColumns,columns);
     
     // ensure that display is always at least one line high
     lines = qMax(1,contentHeight / font_h);
     usedLines = qMin(usedLines,lines);
  }
}

void TerminalDisplay::makeImage()
{
  calcGeometry();

  // confirm that array will be of non-zero size, since the painting code 
  // assumes a non-zero array length
  assert( lines > 0 && columns > 0 );
  assert( usedLines <= lines && usedColumns <= columns );

  image_size=lines*columns;
  
  // We over-commit 1 character so that we can be more relaxed in dealing with
  // certain boundary conditions: image[image_size] is a valid but unused position
  image = (Character*) malloc((image_size+1)*sizeof(Character));
  clearImage();
}

// calculate the needed size
void TerminalDisplay::setSize(int cols, int lins)
{
  int deltaColumns = cols - columns;
  int deltaLines = lins - lines;

  m_size = QSize( (deltaColumns * font_w) + width() ,
				  (deltaLines * font_h) + height() );

  updateGeometry();
}

void TerminalDisplay::setFixedSize(int cols, int lins)
{
  isFixedSize = true;
  
  //ensure that display is at least 1 line by 1 column in size
  columns = qMax(1,cols);
  lines = qMax(1,lins);
  usedColumns = qMin(usedColumns,columns);
  usedLines = qMin(usedLines,lines);

  if (image)
  {
     free(image);
     makeImage();
  }
  setSize(cols, lins);
  QFrame::setFixedSize(m_size);
}

QSize TerminalDisplay::sizeHint() const
{
  return m_size;
}

void TerminalDisplay::styleChange(QStyle &)
{
    propagateSize();
}


/* --------------------------------------------------------------------- */
/*                                                                       */
/* Drag & Drop                                                           */
/*                                                                       */
/* --------------------------------------------------------------------- */

void TerminalDisplay::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasFormat("text/plain"))
      event->acceptProposedAction();
}

enum dropPopupOptions { paste, cd, cp, ln, mv };

void TerminalDisplay::dropEvent(QDropEvent* event)
{
   if (m_drop==0)
   {
      m_drop = new KMenu( this );
      m_pasteAction = m_drop->addAction( i18n( "Paste" ) );
      m_drop->addSeparator();
      m_cdAction = m_drop->addAction( i18n( "Change Directory" ) );
      m_mvAction = m_drop->addAction( i18n( "Move Here" ) );
      m_cpAction = m_drop->addAction( i18n( "Copy Here" ) );
      m_lnAction = m_drop->addAction( i18n( "Link Here" ) );
      m_pasteAction->setData( QVariant( paste ) );
      m_cdAction->setData( QVariant( cd ) );
      m_mvAction->setData( QVariant( mv ) );
      m_cpAction->setData( QVariant( cp ) );
      m_lnAction->setData( QVariant( ln ) );
      connect(m_drop, SIGNAL(triggered(QAction*)), SLOT(drop_menu_activated(QAction*)));
   };
    // The current behaviour when url(s) are dropped is
    // * if there is only ONE url and if it's a LOCAL one, ask for paste or cd/cp/ln/mv
    // * if there are only LOCAL urls, ask for paste or cp/ln/mv
    // * in all other cases, just paste
    //   (for non-local ones, or for a list of URLs, 'cd' is nonsense)
  m_dnd_file_count = 0;
  dropText = "";
  bool justPaste = true;

  KUrl::List urllist = KUrl::List::fromMimeData(event->mimeData());
  if (urllist.count()) {
    justPaste =false;
    KUrl::List::Iterator it;

    m_cdAction->setEnabled( true );
    m_lnAction->setEnabled( true );

    for ( it = urllist.begin(); it != urllist.end(); ++it ) {
      if(m_dnd_file_count++ > 0) {
        dropText += ' ';
        m_cdAction->setEnabled( false );
      }
      KUrl url = KIO::NetAccess::mostLocalUrl( *it, 0 );
      QString tmp;
      if (url.isLocalFile()) {
        tmp = url.path(); // local URL : remove protocol. This helps "ln" & "cd" and doesn't harm the others
      } else if ( url.protocol() == QLatin1String( "mailto" ) ) {
        justPaste = true;
        break;
      } else {
        tmp = url.url();
        m_cdAction->setEnabled( false );
        m_lnAction->setEnabled( false );
      }
      if (urllist.count()>1)
        KRun::shellQuote(tmp);
      dropText += tmp;
    }

    if (!justPaste) m_drop->popup(mapToGlobal(event->pos()));
  }
  if(justPaste && event->mimeData()->hasFormat("text/plain")) {
    kDebug(1211) << "Drop:" << dropText.toLocal8Bit() << "\n";
    emit sendStringToEmu(dropText.toLocal8Bit());
    // Paste it
  }
}

void TerminalDisplay::doDrag()
{
  dragInfo.state = diDragging;
  dragInfo.dragObject = new QDrag(this);
  QMimeData *mimeData = new QMimeData;
  mimeData->setText(QApplication::clipboard()->text(QClipboard::Selection));
  dragInfo.dragObject->setMimeData(mimeData);
  dragInfo.dragObject->start(Qt::CopyAction);
  // Don't delete the QTextDrag object.  Qt will delete it when it's done with it.
}

void TerminalDisplay::drop_menu_activated(QAction* action)
{
  int item = action->data().toInt();
  switch (item)
  {
   case paste:
      if (m_dnd_file_count==1)
        KRun::shellQuote(dropText);
      emit sendStringToEmu(dropText.toLocal8Bit());
      activateWindow();
      break;
   case cd:
     emit sendStringToEmu("cd ");
      struct stat statbuf;
      if ( ::stat( QFile::encodeName( dropText ), &statbuf ) == 0 )
      {
         if ( !S_ISDIR(statbuf.st_mode) )
         {
            KUrl url;
            url.setPath( dropText );
            dropText = url.directory( KUrl::ObeyTrailingSlash ); // remove filename
         }
      }
      KRun::shellQuote(dropText);
      emit sendStringToEmu(dropText.toLocal8Bit());
      emit sendStringToEmu("\n");
      activateWindow();
      break;
   case cp:
     emit sendStringToEmu("kfmclient copy " );
     break;
   case ln:
     emit sendStringToEmu("ln -s ");
     break;
   case mv:
     emit sendStringToEmu("kfmclient move " );
     break;
   }
   if (item>cd && item<=mv) {
      if (m_dnd_file_count==1)
        KRun::shellQuote(dropText);
      emit sendStringToEmu(dropText.toLocal8Bit());
      emit sendStringToEmu(" .\n");
      activateWindow();
   }
}

void TerminalDisplay::outputSuspended(bool suspended)
{
	//create the label when this function is first called
	if (!outputSuspendedLabel)
	{
            //This label includes a link to an English language website
            //describing the 'flow control' (Xon/Xoff) feature found in almost all terminal emulators.
            //If there isn't a suitable article available in the target language the link
            //can simply be removed.
			outputSuspendedLabel = new QLabel( i18n("<qt>Output has been "
                                                "<a href=\"http://en.wikipedia.org/wiki/XON\">suspended</a>"
                                                " by pressing Ctrl+S."
											   "  Press <b>Ctrl+Q</b> to resume.</qt>"),
											   this );

             //fill label with a light yellow 'warning' colour
            //FIXME - It would be better if there was a way of getting a suitable colour based
            //on the current theme.  Last I looked however, the set of colours provided by the theme
            //did not include anything suitable (most being varying shades of grey)

            QPalette palette(outputSuspendedLabel->palette());
            palette.setColor(QPalette::Base, QColor(255,250,150) );
            outputSuspendedLabel->setPalette(palette);
			outputSuspendedLabel->setAutoFillBackground(true);
			outputSuspendedLabel->setBackgroundRole(QPalette::Base);

            outputSuspendedLabel->setMargin(5);

            //enable activation of "Xon/Xoff" link in label
            outputSuspendedLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse | 
                                                          Qt::LinksAccessibleByKeyboard);
            outputSuspendedLabel->setOpenExternalLinks(true);

            outputSuspendedLabel->setVisible(false);

            gridLayout->addWidget(outputSuspendedLabel);       
            gridLayout->addItem( new QSpacerItem(0,0,QSizePolicy::Expanding,QSizePolicy::Expanding),
                                 1,0);

    }

	outputSuspendedLabel->setVisible(suspended);
}

uint TerminalDisplay::lineSpacing() const
{
  return m_lineSpacing;
}

void TerminalDisplay::setLineSpacing(uint i)
{
  m_lineSpacing = i;
  setVTFont(font()); // Trigger an update.
}

#include "TerminalDisplay.moc"
