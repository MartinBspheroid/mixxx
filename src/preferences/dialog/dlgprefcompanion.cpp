#include "preferences/dialog/dlgprefcompanion.h"

#include "companion/companionsettings.h"
#include "companion/networkinfo.h"
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
    // The address is derived from these two, so track them as they are edited
    // rather than only on Apply -- the user is reading it to decide what to set.
    connect(checkBoxAllowLan,
            &QCheckBox::toggled,
            this,
            &DlgPrefCompanion::refreshAddress);
    connect(spinBoxPort,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [this](int) { refreshAddress(); });
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
    const QString code = m_pService ? m_pService->sessionCode() : QString();

    // Always show the code: it belongs to the session, not to the running
    // server, so it stays readable while the user is still setting the API up.
    // The status label below is what says whether anything is listening yet.
    labelCode->setText(code.isEmpty() ? QStringLiteral("——————") : code);
    labelCode->setEnabled(!code.isEmpty());
    labelCodeCaption->setEnabled(!code.isEmpty());
    pushButtonRegenerate->setEnabled(!code.isEmpty());

    refreshAddress();

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

void DlgPrefCompanion::refreshAddress() {
    // Reflect the widgets, not the saved settings: the user is reading this to
    // find out what the choices they are making right now will give them.
    const int port = spinBoxPort->value();

    if (!checkBoxAllowLan->isChecked()) {
        // The server binds loopback in this mode, so no phone can reach it.
        // Saying "127.0.0.1" without that caveat would just waste the user's
        // time typing an address that cannot work.
        labelAddress->setText(
                QStringLiteral("127.0.0.1:%1").arg(port));
        labelAddressHint->setText(
                tr("Only apps on this computer can connect. Tick "
                   "\"Allow connections from other devices\" to reach Mixxx "
                   "from your phone."));
        return;
    }

    const QList<QHostAddress> addresses =
            mixxx::companion::reachableIPv4Addresses();
    if (addresses.isEmpty()) {
        labelAddress->setText(tr("no network address"));
        labelAddressHint->setText(
                tr("This computer has no network connection, so no phone can "
                   "reach it. Connect it to the same Wi-Fi as your phone."));
        return;
    }

    labelAddress->setText(
            QStringLiteral("%1:%2").arg(addresses.first().toString()).arg(port));

    if (addresses.size() > 1) {
        // Multi-homed (VPN, ethernet + Wi-Fi, docker bridges, ...): we cannot
        // know which network the phone is on, so name the alternatives instead
        // of guessing silently. Cap the list -- a machine running containers can
        // have a dozen, and a wall of them helps nobody.
        constexpr int kMaxAlternatives = 3;
        QStringList others;
        for (int i = 1; i < addresses.size() && others.size() < kMaxAlternatives;
                ++i) {
            others.append(QStringLiteral("%1:%2")
                                  .arg(addresses.at(i).toString())
                                  .arg(port));
        }
        const int remaining = addresses.size() - 1 - others.size();
        QString alternatives = others.join(QStringLiteral(", "));
        if (remaining > 0) {
            alternatives = tr("%1 (and %n more)", "", remaining)
                                   .arg(alternatives);
        }
        labelAddressHint->setText(
                tr("This computer has more than one network address. If the "
                   "first does not work, try: %1")
                        .arg(alternatives));
    } else {
        labelAddressHint->setText(
                tr("Your phone must be on the same Wi-Fi as this computer."));
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
