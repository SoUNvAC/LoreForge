#include "main_window.h"

#include <QLabel>
#include <QStatusBar>

namespace loreforge::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("LoreForge"));
    resize(960, 640);

    auto* welcome = new QLabel(tr("LoreForge project workspace"), this);
    welcome->setAlignment(Qt::AlignCenter);
    setCentralWidget(welcome);
    statusBar()->showMessage(tr("Ready"));
}

} // namespace loreforge::app
