#pragma once

#include <QDebug>
#include <QElapsedTimer>

namespace loreforge::app {
// Opt-in diagnostics: fixed stage labels and durations only, never project contents.
class StartupProfile final {
  public:
    explicit StartupProfile(const char* stage) : stage_(stage) {
        if (qEnvironmentVariableIsSet("LOREFORGE_PROFILE_PROJECT"))
            timer_.start();
    }
    ~StartupProfile() {
        if (timer_.isValid())
            qInfo() << "Startup stage" << stage_ << "ms:" << timer_.elapsed();
    }

  private:
    const char* stage_;
    QElapsedTimer timer_;
};
} // namespace loreforge::app
