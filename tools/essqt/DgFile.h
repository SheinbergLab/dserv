#pragma once
#include <QString>
#include <QFile>
#include <QTemporaryFile>
#include <QDebug>

extern "C" {
#include <df.h>
#include <dynio.h>
#include <zlib.h>
}

class DGFile {
public:
    static DYN_GROUP* read_dgz(const QString& filename);
    
private:
    static QFile* uncompressFile(const QString& filename, QString& tempName);
    static void gzUncompress(gzFile in, QFile* out);
};

