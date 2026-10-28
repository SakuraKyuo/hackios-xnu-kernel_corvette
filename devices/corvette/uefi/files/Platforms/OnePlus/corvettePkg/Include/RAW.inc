  FILE FREEFORM = A1E235DE-E825-4591-9623-C43175811826 {
    SECTION UI = "SecParti.cfg"
    SECTION RAW = Binaries/giulia/RawFiles/SecParti.cfg
  }

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  # Panel used by corvette: AA567_P_3_A0004 (see oplus,dsi-panel-primary).
  FILE FREEFORM = E0ABCA0D-64AD-49DF-B2B5-8C6CDB7741B2 {
    SECTION UI = Panel_AA567_P_3_A0004_dsc_cmd.xml
    SECTION RAW = Binaries/giulia/RawFiles/Panel_AA567_P_3_A0004_dsc_cmd.xml
  }
!endif

  FILE FREEFORM = 21E9BDD9-6C3F-4F10-84A5-BBEC322741F1 {
    SECTION UI = "uefipil.cfg"
    SECTION RAW = Binaries/giulia/RawFiles/uefipil.cfg
  }

  FILE FREEFORM = 45FE4B7C-150C-45DA-A021-4BEB2048EC6F {
    SECTION UI = "QcomChargerCfg.cfg"
    SECTION RAW = Binaries/giulia/RawFiles/QcomChargerCfg.cfg
  }
