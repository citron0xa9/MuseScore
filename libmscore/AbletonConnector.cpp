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

#include "AbletonConnector.h"

#include "part.h"
#include "staff.h"
#include "score.h"

#include <QMessageBox>
#include <unordered_map>
#include <cstring>

namespace Ms {

const quint16 NetworkConnector::PORT = 29040;
const QString AbletonConnector::XML_TAG{"AbletonConnector"};
const QString AbletonConnector::ID_MAP_XML_TAG{"IdMap"};
const QString AbletonConnector::MAP_ELEMENT_XML_TAG{"map"};
const char* AbletonConnector::MAP_OWN_ID_ATTRIBUTE_NAME = "ownId";
const char* AbletonConnector::MAP_LIVE_ID_ATTRIBUTE_NAME = "liveId";

AbletonConnector::AbletonConnector(MasterScore& score)
    : mScore{score}
    , mDebugFile{"C:\\Users\\Leonie\\Documents\\MuseScore3Development\\log.txt"}
    , mMidiRenderer{&score}
{
    mScore.setExpandRepeats(true);
    mMidiRenderer.setMinChunkSize(5);
    makePartNamesUnique();
    if (mDebugFile.open(QIODevice::ReadWrite | QIODevice::Text | QIODevice::Truncate)) {
        mDebugPtr = std::make_unique<QDebug>(&mDebugFile);
    } else {
        mDebugPtr = std::make_unique<QDebug>(QtMsgType::QtDebugMsg);
    }
    mNetworkConnectorPtr = new NetworkConnector{};
    connect(&mNetworkThread, &QThread::finished, mNetworkConnectorPtr, &QObject::deleteLater);
    connect(mNetworkConnectorPtr,
        &NetworkConnector::socketConnected,
        this,
        &AbletonConnector::onConnected);
    connect(mNetworkConnectorPtr,
        &NetworkConnector::socketDisconnected,
        this,
        &AbletonConnector::onDisconnected);
    connect(mNetworkConnectorPtr,
        &NetworkConnector::socketConnectFailed,
        this,
        &AbletonConnector::socketConnectFailed);
    connect(mNetworkConnectorPtr,
        &NetworkConnector::errorOccurred,
        this,
        &AbletonConnector::connectionError);
    connect(mNetworkConnectorPtr,
        &NetworkConnector::handleMessage,
        this,
        &AbletonConnector::handleReceivedMessage);
    connect(this,
        &AbletonConnector::threadSendJsonDocument,
        mNetworkConnectorPtr,
        &NetworkConnector::sendJsonDocument);
    connect(this,
        &AbletonConnector::threadConnectSocket,
        mNetworkConnectorPtr,
        &NetworkConnector::connectSocket);
    connect(this,
        &AbletonConnector::threadDisconnectSocket,
        mNetworkConnectorPtr,
        &NetworkConnector::disconnectSocket);
    mNetworkConnectorPtr->moveToThread(&mNetworkThread);
    mNetworkThread.start();
}

AbletonConnector::~AbletonConnector()
{
    mDebugPtr.reset();
    mNetworkThread.quit();
    mNetworkThread.wait();
}

bool AbletonConnector::isConnected() const
{
    return mIsConnected;
}

void AbletonConnector::connectSocket()
{
    emit threadConnectSocket();
}

void AbletonConnector::disconnectSocket()
{
    emit threadDisconnectSocket();
}

void AbletonConnector::synchronizeAll()
{
    assert(isConnected());
    EventMap events;
    
    updateTimeSignature();
    mMidiRenderer.setScoreChanged();

    MidiRenderer::Context renderContext(mScore.synthesizerState());
    renderContext.metronome = false;
    renderContext.renderHarmony = true;
    mMidiRenderer.renderScore(&events, renderContext);

    QJsonObject synchronizeObject;
    QJsonArray tracksArray;

    for (auto* part : mScore.parts()) {
        tracksArray.push_back(createPartJsonData(events, part));
    }
    synchronizeObject["command"] = "SynchronizeTrackRange";
    synchronizeObject["tracks"] = tracksArray;
    synchronizeObject["start"] = 0.0f;
    synchronizeObject["duration"] = scoreEndBeat(mScore);
    synchronizeObject["score_duration"] = scoreEndBeat(mScore);

    QJsonDocument document{synchronizeObject};
    emit threadSendJsonDocument(document);
}

void AbletonConnector::read(XmlReader& reader)
{
    mOwnNoteIdToLiveNoteIdMap.clear();
    while (reader.readNextStartElement()) {
        if (reader.name() == ID_MAP_XML_TAG) {
            while (reader.readNextStartElement()) {
                if (reader.name() == MAP_ELEMENT_XML_TAG) {
                    const auto ownId = reader.attribute(MAP_OWN_ID_ATTRIBUTE_NAME).toULongLong();
                    const auto liveId = reader.attribute(MAP_LIVE_ID_ATTRIBUTE_NAME).toULongLong();
                    mOwnNoteIdToLiveNoteIdMap[ownId] = liveId;
                    reader.skipCurrentElement();
                } else {
                    reader.unknown();
                }
            }
        } else {
            reader.unknown();
        }
    }
}

void AbletonConnector::write(XmlWriter& writer)
{
    writer.stag(XML_TAG);
    writer.stag(ID_MAP_XML_TAG);
    for (const auto& entry : mOwnNoteIdToLiveNoteIdMap) {
        writer.tagE(MAP_ELEMENT_XML_TAG + " " + QString(QString(MAP_OWN_ID_ATTRIBUTE_NAME) + "=%1 " + MAP_LIVE_ID_ATTRIBUTE_NAME + "=%2").arg(entry.first).arg(entry.second));
    }
}

void AbletonConnector::synchronizeRange(const DirtyMusicRange& range)
{
    EventMap events;
    MidiRenderer::Context renderContext(mScore.synthesizerState());
    renderContext.metronome = false;
    renderContext.renderHarmony = true;

    auto* part = range.mStaff ? range.mStaff->part() : nullptr;

    int currentTick = range.mStartTick;
    const int endTick = (range.mTickDuration == -1) ? mScore.endTick().ticks() : range.mStartTick + range.mTickDuration;

    int finalStartTick = -1;
    while (currentTick < endTick) {
        const auto currentChunk = mMidiRenderer.getChunkAt(currentTick);
        if (finalStartTick == -1) {
            finalStartTick = currentChunk.tick1();
        }
        if (part != nullptr) {
            mMidiRenderer.renderPartChunk(currentChunk, part, &events, renderContext);
        } else {
            mMidiRenderer.renderChunk(currentChunk, &events, renderContext);
        }
        currentTick = currentChunk.tick2();
    }
    QJsonObject synchronizeObject;
    QJsonArray tracksArray;

    synchronizeObject["command"] = "SynchronizeTrackRange";
    synchronizeObject["start"] = tickToBeat(finalStartTick);
    synchronizeObject["duration"] = tickToBeat(currentTick-finalStartTick);
    synchronizeObject["score_duration"] = scoreEndBeat(mScore);

    if (part != nullptr) {
        tracksArray.push_back(createPartJsonData(events, part));
    } else {
        for (auto* currentPart : mScore.parts()) {
            tracksArray.push_back(createPartJsonData(events, currentPart));
        }
    }
    synchronizeObject["tracks"] = tracksArray;
    QJsonDocument synchronizeDocument{synchronizeObject};
    emit threadSendJsonDocument(synchronizeDocument);

    removeDirtyRanges(part, range.mStartTick, currentTick);
}

void AbletonConnector::removeDirtyRanges(Part* part, const int startTick, const int endTick)
{
    const auto newEnd = std::remove_if(mDirtyRanges.begin(), mDirtyRanges.end(), [part, startTick, endTick](const auto& range) {
        if (part != nullptr && (range.mStaff == nullptr || range.mStaff->part() != part)) {
            return false;
        }
        return (range.mStartTick >= startTick && (range.mStartTick + range.mTickDuration) <= endTick);
    });
    mDirtyRanges.erase(newEnd, mDirtyRanges.end());
}

QJsonObject AbletonConnector::createPartJsonData(EventMap& events, Part* part)
{
    QJsonObject track;
    track["name"] = part->partName();

    QJsonArray notes;
    for (auto* staff : *part->staves()) {
        createStaffJsonData(events, staff, part, notes);
    }
    track["notes"] = notes;
    return track;
}

void AbletonConnector::createStaffJsonData(EventMap& events, Staff* staff, Part* part, QJsonArray& notes)
{
    const auto* instrumentList = part->instruments();
    for (auto instrumentIt = instrumentList->begin(); instrumentIt != instrumentList->end(); ++instrumentIt) {
        for (const auto* instrumentChannel : instrumentIt->second->channel()) {
            const auto* playbackChannel = part->masterScore()->playbackChannel(instrumentChannel);
            char midiPort = part->masterScore()->midiPort(playbackChannel->channel());
            char midiChannel = part->masterScore()->midiChannel(playbackChannel->channel());

            createChannelJsonData(events, staff, part, midiPort, midiChannel, notes);
        }
    }
}

void AbletonConnector::createChannelJsonData(EventMap& events,
    Staff* staff,
    Part* part,
    const char midiPort,
    const char midiChannel,
    QJsonArray& notes)
{
    NoteData notesData[128]; //for each pitch
    std::memset(notesData, 0, sizeof(notesData));
    const auto staffIndex = staff->idx();
    for (auto& entry : events) {
        auto& event = entry.second;
        //if (event.isMuted()) muted to prevent muting mixer to have influence 
        //    continue;
        if (event.discard() == staffIndex + 1 && event.velo() > 0 && notesData[event.pitch()].mIsPlaying) {
            // end note so we can restrike it in another track
            (*mDebugPtr) << "Restriking note for staff index " << staffIndex << " pitch " << event.pitch(); 
            auto& currentNoteData = notesData[event.pitch()];
            notes.append(createNote(currentNoteData.mOnTick, entry.first, event.pitch(), currentNoteData.mVelocity, event.note()->noteId()));
            currentNoteData.mIsPlaying = false;
        }

        if (event.getOriginatingStaff() != staffIndex)
            continue;

        if (event.discard() && event.velo() == 0)
            // ignore noteoff but restrike noteon
            continue;

        char eventPort = mScore.midiPort(event.channel());
        char eventChannel = mScore.midiChannel(event.channel());
        if (midiPort != eventPort || midiChannel != eventChannel)
            continue;

        if (event.type() == ME_NOTEON) {
            const auto pitch = event.portamento() ? event.note()->pitch() : event.pitch();
            auto& currentNoteData = notesData[pitch];
            if (event.velo() > 0 && !currentNoteData.mIsPlaying) {
                currentNoteData.mIsPlaying = true;
                currentNoteData.mOnTick = entry.first;
                currentNoteData.mVelocity = event.velo();
            } else if (event.velo() == 0 && currentNoteData.mIsPlaying) {
                notes.append(createNote(currentNoteData.mOnTick, entry.first, pitch, currentNoteData.mVelocity, event.note()->noteId()));
                currentNoteData.mIsPlaying = false;
            } else {
                (*mDebugPtr) << "Part: " << part->partName() << "Staff index " << staff->idx() << "Pitch " << pitch
                             << "Beat " << tickToBeat(entry.first)
                             << " Event velo is " << event.velo() << " but playing is " << currentNoteData.mIsPlaying;
            }
        }
    }
}

QJsonObject AbletonConnector::createNote(int onTick, int offTick, int pitch, int velocity, std::uint64_t noteId)
{
    QJsonObject note;
    const float startTimeBeat = tickToBeat(onTick);
    const float durationBeats = tickToBeat(offTick - onTick + 1);  //+1 because in rendermidi.cpp:372 -1 is used. Using +1 gets the correct note length.
        //It seems the note off might be considered inclusive, i.e. the tick of note off is still a tick where the note plays??

    note["start_time"] = startTimeBeat;
    note["duration"] = durationBeats;
    note["pitch"] = pitch;
    note["velocity"] = velocity;
    note["musescoreId"] = static_cast<qint64>(noteId);
    if (mOwnNoteIdToLiveNoteIdMap.count(noteId) > 0) {
        note["liveId"] = static_cast<qint64>(mOwnNoteIdToLiveNoteIdMap[noteId]);
    }
    return note;
}

float AbletonConnector::scoreEndBeat(Score& score)
{
    return tickToBeat(score.endTick().ticks());
}

float AbletonConnector::tickToBeat(const int tick) const
{
    const auto beatTicks = mTimeSignature.beatTicks();
    return static_cast<float>(tick) / beatTicks;
}

void AbletonConnector::updateTimeSignature()
{
    mTimeSignature = mScore.sigmap()->timesig(0).timesig();
}

void AbletonConnector::makePartNamesUnique()
{
    std::unordered_map<QString, unsigned int> usedPartNames;

    for (auto* part : mScore.parts()) {
        auto partName = part->partName();
        if (usedPartNames.count(partName) > 0) {
            const auto id = usedPartNames[partName] + 1;
            usedPartNames[partName] = id;
            part->setPartName(QString{" - "} + QString::number(id));
        } else {
            usedPartNames[partName] = 1;
        }
    }
}

void AbletonConnector::onConnected()
{
    mIsConnected = true;

    assert(mDirtyRanges.empty());
    connect(&mApplyChangesTimer, &QTimer::timeout, this, &AbletonConnector::onApplyChanges);
    connect(&mScore, &Score::musicChanged, this, &AbletonConnector::onMusicChanged);
    mApplyChangesTimer.start(std::chrono::milliseconds(APPLY_CHANGES_TIMEOUT));

    QMessageBox::information(nullptr, "Ableton Connection", "Successfully connected");
    emit socketConnected();
}

void AbletonConnector::onDisconnected()
{
    mIsConnected = false;

    mApplyChangesTimer.disconnect(this);
    mScore.disconnect(this);
    mApplyChangesTimer.stop();
    mDirtyRanges.clear();

    QMessageBox::information(nullptr, "Ableton Connection", "Successfully disconnected");
    emit socketDisconnected();
}

void AbletonConnector::onApplyChanges()
{
    if (!isConnected()) {
        return;
    }
    updateTimeSignature();
    mMidiRenderer.setScoreChanged();
    std::sort(mDirtyRanges.begin(), mDirtyRanges.end(), [](auto& firstRange, auto& lastRange) {
        return firstRange.mStartTick < lastRange.mStartTick;
    });
    while (!mDirtyRanges.empty()) {
        synchronizeRange(mDirtyRanges.front());
    }
}

void AbletonConnector::onMusicChanged(const int startTick, const int tickDuration, Staff* const staff)
{
    assert(mIsConnected);
    mDirtyRanges.push_back(DirtyMusicRange{startTick, tickDuration, staff});
    std::cout << ">>>>>>>>>>On Music Changed!!\n";
}

void AbletonConnector::handleReceivedMessage(const QJsonDocument& message)
{
    qDebug() << "Received message: " << message;
    if (message["command"] == "SynchronizeNoteIds") {
        auto noteIdMap = message["NoteIdMap"].toObject();
        for (auto it = noteIdMap.constBegin(); it != noteIdMap.constEnd(); ++it) {
            mOwnNoteIdToLiveNoteIdMap[static_cast<std::uint64_t>(it.key().toDouble())] = static_cast<std::uint64_t>(it.value().toDouble());
        }
    }
}

void NetworkConnector::connectSocket()
{
    mIsTryingToConnect = true;
    mSocketPtr->connectToHost(QHostAddress::LocalHost, PORT);
}

void NetworkConnector::disconnectSocket()
{
    mSocketPtr->disconnectFromHost();
}

void NetworkConnector::sendJsonDocument(const QJsonDocument& document)
{
    const auto data = document.toJson(QJsonDocument::JsonFormat::Compact);
    const auto dataSize = qToBigEndian(static_cast<unsigned int>(data.size()));
    if (mSocketPtr->write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize)) != sizeof(dataSize)) {
        emit errorOccurred("Failed to write message size");
    }
    if (mSocketPtr->write(data) != data.size()) {
        emit errorOccurred("Failed to write message");
    }
}

