#pragma once
#include <QPixmap>
#include "LapesEye/core/FileScanner.h"
#include "LapesEye/core/MetaStore.h"

namespace LapesEye {

struct ThumbnailCanvasItem {
    ScannedFile  file;
    QPixmap      thumb;
    FileMetadata meta;
    bool         meta_loaded = false;
};

} // namespace LapesEye
