##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = corvette
  PLATFORM_GUID                  = A8854916-202A-456E-A74B-FF22ECB1FCF9
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/corvettePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = corvettePkg/corvette.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8650-AA
  # 1 = SM8650-Q-AA
  # 2 = SM8650-AB
  # 3 = SM8650-Q-AB
  # 4 = SM8650-AC
  #
  SOC_TYPE                       = 2

!include LanaiPkg/LanaiPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"OnePlus"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"corvette"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"corvette"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"corvette"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"corvette"

  #
  # Simple Frame Buffer
  #
  # Active size of the AA567_P_3_A0004 panel (HorizontalActive 1264,
  # VerticalActive 2780). The frame buffer itself is the "Display Reserved"
  # region of MemoryMapLib, 0xD5100000 -- the same address as the loader's
  # platform_corvette.fb_base; confirm it from the first boot log
  # ("display 1264 x 2780 at ...").
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1264
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2780
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|30000

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|corvettePkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|corvettePkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf
