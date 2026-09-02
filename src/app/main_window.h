#pragma once

#include <QMainWindow>

namespace loreforge::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace loreforge::app