void NetworkConnector::onReadReady()
{
    readMessage();
}

void NetworkConnector::readMessage()
{
    const auto messageSizeData = receiveCount(4);
    const unsigned int messageSize = qFromBigEndian<unsigned int>(messageSizeData.data());
    const auto messageData = receiveCount(messageSize);
    const QByteArray messageDataArray = QByteArray::fromRawData(reinterpret_cast<const char*>(messageData.data()), static_cast<int>(messageSize));
    QJsonDocument messageJson = QJsonDocument::fromJson(messageDataArray);
    if (messageJson.isNull()) {
        throw std::runtime_error("convert received message to json failed");
    }
    emit handleMessage(messageJson);
}

std::vector<uint8_t> NetworkConnector::receiveCount(const uint64_t byteCount)
{
    assert(byteCount > 0);
    std::vector<uint8_t> data(byteCount);
    
    constexpr auto maxSleepTime = std::chrono::seconds(1);
    constexpr auto loopSleepTime = std::chrono::milliseconds(50);
    auto sleepTimeElapsed = std::chrono::milliseconds(0);

    uint64_t bytesReadTotal = 0;
    bytesReadTotal += readFromSocket(data.data(), byteCount);
    while (bytesReadTotal < byteCount && sleepTimeElapsed < maxSleepTime) {
        std::this_thread::sleep_for(loopSleepTime);
        sleepTimeElapsed += loopSleepTime;
        bytesReadTotal += readFromSocket(data.data(), byteCount - bytesReadTotal);
    }
    if (bytesReadTotal < byteCount) {
        throw std::runtime_error("Failed to receive count bytes");
    }
    assert(byteCount == bytesReadTotal);
    return data;
}

