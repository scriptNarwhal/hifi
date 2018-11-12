//
//  OctreeDataUtils.cpp
//  libraries/octree/src
//
//  Created by Ryan Huffman 2018-02-26
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html

#include "OctreeDataUtils.h"
#include "OctreeEntitiesFileParser.h"

#include <Gzip.h>
#include <udt/PacketHeaders.h>

#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>

namespace {
    // Reads octree file and parses it into a QJsonDocument. Handles both gzipped and non-gzipped files.
    // Returns true if the file was successfully opened and parsed, otherwise false.
    // Example failures: file does not exist, gzipped file cannot be unzipped, invalid JSON.
    bool readOctreeFile(QString path, QJsonDocument* doc) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical() << "Cannot open json file for reading: " << path;
            return false;
        }

        QByteArray data = file.readAll();
        QByteArray jsonData;

        if (!gunzip(data, jsonData)) {
            jsonData = data;
        }

        QJsonParseError parserError;
        *doc = QJsonDocument::fromJson(jsonData, &parserError);
        if (parserError.error != QJsonParseError::NoError) {
            qWarning() << "Error reading JSON file" << path << "-" << parserError.errorString();
        }
        return !doc->isNull();
    }

}  // Anon namespace.

bool OctreeUtils::RawOctreeData::readOctreeDataInfoFromJSON(QJsonObject root) {
    if (root.contains("Id") && root.contains("DataVersion") && root.contains("Version")) {
        id = root["Id"].toVariant().toUuid();
        dataVersion = root["DataVersion"].toInt();
        version = root["Version"].toInt();
    }
    readSubclassData(root);
    return true;
}

bool OctreeUtils::RawOctreeData::readOctreeDataInfoFromMap(const QVariantMap& map) {
    if (map.contains("Id") && map.contains("DataVersion") && map.contains("Version")) {
        id = map["Id"].toUuid();
        dataVersion = map["DataVersion"].toInt();
        version = map["Version"].toInt();
    }
    readSubclassData(map);
    return true;
}

bool OctreeUtils::RawOctreeData::readOctreeDataInfoFromData(QByteArray data) {
    QByteArray jsonData;
    if (gunzip(data, jsonData)) {
        data = jsonData;
    }

    OctreeEntitiesFileParser jsonParser;
    jsonParser.setEntitiesString(data);
    QVariantMap entitiesMap;
    if (!jsonParser.parseEntities(entitiesMap)) {
        qCritical() << "Can't parse Entities JSON: " << jsonParser.getErrorString().c_str();
        return false;
    }

    return readOctreeDataInfoFromMap(entitiesMap);
}

// Reads octree file and parses it into a RawOctreeData object.
// Returns false if readOctreeFile fails.
bool OctreeUtils::RawOctreeData::readOctreeDataInfoFromFile(QString path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "Cannot open json file for reading: " << path;
        return false;
    }

    QByteArray data = file.readAll();
    QByteArray jsonData;

    if (!gunzip(data, jsonData)) {
        jsonData = data;
    }

    return readOctreeDataInfoFromData(jsonData);
}

QByteArray OctreeUtils::RawOctreeData::toByteArray() {
    QJsonObject obj {
        { "DataVersion", QJsonValue((qint64)dataVersion) },
        { "Id", QJsonValue(id.toString()) },
        { "Version", QJsonValue((qint64)version) },
    };

    writeSubclassData(obj);

    QJsonDocument doc;
    doc.setObject(obj);

    return doc.toJson();
}

QByteArray OctreeUtils::RawOctreeData::toGzippedByteArray() {
    auto data = toByteArray();
    QByteArray gzData;

    if (!gzip(data, gzData, -1)) {
        qCritical("Unable to gzip data while converting json.");
        return QByteArray();
    }

    return gzData;
}

PacketType OctreeUtils::RawOctreeData::dataPacketType() const {
    Q_ASSERT(false);
    qCritical() << "Attemping to read packet type for incomplete base type 'RawOctreeData'";
    return (PacketType)0;
}

void OctreeUtils::RawOctreeData::resetIdAndVersion() {
    id = QUuid::createUuid();
    dataVersion = OctreeUtils::INITIAL_VERSION;
    qDebug() << "Reset octree data to: " << id << dataVersion;
}

void OctreeUtils::RawEntityData::readSubclassData(const QJsonObject& root) {
    if (root.contains("Entities")) {
        entityData = root["Entities"].toArray();
    }
}

void OctreeUtils::RawEntityData::readSubclassData(const QVariantMap& root) {
    variantEntityData = root["Entities"].toList();
}

void OctreeUtils::RawEntityData::writeSubclassData(QJsonObject& root) const {
    //root["Entities"] = entityData;
    QJsonArray entitiesJsonArray;
    for (auto entityIter = variantEntityData.begin(); entityIter != variantEntityData.end(); ++entityIter) {
        entitiesJsonArray += entityIter->toJsonObject();
    }

    root["Entities"] = entitiesJsonArray;
}

PacketType OctreeUtils::RawEntityData::dataPacketType() const { return PacketType::EntityData; }
