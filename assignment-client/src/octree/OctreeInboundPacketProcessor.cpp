//
//  OctreeInboundPacketProcessor.cpp
//  assignment-client/src/octree
//
//  Created by Brad Hefta-Gaub on 8/21/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <PacketHeaders.h>
#include <PerfStat.h>

#include "OctreeServer.h"
#include "OctreeServerConsts.h"
#include "OctreeInboundPacketProcessor.h"

static QUuid DEFAULT_NODE_ID_REF;

OctreeInboundPacketProcessor::OctreeInboundPacketProcessor(OctreeServer* myServer) :
    _myServer(myServer),
    _receivedPacketCount(0),
    _totalTransitTime(0),
    _totalProcessTime(0),
    _totalLockWaitTime(0),
    _totalElementsInPacket(0),
    _totalPackets(0),
    _lastNackTime(usecTimestampNow())
{
}

void OctreeInboundPacketProcessor::resetStats() {
    _totalTransitTime = 0;
    _totalProcessTime = 0;
    _totalLockWaitTime = 0;
    _totalElementsInPacket = 0;
    _totalPackets = 0;
    _lastNackTime = usecTimestampNow();

    _singleSenderStats.clear();
}

bool OctreeInboundPacketProcessor::process() {

    const quint64 TOO_LONG_SINCE_LAST_NACK = 1 * USECS_PER_SECOND;
    quint64 now = usecTimestampNow();

    if (_packets.size() == 0) {
        // calculate time until next sendNackPackets()
        quint64 nextNackTime = _lastNackTime + TOO_LONG_SINCE_LAST_NACK;
        quint64 now = usecTimestampNow();
        if (now >= nextNackTime) {
            // send nacks if we're already past time to send it
            _lastNackTime = now;
            sendNackPackets();
        }
        else {
            // otherwise, wait until the next nack time or until a packet arrives
            quint64 waitTimeMsecs = (nextNackTime - now) / USECS_PER_MSEC + 1;
            _waitingOnPacketsMutex.lock();
            _hasPackets.wait(&_waitingOnPacketsMutex, waitTimeMsecs);
            _waitingOnPacketsMutex.unlock();
        }
    }
    while (_packets.size() > 0) {
        lock(); // lock to make sure nothing changes on us
        NetworkPacket& packet = _packets.front(); // get the oldest packet
        NetworkPacket temporary = packet; // make a copy of the packet in case the vector is resized on us
        _packets.erase(_packets.begin()); // remove the oldest packet
        _nodePacketCounts[temporary.getNode()->getUUID()]--;
        unlock(); // let others add to the packets
        processPacket(temporary.getNode(), temporary.getByteArray()); // process our temporary copy

        // if it's time to send nacks, send them.
        if (usecTimestampNow() - _lastNackTime >= TOO_LONG_SINCE_LAST_NACK) {
            _lastNackTime = now;
            sendNackPackets();
        }
    }
    return isStillRunning();  // keep running till they terminate us
}


void OctreeInboundPacketProcessor::processPacket(const SharedNodePointer& sendingNode, const QByteArray& packet) {

    bool debugProcessPacket = _myServer->wantsVerboseDebug();

    if (debugProcessPacket) {
        qDebug("OctreeInboundPacketProcessor::processPacket() packetData=%p packetLength=%d", &packet, packet.size());
    }

    int numBytesPacketHeader = numBytesForPacketHeader(packet);


    // Ask our tree subclass if it can handle the incoming packet...
    PacketType packetType = packetTypeForPacket(packet);
    if (_myServer->getOctree()->handlesEditPacketType(packetType)) {
        PerformanceWarning warn(debugProcessPacket, "processPacket KNOWN TYPE",debugProcessPacket);
        _receivedPacketCount++;
        
        const unsigned char* packetData = reinterpret_cast<const unsigned char*>(packet.data());

        unsigned short int sequence = (*((unsigned short int*)(packetData + numBytesPacketHeader)));
        quint64 sentAt = (*((quint64*)(packetData + numBytesPacketHeader + sizeof(sequence))));
        quint64 arrivedAt = usecTimestampNow();
        quint64 transitTime = arrivedAt - sentAt;
        int editsInPacket = 0;
        quint64 processTime = 0;
        quint64 lockWaitTime = 0;

        if (_myServer->wantsDebugReceiving()) {
            qDebug() << "PROCESSING THREAD: got '" << packetType << "' packet - " << _receivedPacketCount
                    << " command from client receivedBytes=" << packet.size()
                    << " sequence=" << sequence << " transitTime=" << transitTime << " usecs";
        }
        int atByte = numBytesPacketHeader + sizeof(sequence) + sizeof(sentAt);
        unsigned char* editData = (unsigned char*)&packetData[atByte];
        while (atByte < packet.size()) {
            int maxSize = packet.size() - atByte;

            if (debugProcessPacket) {
                qDebug("OctreeInboundPacketProcessor::processPacket() %c "
                       "packetData=%p packetLength=%d voxelData=%p atByte=%d maxSize=%d",
                        packetType, packetData, packet.size(), editData, atByte, maxSize);
            }

            quint64 startLock = usecTimestampNow();
            _myServer->getOctree()->lockForWrite();
            quint64 startProcess = usecTimestampNow();
            int editDataBytesRead = _myServer->getOctree()->processEditPacketData(packetType,
                                                                                  reinterpret_cast<const unsigned char*>(packet.data()),
                                                                                  packet.size(),
                                                                                  editData, maxSize, sendingNode);
            _myServer->getOctree()->unlock();
            quint64 endProcess = usecTimestampNow();

            editsInPacket++;
            quint64 thisProcessTime = endProcess - startProcess;
            quint64 thisLockWaitTime = startProcess - startLock;
            processTime += thisProcessTime;
            lockWaitTime += thisLockWaitTime;

            // skip to next voxel edit record in the packet
            editData += editDataBytesRead;
            atByte += editDataBytesRead;
        }

        if (debugProcessPacket) {
            qDebug("OctreeInboundPacketProcessor::processPacket() DONE LOOPING FOR %c "
                   "packetData=%p packetLength=%d voxelData=%p atByte=%d",
                    packetType, packetData, packet.size(), editData, atByte);
        }

        // Make sure our Node and NodeList knows we've heard from this node.
        QUuid& nodeUUID = DEFAULT_NODE_ID_REF;
        if (sendingNode) {
            sendingNode->setLastHeardMicrostamp(usecTimestampNow());
            nodeUUID = sendingNode->getUUID();
            if (debugProcessPacket) {
                qDebug() << "sender has uuid=" << nodeUUID;
            }
        } else {
            if (debugProcessPacket) {
                qDebug() << "sender has no known nodeUUID.";
            }
        }
        trackInboundPacket(nodeUUID, sequence, transitTime, editsInPacket, processTime, lockWaitTime);
    } else {
        qDebug("unknown packet ignored... packetType=%d", packetType);
    }
}