uint64_t NetworkConnector::readFromSocket(uint8_t* dataPtr, const qint64 maxSize)
{
    qint64 bytesRead;
    bytesRead = mSocketPtr->read(reinterpret_cast<char*>(dataPtr), maxSize);
    if (bytesRead <= 0) {
        throw std::runtime_error("Failed to read bytes from socket");
    }
    return static_cast<uint64_t>(bytesRead);
}

void NetworkConnector::handleSocketError(QAbstractSocket::SocketError error)
{
    if (mIsTryingToConnect) {
        mIsTryingToConnect = false;
        emit socketConnectFailed();
        return;
    }
    if (error == QAbstractSocket::SocketError::RemoteHostClosedError) {
        return;
    }
    emit errorOccurred("Unknown socket error");
}

void NetworkConnector::onConnected()
{
    mIsTryingToConnect = false;
    emit socketConnected();
}

NetworkConnector::NetworkConnector()
{
    mSocketPtr = new QTcpSocket{this};
    connect(mSocketPtr,
        &QAbstractSocket::errorOccurred,
        this,
        &NetworkConnector::handleSocketError);
    connect(mSocketPtr, &QAbstractSocket::connected, this, &NetworkConnector::onConnected);
    connect(mSocketPtr,
        &QAbstractSocket::disconnected,
        this,
        &NetworkConnector::socketDisconnected);
    connect(mSocketPtr, &QAbstractSocket::readyRead, this, &NetworkConnector::onReadReady);
}

} // namespace Ms


