
#include "DgFile.h"
#include <QFileInfo>

DYN_GROUP* DGFile::read_dgz(const QString& filename) {
    if (filename.isEmpty()) {
        return nullptr;
    }
    
    DYN_GROUP* dg = dfuCreateDynGroup(4);
    if (!dg) {
        return nullptr;
    }
    
    QFileInfo fileInfo(filename);
    QString suffix = fileInfo.suffix().toLower();
    QString tempName;
    QFile* fp = nullptr;
    
    // Handle different file types
    if (suffix == "dg") {
        // Plain .dg file - no decompression needed
        fp = new QFile(filename);
        if (!fp->open(QIODevice::ReadOnly)) {
            dfuFreeDynGroup(dg);
            delete fp;
            return nullptr;
        }
    } else if (suffix == "lz4") {
        // LZ4 compressed file
        QByteArray filenameBytes = filename.toLocal8Bit();
        if (dgReadDynGroup((char *) filenameBytes.constData(), dg) == DF_OK) {
            return dg;  // Successfully read LZ4 file
        } else {
            dfuFreeDynGroup(dg);
            return nullptr;
        }
    } else {
        // Try to decompress (assuming .dgz or other compressed format)
        fp = uncompressFile(filename, tempName);
        if (!fp) {
            // Try with different extensions
            QStringList extensions = {".dg", ".dgz"};
            for (const QString& ext : extensions) {
                QString tryFilename = filename + ext;
                if (QFile::exists(tryFilename)) {
                    fp = uncompressFile(tryFilename, tempName);
                    if (fp) break;
                }
            }
            
            if (!fp) {
                dfuFreeDynGroup(dg);
                return nullptr;
            }
        }
    }
    
    // Convert QFile to FILE* for dguFileToStruct
    FILE* cFile = nullptr;
    if (fp) {
        // Get the file descriptor and create a FILE*
        // Note: This is platform-specific and might need adjustment
#ifdef _WIN32
        int fd = fp->handle();
        if (fd != -1) {
            cFile = _fdopen(fd, "rb");
        }
#else
        int fd = fp->handle();
        if (fd != -1) {
            cFile = fdopen(dup(fd), "rb");  // dup to avoid double-close
        }
#endif
    }
    
    if (!cFile) {
        dfuFreeDynGroup(dg);
        delete fp;
        if (!tempName.isEmpty()) {
            QFile::remove(tempName);
        }
        return nullptr;
    }
    
    // Read the dynamic group structure
    if (!dguFileToStruct(cFile, dg)) {
        fclose(cFile);
        delete fp;
        if (!tempName.isEmpty()) {
            QFile::remove(tempName);
        }
        dfuFreeDynGroup(dg);
        return nullptr;
    }
    
    fclose(cFile);
    delete fp;
    if (!tempName.isEmpty()) {
        QFile::remove(tempName);
    }
    
    return dg;
}

QFile* DGFile::uncompressFile(const QString& filename, QString& tempName) {
    gzFile in = gzopen(filename.toLocal8Bit().constData(), "rb");
    if (!in) {
        qDebug() << "Failed to open compressed file:" << filename;
        return nullptr;
    }
    
    // Create temporary file
    QTemporaryFile* tempFile = new QTemporaryFile();
    if (!tempFile->open()) {
        gzclose(in);
        delete tempFile;
        return nullptr;
    }
    
    tempName = tempFile->fileName();
    
    // Decompress
    gzUncompress(in, tempFile);
    gzclose(in);
    
    // Close and reopen for reading
    tempFile->close();
    
    QFile* readFile = new QFile(tempName);
    if (!readFile->open(QIODevice::ReadOnly)) {
        delete tempFile;
        delete readFile;
        QFile::remove(tempName);
        return nullptr;
    }
    
    delete tempFile;  // QTemporaryFile object no longer needed
    return readFile;
}

void DGFile::gzUncompress(gzFile in, QFile* out) {
    const int bufSize = 2048;
    char buf[bufSize];
    int len;
    
    while ((len = gzread(in, buf, bufSize)) > 0) {
        if (out->write(buf, len) != len) {
            qDebug() << "Write error during decompression";
            return;
        }
    }
    
    if (len < 0) {
        qDebug() << "Read error during decompression";
    }
}
