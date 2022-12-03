//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2002-2014 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#pragma once

#include "abstractdialog.h"
#include "libmscore/AbletonConnector.h"


namespace Ui {
    class AbletonDialog;
}

namespace Ms {

class Score;

//---------------------------------------------------------
//   InstrumentsDialog
//---------------------------------------------------------

class AbletonDialog : public AbstractDialog {
      Q_OBJECT


public:
    AbletonDialog(AbletonConnector& connector, QWidget* parent = 0);
    ~AbletonDialog() override;

public slots:
    void onButtonBoxClicked(QAbstractButton*);
    void onConnectClicked();
    void onSynchronizeClicked();

protected:
    virtual void retranslate();

private:
    Ui::AbletonDialog* mUiPtr;
    AbletonConnector& mAbletonConnector;

    void updateStatus();

private slots:
    void onConnected();
    void onConnectFailed();
    void onDisconnected();
    void onConnectionError();

};

} // namespace Ms


