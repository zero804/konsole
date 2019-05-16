#ifndef TERMINAL_WIDGET_H
#define TERMINAL_WIDGET_H

#include <QWidget>

namespace Konsole {
    class TerminalHeaderBar;
    class TerminalDisplay;
    class Session;
    
    class TerminalWidget : public QWidget {
        Q_OBJECT
    public:
        TerminalWidget(uint randomSeed, QWidget *parent = nullptr);
        TerminalHeaderBar *headerBar() const;
        TerminalDisplay *terminalDisplay() const;
    private:
        TerminalHeaderBar *m_headerBar;
        TerminalDisplay *m_terminalDisplay;
    };
};

#endif
