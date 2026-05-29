#include "LapesEye/ui/MainWindow.h"
#include "LapesEye/ui/FolderPanel.h"
#include "LapesEye/ui/ThumbnailGrid.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Lape's Eye");
    app.setOrganizationName("Lape");
    app.setApplicationVersion("0.4.0");
    app.setDesktopFileName("lapes-eye");

    // Ikona aplikacji — wielorozdzielcza z QRC
    QIcon appIcon;
    appIcon.addFile(":/icons/lapes-eye-16.png",  QSize(16,16));
    appIcon.addFile(":/icons/lapes-eye-22.png",  QSize(22,22));
    appIcon.addFile(":/icons/lapes-eye-32.png",  QSize(32,32));
    appIcon.addFile(":/icons/lapes-eye-48.png",  QSize(48,48));
    appIcon.addFile(":/icons/lapes-eye-64.png",  QSize(64,64));
    appIcon.addFile(":/icons/lapes-eye-128.png", QSize(128,128));
    appIcon.addFile(":/icons/lapes-eye-256.png", QSize(256,256));
    app.setWindowIcon(appIcon);

    // Upewnij się że katalogi konfiguracyjne istnieją
    QString cfg = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QDir().mkpath(cfg + "/lape/bridge");
    QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);

    LapesEye::MainWindow win;

    // Globalnie wyeliminuj focus rect z QScrollArea — KDE Breeze rysuje niebieską
    // ramkę na QScrollArea::viewport() gdy widget ma StrongFocus. Dotyczy to
    // ThumbnailGrid::m_scroll. Ustawiamy to po stworzeniu okna (po załadowaniu stylu).
    app.setStyleSheet(app.styleSheet() +
        "QScrollArea { border: none; outline: none; }"
        "QScrollArea:focus { border: none; outline: none; }"
        "QScrollArea > QWidget > QWidget { outline: none; }"
        "QAbstractScrollArea { border: none; outline: none; }"
        "QAbstractScrollArea:focus { border: none; outline: none; }"
    );

    win.show();

    // Jeśli podano folder jako argument, otwórz go po pokazaniu okna
    const auto args = QApplication::arguments();
    if (args.size() > 1 && QDir(args[1]).exists()) {
        QString startDir = args[1];
        QTimer::singleShot(0, &win, [&win, startDir]() {
            win.open_folder(startDir);
        });
    }

    return app.exec();
}
