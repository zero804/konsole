/*
    SPDX-FileCopyrightText: 1997, 1998 Lars Doelle <lars.doelle@on-line.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef HISTORYSCROLLFILE_H
#define HISTORYSCROLLFILE_H

#include "konsoleprivate_export.h"

// History
#include "HistoryFile.h"
#include "HistoryScroll.h"

namespace Konsole
{

//////////////////////////////////////////////////////////////////////
// File-based history (e.g. file log, no limitation in length)
//////////////////////////////////////////////////////////////////////

class KONSOLEPRIVATE_EXPORT HistoryScrollFile : public HistoryScroll
{
public:
    explicit HistoryScrollFile();
    ~HistoryScrollFile() override;

    int  getLines() override;
    int  getMaxLines() override;
    int  getLineLen(int lineno) override;
    void getCells(int lineno, int colno, int count, Character res[]) override;
    bool isWrappedLine(int lineno) override;

    void addCells(const Character text[], int count) override;
    void addLine(bool previousWrapped = false) override;

    void insertCellsVector(int position, const QVector<Character> &cells) override;
    void insertCells(int position, const Character a[], int count) override;
    void removeCells(int position) override;
    void setCellsAt(int position, const Character a[], int count) override;
    void setCellsVectorAt(int position, const QVector<Character> &cells) override;
    void setLineAt(int position, bool previousWrapped) override;

private:
    qint64 startOfLine(int lineno);

    HistoryFile _index; // lines Row(qint64)
    HistoryFile _cells; // text  Row(Character)
    HistoryFile _lineflags; // flags Row(unsigned char)
};

}

#endif
