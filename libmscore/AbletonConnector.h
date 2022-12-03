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

#include <QTcpSocket>
#include <QFile>
#include <QDebug>
#include <QTimer>

#include <unordered_map>

#include "libmscore/sig.h"
#include "rendermidi.h"


namespace Ms {

class MasterScore;
class Part;
class Staff;
class EventMap;
class NetworkConnector;

class AbletonConnector : public QObject {
    Q_OBJECT
public:
    static const QString XML_TAG;
    static const QString ID_MAP_XML_TAG;
    static const QString MAP_ELEMENT_XML_TAG;
    static const char* MAP_OWN_ID_ATTRIBUTE_NAME;
    static const char* MAP_LIVE_ID_ATTRIBUTE_NAME;

    AbletonConnector(MasterScore& score);
    ~AbletonConnector() override;

    bool isConnected() const;

    void connectSocket();
    void disconnectSocket();

    void synchronizeAll();

    void read(XmlReader& reader);
    void write(XmlWriter& writer);

signals:
    void connectionError(const QString&);
    void socketConnectFailed();
    void socketConnected();
    void socketDisconnected();

    void threadSendJsonDocument(const QJsonDocument&);
    void threadConnectSocket();
    void threadDisconnectSocket();

private:
    struct NoteData {
        bool mIsPlaying;
        int mOnTick;
        int mVelocity;
    };

    struct DirtyMusicRange {
        int mStartTick;
        int mTickDuration;
        Staff* mStaff;
    };

    static constexpr uint32_t APPLY_CHANGES_TIMEOUT = 30000;
    QFile mDebugFile;
    std::unique_ptr<QDebug> mDebugPtr;
    
    MasterScore& mScore;
    TimeSigFrac mTimeSignature;
    MidiRenderer mMidiRenderer;

    NetworkConnector* mNetworkConnectorPtr;
    QThread mNetworkThread;
    bool mIsConnected = false;
    QTimer mApplyChangesTimer;
    std::vector<DirtyMusicRange> mDirtyRanges;
    std::unordered_map<std::uint64_t, std::uint64_t> mOwnNoteIdToLiveNoteIdMap;

    void synchronizeRange(const DirtyMusicRange& range);
    void removeDirtyRanges(Part* part, int startTick, int endTick);

    QJsonObject createPartJsonData(EventMap& events, Part* part);
    void createStaffJsonData(EventMap& events, Staff* staff, Part* part, QJsonArray& notes);
    void createChannelJsonData(EventMap& events,
        Staff* staff,
        Part* part,
        char midiPort,
        char midiChannel,
        QJsonArray& notes);
    QJsonObject createNote(int onTick, int offTick, int pitch, int velocity, std::uint64_t noteId);

    float scoreEndBeat(Score& score);
    float tickToBeat(int tick) const;

    void updateTimeSignature();
    void makePartNamesUnique();

private slots:
    void onConnected();
    void onDisconnected();
    void onApplyChanges();
    void onMusicChanged(int startTick, int tickDuration, Staff* staff);
    void handleReceivedMessage(const QJsonDocument& message);
};

class NetworkConnector : public QObject {
    Q_OBJECT

public:
    NetworkConnector();

public slots:
    void connectSocket();
    void disconnectSocket();
    void sendJsonDocument(const QJsonDocument& document);
    void onReadReady();

private:
    static const quint16 PORT;
    bool mIsTryingToConnect = false;
    QTcpSocket* mSocketPtr = nullptr;

    void readMessage();
    std::vector<uint8_t> receiveCount(uint64_t byteCount);
    uint64_t readFromSocket(uint8_t* dataPtr, qint64 maxSize);

signals:
    void errorOccurred(const QString&);
    void socketConnectFailed();
    void socketConnected();
    void socketDisconnected();
    void handleMessage(const QJsonDocument& message);

private slots:
    void handleSocketError(QAbstractSocket::SocketError error);
    void onConnected();
};

} // namespace Ms


