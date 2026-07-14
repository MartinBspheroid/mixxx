#include <gtest/gtest.h>

#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>

#include "companion/companionservice.h"
#include "preferences/dialog/dlgprefcompanion.h"
#include "test/mixxxtest.h"

namespace {

class CompanionPairingCodeTest : public MixxxTest {
  protected:
    // The service only stores these collaborators; the pairing code and the
    // Preferences page never touch them, so a UI-only test can pass nullptr.
    mixxx::companion::CompanionService makeService() {
        return mixxx::companion::CompanionService(config(),
                nullptr,
                nullptr,
                nullptr,
                QStringLiteral("2.6.0"));
    }
};

// The API ships disabled, so start() returns early and the service never runs.
// The code must exist anyway: this is the state a first-time user opens the
// Preferences page in, and a code they cannot read is a code they cannot pair.
TEST_F(CompanionPairingCodeTest, CodeExistsBeforeTheServiceEverRuns) {
    const auto service = makeService();
    ASSERT_FALSE(service.isRunning());
    EXPECT_TRUE(service.sessionCode().contains(
            QRegularExpression(QStringLiteral("^[0-9]{6}$"))))
            << "expected a 6-digit code, got: "
            << service.sessionCode().toStdString();
}

// restart() is stop()+start(), and Apply calls it whenever the port or the LAN
// toggle changes. Rotating the code there would silently lock out a phone that
// had already paired.
TEST_F(CompanionPairingCodeTest, CodeSurvivesStop) {
    auto service = makeService();
    const QString before = service.sessionCode();
    ASSERT_FALSE(before.isEmpty());
    service.stop();
    EXPECT_FALSE(service.sessionCode().isEmpty()) << "stop() wiped the code";
    EXPECT_EQ(service.sessionCode(), before);
}

// Regenerate is the one thing that may change it -- and it has to work while
// stopped, which is exactly when the old code left the button disabled.
TEST_F(CompanionPairingCodeTest, RegenerateWorksWhileStopped) {
    auto service = makeService();
    ASSERT_FALSE(service.isRunning());
    service.regenerateSessionCode();
    // Two random codes collide 1 time in a million, so assert the shape rather
    // than a difference from the previous one.
    EXPECT_TRUE(service.sessionCode().contains(
            QRegularExpression(QStringLiteral("^[0-9]{6}$"))))
            << "regenerate left the code unusable while stopped: "
            << service.sessionCode().toStdString();
}

// The page is what the user actually reads the code off, and it is the half of
// the fix the service tests cannot see: it used to blank the code out whenever
// the service was not running, which is exactly the state you set it up from.
TEST_F(CompanionPairingCodeTest, PreferencesPageShowsCodeWhileStopped) {
    auto service = makeService();
    DlgPrefCompanion page(nullptr, config(), &service);
    page.slotUpdate();
    ASSERT_FALSE(service.isRunning());

    auto* pCode = page.findChild<QLabel*>(QStringLiteral("labelCode"));
    ASSERT_NE(pCode, nullptr);
    EXPECT_EQ(pCode->text(), service.sessionCode());
    EXPECT_NE(pCode->text(), QStringLiteral("——————"));
    EXPECT_TRUE(pCode->isEnabled()) << "code is greyed out while stopped";

    auto* pRegenerate =
            page.findChild<QPushButton*>(QStringLiteral("pushButtonRegenerate"));
    ASSERT_NE(pRegenerate, nullptr);
    EXPECT_TRUE(pRegenerate->isEnabled()) << "cannot rotate a code while stopped";

    // Render it when asked, so the page can be eyeballed without a display.
    const QString outDir = qEnvironmentVariable("COMPANION_PREF_SHOT_DIR");
    if (!outDir.isEmpty()) {
        page.resize(560, 300);
        ASSERT_TRUE(page.grab().save(QDir(outDir).filePath(
                QStringLiteral("companion_prefs_stopped.png"))));
    }
}

} // namespace
