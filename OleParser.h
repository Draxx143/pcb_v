#pragma once

#include <QString>
#include <QByteArray>
#include <QVector>
#include <QStringList>
#include <QPointF>

struct PcbElement {
    QString type; // "Track", "Pad"
    QPointF p1;
    QPointF p2;
};

class OleParser {
public:
    explicit OleParser(const QString &filePath);
    bool parse();
    
    QStringList getStreamsList() const;
    QString getSummaryInfo() const;
    QVector<PcbElement> getElements() const;

private:
    QString filePath;
    bool isLoaded;
    QStringList streams;
    QVector<PcbElement> elements;
    qint64 fileSize;
};