void OctreeInboundPacketProcessor::trackInboundPacket(const QUuid& nodeUUID, unsigned short int sequence, quint64 transitTime,
            int editsInPacket, quint64 processTime, quint64 lockWaitTime) {

    _totalTransitTime += transitTime;
    _totalProcessTime += processTime;
    _totalLockWaitTime += lockWaitTime;
    _totalElementsInPacket += editsInPacket;
    _totalPackets++;

    // find the individual senders stats and track them there too...
    // see if this is the first we've heard of this node...
    if (_singleSenderStats.find(nodeUUID) == _singleSenderStats.end()) {
        SingleSenderStats stats;
        stats.trackInboundPacket(sequence, transitTime, editsInPacket, processTime, lockWaitTime);
        _singleSenderStats[nodeUUID] = stats;
    } else {
        SingleSenderStats& stats = _singleSenderStats[nodeUUID];
        stats.trackInboundPacket(sequence, transitTime, editsInPacket, processTime, lockWaitTime);
    }
}

int OctreeInboundPacketProcessor::sendNackPackets() {

    int packetsSent = 0;

    NodeToSenderStatsMapIterator i = _singleSenderStats.begin();
    while (i != _singleSenderStats.end()) {

        QUuid nodeUUID = i.key();
        SingleSenderStats nodeStats = i.value();

        // check if this node is still alive.  Remove its stats if it's dead.
        if (!isAlive(nodeUUID)) {
            i = _singleSenderStats.erase(i);
            continue;
        }

        // if there are packets from _node that are waiting to be processed,
        // don't send a NACK since the missing packets may be among those waiting packets.
        if (hasPacketsToProcessFrom(nodeUUID)) {
            continue;
        }

        const SharedNodePointer& destinationNode = NodeList::getInstance()->getNodeHash().value(nodeUUID);
        const QSet<unsigned short int>& missingSequenceNumbers = nodeStats.getMissingSequenceNumbers();

        // check if there are any sequence numbers that need to be nacked
        int numSequenceNumbersAvailable = missingSequenceNumbers.size();

        // construct nack packet(s) for this node

        QSet<unsigned short int>::const_iterator missingSequenceNumberIterator = missingSequenceNumbers.begin();
        char packet[MAX_PACKET_SIZE];

        while (numSequenceNumbersAvailable > 0) {

            char* dataAt = packet;
            int bytesRemaining = MAX_PACKET_SIZE;

            // pack header
            int numBytesPacketHeader = populatePacketHeader(packet, _myServer->getMyEditNackType());
            dataAt += numBytesPacketHeader;
            bytesRemaining -= numBytesPacketHeader;

            // calculate and pack the number of sequence numbers to nack
            int numSequenceNumbersRoomFor = (bytesRemaining - sizeof(uint16_t)) / sizeof(unsigned short int);
            uint16_t numSequenceNumbers = std::min(numSequenceNumbersAvailable, numSequenceNumbersRoomFor);
            uint16_t* numSequenceNumbersAt = (uint16_t*)dataAt;
            *numSequenceNumbersAt = numSequenceNumbers;
            dataAt += sizeof(uint16_t);

            // pack sequence numbers to nack
            for (uint16_t i = 0; i < numSequenceNumbers; i++) {
                unsigned short int* sequenceNumberAt = (unsigned short int*)dataAt;
                *sequenceNumberAt = *missingSequenceNumberIterator;
                dataAt += sizeof(unsigned short int);

                missingSequenceNumberIterator++;
            }
            numSequenceNumbersAvailable -= numSequenceNumbers;

            // send it
            qint64 bytesWritten = NodeList::getInstance()->writeUnverifiedDatagram(packet, dataAt - packet, destinationNode);

            packetsSent++;
        }
        i++;
    }
    return packetsSent;
}


