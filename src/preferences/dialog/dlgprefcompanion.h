#pragma once

#include "companion/companionservice.h"
#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgprefcompaniondlg.h"
#include "preferences/usersettings.h"

/// Preferences page for the Companion API: enable/port/LAN plus a live display
/// of the session pairing code (what the user types on their phone).
class DlgPrefCompanion : public DlgPreferencePage,
                         public Ui::DlgPrefCompanionDlg {
    Q_OBJECT
  public:
    DlgPrefCompanion(QWidget* pParent,
            UserSettingsPointer pConfig,
            mixxx::companion::CompanionService* pService);
    ~DlgPrefCompanion() override = default;

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  private slots:
    void slotRegenerate();
    void refreshLiveState();

  private:
    const UserSettingsPointer m_pConfig;
    mixxx::companion::CompanionService* m_pService;
};
