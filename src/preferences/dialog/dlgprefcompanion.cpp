#include "preferences/dialog/dlgprefcompanion.h"

#include "companion/companionsettings.h"
#include "moc_dlgprefcompanion.cpp"

DlgPrefCompanion::DlgPrefCompanion(QWidget* pParent,
        UserSettingsPointer pConfig,
        mixxx::companion::CompanionService* pService)
        : DlgPreferencePage(pParent),
          m_pConfig(pConfig),
          m_pService(pService) {
    setupUi(this);

    connect(pushButtonRegenerate,
            &QPushButton::clicked,
            this,
            &DlgPrefCompanion::slotRegenerate);
    if (m_pService) {
        // Keep the code/status label live while the page is open.
        connect(m_pService,
                &mixxx::companion::CompanionService::stateChanged,
                this,
                &DlgPrefCompanion::refreshLiveState);
    }

    slotUpdate();
}

void DlgPrefCompanion::slotUpdate() {
    const mixxx::companion::CompanionSettings settings(m_pConfig);
    checkBoxEnabled->setChecked(settings.isEnabled());
    spinBoxPort->setValue(settings.port());
    checkBoxAllowLan->setChecked(settings.allowLan());
    refreshLiveState();
}

void DlgPrefCompanion::refreshLiveState() {
    const bool running = m_pService && m_pService->isRunning();
    const QString code = running ? m_pService->sessionCode() : QString();

    // The code is only meaningful for LAN clients on a running service.
    labelCode->setText(code.isEmpty() ? QStringLiteral("——————") : code);
    const bool codeUsable = running && !code.isEmpty();
    labelCode->setEnabled(codeUsable);
    labelCodeCaption->setEnabled(codeUsable);
    pushButtonRegenerate->setEnabled(codeUsable);

    if (!m_pService) {
        labelStatus->setText(tr("Status: unavailable"));
    } else if (running) {
        labelStatus->setText(tr("Status: running on port %1")
                                     .arg(spinBoxPort->value()));
    } else {
        labelStatus->setText(
                tr("Status: stopped (enable and click Apply to start)"));
    }
}

void DlgPrefCompanion::slotApply() {
    const mixxx::companion::CompanionSettings settings(m_pConfig);
    const bool wasEnabled = settings.isEnabled();
    const int wasPort = settings.port();
    const bool wasLan = settings.allowLan();

    const bool enabled = checkBoxEnabled->isChecked();
    const int port = spinBoxPort->value();
    const bool allowLan = checkBoxAllowLan->isChecked();

    m_pConfig->setValue(
            ConfigKey(QStringLiteral("[CompanionAPI]"),
                    QStringLiteral("enabled")),
            enabled);
    m_pConfig->setValue(
            ConfigKey(QStringLiteral("[CompanionAPI]"), QStringLiteral("port")),
            port);
    m_pConfig->setValue(
            ConfigKey(QStringLiteral("[CompanionAPI]"),
                    QStringLiteral("allow_lan")),
            allowLan);

    // Restart the service so the new config takes effect immediately.
    const bool changed =
            enabled != wasEnabled || port != wasPort || allowLan != wasLan;
    if (m_pService && changed) {
        m_pService->restart();
    }
    refreshLiveState();
}

void DlgPrefCompanion::slotResetToDefaults() {
    checkBoxEnabled->setChecked(false);
    spinBoxPort->setValue(mixxx::companion::kDefaultPort);
    checkBoxAllowLan->setChecked(false);
}

void DlgPrefCompanion::slotRegenerate() {
    if (m_pService) {
        m_pService->regenerateSessionCode();
        refreshLiveState();
    }
}
