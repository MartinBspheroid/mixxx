#pragma once

#include "companion/companionservice.h"
#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgprefcompaniondlg.h"
#include "preferences/usersettings.h"

/// Preferences page for the Companion API: enable/port/LAN plus a live display
/// of everything needed to connect a phone -- the address to open and the
/// session pairing code to type.
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
    /// Recompute the "open this on your phone" address from the current widget
    /// state (port + LAN toggle), independent of what is saved or running.
    void refreshAddress();

  private:
    const UserSettingsPointer m_pConfig;
    mixxx::companion::CompanionService* m_pService;
};
