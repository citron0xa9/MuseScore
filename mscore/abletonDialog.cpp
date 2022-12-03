//=============================================================================
//  MuseScore
//  Linux Music Score Editor
//
//  Copyright (C) 2002-2009 Werner Schweer and others
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//=============================================================================

#include "abletonDialog.h"
#include "musescore.h"
#include "ui_abletonDialog.h"

#include <QMessageBox>

namespace Ms {

//---------------------------------------------------------
//   InstrumentsDialog
//---------------------------------------------------------

AbletonDialog::AbletonDialog(AbletonConnector& connector, QWidget* parent)
    : AbstractDialog(parent),
    mUiPtr{ new Ui::AbletonDialog{} },
    mAbletonConnector{ connector }
{
    mUiPtr->setupUi(this);
    setWindowFlags(this->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    addAction(getAction(MuseScore::ABLETON_ACTION));
    updateStatus();

    connect(&mAbletonConnector, &AbletonConnector::socketConnected, this, &AbletonDialog::onConnected);
    connect(&mAbletonConnector, &AbletonConnector::socketConnectFailed, this, &AbletonDialog::onConnectFailed);
    connect(&mAbletonConnector, &AbletonConnector::socketDisconnected, this, &AbletonDialog::onDisconnected);
    connect(&mAbletonConnector, &AbletonConnector::connectionError, this, &AbletonDialog::onConnectionError);
}


AbletonDialog::~AbletonDialog()
{
    delete mUiPtr;
}

void AbletonDialog::retranslate()
{
    mUiPtr->retranslateUi(this);
}

void AbletonDialog::onButtonBoxClicked(QAbstractButton* button)
{
    switch (mUiPtr->buttonBox->buttonRole(button)) {
        case QDialogButtonBox::AcceptRole:
            accept();
            break;
        case QDialogButtonBox::RejectRole:
            reject();
            break;
        default:
            assert(false);
            reject();
            break;
    }
}

void AbletonDialog::updateStatus()
{
    const bool isConnected = mAbletonConnector.isConnected();
    mUiPtr->synchronizeButton->setEnabled(isConnected);
    const char* connectButtonText = isConnected ? "Disconnect" : "Connect";
    const char* statusLabelText = isConnected ? "Connected" : "Not connected";
    mUiPtr->connectButton->setText(connectButtonText);
    mUiPtr->statusLabel->setText(statusLabelText);
}

void AbletonDialog::onConnectClicked()
{
    if (mAbletonConnector.isConnected()) {
        mAbletonConnector.disconnectSocket();
    } else {
        mAbletonConnector.connectSocket();
    }
}

void AbletonDialog::onSynchronizeClicked()
{
    try {
        mAbletonConnector.synchronizeAll(); 
    } catch (const std::runtime_error& error) {
        QMessageBox::critical(this, "Synchronize Error", "Failed to synchronize with ableton");
    }

}

void AbletonDialog::onConnected()
{
    updateStatus();
}

void AbletonDialog::onConnectFailed()
{
    QMessageBox::warning(this, "Error", "Failed to connect");
}

void AbletonDialog::onDisconnected()
{
    updateStatus();
}

void AbletonDialog::onConnectionError()
{
    QTimer::singleShot(0, [this]() { updateStatus(); });
}

void MuseScore::showAbletonDialog()
{
    if (cs == nullptr) {
        return;
    }
    auto* dialog = new AbletonDialog(cs->masterScore()->abletonConnector());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->exec();
}

}