SingleSenderStats::SingleSenderStats()
    : _totalTransitTime(0),
    _totalProcessTime(0),
    _totalLockWaitTime(0),
    _totalElementsInPacket(0),
    _totalPackets(0),
    _incomingLastSequence(0),
    _missingSequenceNumbers()
{

}

void SingleSenderStats::trackInboundPacket(unsigned short int incomingSequence, quint64 transitTime,
    int editsInPacket, quint64 processTime, quint64 lockWaitTime) {

    const int UINT16_RANGE = UINT16_MAX + 1;

    const int  MAX_REASONABLE_SEQUENCE_GAP = 1000;  // this must be less than UINT16_RANGE / 2 for rollover handling to work
    const int MAX_MISSING_SEQUENCE_SIZE = 100;

    unsigned short int expectedSequence = _totalPackets == 0 ? incomingSequence : _incomingLastSequence + (unsigned short int)1;
    
    if (incomingSequence == expectedSequence) {         // on time
        _incomingLastSequence = incomingSequence;
    }
    else {                                              // out of order

        int incoming = (int)incomingSequence;
        int expected = (int)expectedSequence;

        // check if the gap between incoming and expected is reasonable, taking possible rollover into consideration
        int absGap = std::abs(incoming - expected);
        if (absGap >= UINT16_RANGE - MAX_REASONABLE_SEQUENCE_GAP) {
            // rollover likely occurred between incoming and expected.
            // correct the larger of the two so that it's within [-UINT16_RANGE, -1] while the other remains within [0, UINT16_RANGE-1]
            if (incoming > expected) {
                incoming -= UINT16_RANGE;
            } else {
                expected -= UINT16_RANGE;
            }
        } else if (absGap > MAX_REASONABLE_SEQUENCE_GAP) {
            // ignore packet if gap is unreasonable
            qDebug() << "ignoring unreasonable packet... sequence:" << incomingSequence
                << "_incomingLastSequence:" << _incomingLastSequence;
            return;
        }
        
        // now that rollover has been corrected for (if it occurred), incoming and expected can be
        // compared to each other directly, though one of them might be negative

        if (incoming > expected) {                          // early

            // add all sequence numbers that were skipped to the missing sequence numbers list
            for (int missingSequence = expected; missingSequence < incoming; missingSequence++) {
                _missingSequenceNumbers.insert(missingSequence < 0 ? missingSequence + UINT16_RANGE : missingSequence);
            }
            _incomingLastSequence = incomingSequence;

        } else {                                            // late

            // remove this from missing sequence number if it's in there
            _missingSequenceNumbers.remove(incomingSequence);

            // do not update _incomingLastSequence; it shouldn't become smaller
        }
    }

    // prune missing sequence list if it gets too big; sequence numbers that are older than MAX_REASONABLE_SEQUENCE_GAP
    // will be removed.
    if (_missingSequenceNumbers.size() > MAX_MISSING_SEQUENCE_SIZE) {
        
        // some older sequence numbers may be from before a rollover point; this must be handled.
        // some sequence numbers in this list may be larger than _incomingLastSequence, indicating that they were received
        // before the most recent rollover.
        int cutoff = (int)_incomingLastSequence - MAX_REASONABLE_SEQUENCE_GAP;
        if (cutoff >= 0) {
            foreach(unsigned short int missingSequence, _missingSequenceNumbers) {
                unsigned short int nonRolloverCutoff = (unsigned short int)cutoff;
                if (missingSequence > _incomingLastSequence || missingSequence <= nonRolloverCutoff) {
                    _missingSequenceNumbers.remove(missingSequence);
                }
            }
        } else {
            unsigned short int rolloverCutoff = (unsigned short int)(cutoff + UINT16_RANGE);
            foreach(unsigned short int missingSequence, _missingSequenceNumbers) {
                if (missingSequence > _incomingLastSequence && missingSequence <= rolloverCutoff) {
                    _missingSequenceNumbers.remove(missingSequence);
                }
            }
        }
    }

    // update other stats
    _totalTransitTime += transitTime;
    _totalProcessTime += processTime;
    _totalLockWaitTime += lockWaitTime;
    _totalElementsInPacket += editsInPacket;
    _totalPackets++;
}
