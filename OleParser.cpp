#include "OleParser.h"
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

OleParser::OleParser(const QString &path) : filePath(path), isLoaded(false), fileSize(0) {}

bool OleParser::parse() {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // اگر متن‌باز نشد، به صورت باینری می‌خوانیم
        if (!file.open(QIODevice::ReadOnly)) return false;
    }

    QFileInfo info(file);
    fileSize = info.size();
    
    QTextStream in(&file);
    // پشتیبانی از انکودینگ‌های مختلف فایل‌های آلتیوم
    #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    in.setCodec("UTF-8");
    #endif

    int trackCount = 0;
    int padCount = 0;

    while (!in.atEnd()) {
        QString line = in.readLine();
        
        // استخراج خطوط (Tracks)
        if (line.contains("RECORD=Track", Qt::CaseInsensitive)) {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            QStringList parts = line.split('|');
            for (const QString &part : parts) {
                if (part.startsWith("X1=", Qt::CaseInsensitive)) x1 = part.mid(3).remove("mil").toDouble();
                else if (part.startsWith("Y1=", Qt::CaseInsensitive)) y1 = part.mid(3).remove("mil").toDouble();
                else if (part.startsWith("X2=", Qt::CaseInsensitive)) x2 = part.mid(3).remove("mil").toDouble();
                else if (part.startsWith("Y2=", Qt::CaseInsensitive)) y2 = part.mid(3).remove("mil").toDouble();
            }
            if (x1 != 0 || y1 != 0 || x2 != 0 || y2 != 0) {
                PcbElement el;
                el.type = "Track";
                el.p1 = QPointF(x1, y1);
                el.p2 = QPointF(x2, y2);
                elements.append(el);
                trackCount++;
            }
        }
        // استخراج پدها (Pads)
        else if (line.contains("RECORD=Pad", Qt::CaseInsensitive)) {
            double x = 0, y = 0;
            QStringList parts = line.split('|');
            for (const QString &part : parts) {
                if (part.startsWith("X=", Qt::CaseInsensitive) && !part.startsWith("XSIZE=")) x = part.mid(2).remove("mil").toDouble();
                else if (part.startsWith("Y=", Qt::CaseInsensitive) && !part.startsWith("YSIZE=")) y = part.mid(2).remove("mil").toDouble();
            }
            if (x != 0 || y != 0) {
                PcbElement el;
                el.type = "Pad";
                el.p1 = QPointF(x, y);
                el.p2 = QPointF(x, y);
                elements.append(el);
                padCount++;
            }
        }
    }

    file.close();

    streams.append(QString("Parsed Altium ASCII/Container format successfully."));
    streams.append(QString("Tracks found: %1").arg(trackCount));
    streams.append(QString("Pads found: %1").arg(padCount));

    isLoaded = true;
    return true;
}

QStringList OleParser::getStreamsList() const {
    return streams;
}

QString OleParser::getSummaryInfo() const {
    QString summary = QString("File Size: %1 bytes\nTotal Elements Rendered: %2\n\nLogs:\n").arg(fileSize).arg(elements.size());
    for (const QString &stream : streams) {
        summary += QString(" - %1\n").arg(stream);
    }
    return summary;
}

QVector<PcbElement> OleParser::getElements() const {
    return elements;
}
