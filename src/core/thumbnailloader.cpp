#include "thumbnailloader.h"
#include <string>
#include <memory>
#include <algorithm>
#include <libexif/exif-loader.h>
#include <QImageReader>

namespace Fm2 {

QThreadPool* ThumbnailLoader::threadPool_ = nullptr;

ThumbnailLoader::ThumbnailLoader(std::shared_ptr<const FileInfo> file, int size, std::function<void (ThumbnailLoader&)> callback):
    file_{file},
    size_{size},
    callback_{callback} {
}

void ThumbnailLoader::run() {
    if(!isCancelled()) {
        // thumbnails are stored in $XDG_CACHE_HOME/thumbnails/large|normal|failed
        std::string thumbnailDir{g_get_user_cache_dir()};
        thumbnailDir += "/thumbnails/";

        // generate base name of the thumbnail  => {md5 of uri}.png
        auto origPath = file_->path();
        auto uri = origPath.uri();
        char thumbnailName[32 + 5];
        // calculate md5 hash for the uri of the original file
        GChecksum* sum = g_checksum_new(G_CHECKSUM_MD5);
        g_checksum_update(sum, reinterpret_cast<const unsigned char*>(uri.get()), -1);
        memcpy(thumbnailName, g_checksum_get_string(sum), 32);
        mempcpy(thumbnailName + 32, ".png", 5);

        QString thumbnailFilename = QString::fromStdString(thumbnailDir);
        thumbnailFilename += '/';

        const char* subdir = size_ > 128 ? "large" : "normal";
        thumbnailDir += subdir;

        // try to load the thumbnail file if it exists
        QImage thumbnail{thumbnailFilename};
        if(thumbnail.isNull() || isThumbnailOutdated(thumbnail)) {
            // the existing thumbnail cannot be loaded, generate a new one
            thumbnail = generateThumbnail(origPath, uri.get(), thumbnailFilename);
        }
        // resize to the size we need
        if(thumbnail.width() > size_ || thumbnail.height() > size_) {
            thumbnail = thumbnail.scaled(size_, size_, Qt::KeepAspectRatio);
        }
        result_ = thumbnail;
    }
    if(callback_) {
        callback_(*this);
    }
}

QImage ThumbnailLoader::readImageFromStream(GInputStream* stream, size_t len) {
    // FIXME: should we set a limit here? Otherwise if len is too large, we can run out of memory.
    std::unique_ptr<unsigned char[]> buffer{new unsigned char[len]}; // allocate enough buffer
    unsigned char* pbuffer = buffer.get();
    size_t totalReadSize = 0;
    while(!isCancelled() && totalReadSize < len) {
        size_t bytesToRead = totalReadSize + 4096 > len ? len - totalReadSize : 4096;
        gssize readSize = g_input_stream_read(stream, pbuffer, bytesToRead, cancellable_.get(), NULL);
        if(readSize == 0) { // end of file
            break;
        }
        else if(readSize == -1) { // error
            return QImage();
        }
        totalReadSize += readSize;
        pbuffer += readSize;
    }
    QImage image;
    image.loadFromData(buffer.get(), totalReadSize);
    return image;
}

bool ThumbnailLoader::isSupportedImageType() const {
    auto mime_type = file_->mimeType();
    if(mime_type->isImage()) {
        auto supportedTypes = QImageReader::supportedMimeTypes();
        auto found = std::find(supportedTypes.cbegin(), supportedTypes.cend(), mime_type->name());
        if(found != supportedTypes.cend())
            return true;
    }
    return false;
}

bool ThumbnailLoader::isThumbnailOutdated(const QImage &thumbnail) const {
    QString thumb_mtime = thumbnail.text("tEXt::Thumb::MTime");
    return (thumb_mtime.isEmpty()&& thumb_mtime.toInt() != file_->getMtime());
}

QImage ThumbnailLoader::generateThumbnail(const FilePath& origPath, const char* uri, const QString& thumbnailFilename) {
    QImage result;
    auto mime_type = file_->mimeType();
    if(isSupportedImageType()) {
        GFileInputStreamPtr ins{g_file_read(origPath.gfile().get(), cancellable_.get(), NULL), false};
        if(!ins)
            return QImage();
        int rotate_degrees = 0;
        if(strcmp(mime_type->name(), "image/jpeg") == 0) { // if this is a jpeg file
            /* try to extract thumbnails embedded in jpeg files */
            ExifLoader* exif_loader = exif_loader_new();
            while(!isCancelled()) {
                unsigned char buf[4096];
                gssize read_size = g_input_stream_read(G_INPUT_STREAM(ins.get()), buf, 4096, cancellable_.get(), NULL);
                if(read_size <= 0) { // EOF or error
                    break;
                }
                if(exif_loader_write(exif_loader, buf, read_size) == 0) {
                    break;    // no more EXIF data
                }
            }
            ExifData* exif_data = exif_loader_get_data(exif_loader);
            exif_loader_unref(exif_loader);
            if(exif_data) {
                /* reference for EXIF orientation tag:
                 * http://www.impulseadventure.com/photo/exif-orientation.html */
                ExifEntry* orient_ent = exif_data_get_entry(exif_data, EXIF_TAG_ORIENTATION);
                if(orient_ent) { /* orientation flag found in EXIF */
                    gushort orient;
                    ExifByteOrder bo = exif_data_get_byte_order(exif_data);
                    /* bo == EXIF_BYTE_ORDER_INTEL ; */
                    orient = exif_get_short(orient_ent->data, bo);
                    switch(orient) {
                    case 1: /* no rotation */
                        rotate_degrees = 0;
                        break;
                    case 8:
                        rotate_degrees = 90;
                        break;
                    case 3:
                        rotate_degrees = 180;
                        break;
                    case 6:
                        rotate_degrees = 270;
                        break;
                    }
                }
                if(exif_data->data) { // if an embedded thumbnail is available, load it
                    result.loadFromData(exif_data->data, exif_data->size);
                }
                exif_data_unref(exif_data);
            }
        }
        if(result.isNull()) {  // not able to generate a thumbnail from the EXIF data
            // load the original file
            g_seekable_seek(G_SEEKABLE(ins.get()), 0, G_SEEK_SET, cancellable_.get(), nullptr);
            result = readImageFromStream(G_INPUT_STREAM(ins.get()), file_->getSize());
        }
        g_input_stream_close(G_INPUT_STREAM(ins.get()), nullptr, nullptr);

        if(!result.isNull()) { // the image is successfully loaded
            // scale the image as needed
            int target_size = size_ > 128 ? 256 : 128;

            // only scale the original image if it's too large
            if(result.width() > target_size || result.height() > target_size) {
                result = result.scaled(target_size, target_size, Qt::KeepAspectRatio);
            }

            // save the generated thumbnail to disk
            // FIXME: create the thumbnail dir as needd
            result.setText("tEXt::Thumb::MTime", QString::number(file_->getMtime()));
            result.setText("tEXt::Thumb::URI", uri);
            result.save(thumbnailFilename, "PNG");

            if(rotate_degrees != 0) {
                // degree values are 0, 90, 180, and 270 counterclockwise.
                // In Qt, QMatrix does rotation counterclockwise as well.
                // However, because the y axis of widget coordinate system is downward,
                // the real effect of the coordinate transformation becomes clockwise rotation.
                // So we need to use (360 - degree) here.
                // Quote from QMatrix API doc:
                // Note that if you apply a QMatrix to a point defined in widget
                // coordinates, the direction of the rotation will be clockwise because
                // the y-axis points downwards.
                result = result.transformed(QMatrix().rotate(360 - rotate_degrees));
            }
        }
    }
    else { // the image format is not supported, try to find an external thumbnailer
        // TODO: find an external thumbnailer for it
        // Calling an external thumbnailer is expensive, so we might check the "failed" dir first
    }
    return result;
}

QThreadPool* ThumbnailLoader::threadPool() {
    if(Q_UNLIKELY(threadPool_)) {
        threadPool_ = new QThreadPool();
        threadPool_->setMaxThreadCount(1);
    }
    return threadPool_;
}


} // namespace Fm2
