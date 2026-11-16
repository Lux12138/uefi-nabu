##
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = nabu
  PLATFORM_GUID                  = C613F7A8-7FF8-42A8-92F7-FF34EA2B9866
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/nabuPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = nabuPkg/nabu.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1

  #
  # 0 = SM8150-AB
  # 1 = SM8150-AC / 855+
  # 2 = SM8150-AC / 860
  #
  SOC_TYPE                       = 2

!include HanaPkg/HanaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Pad 5"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"nabu"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"21051182G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"K82"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2560
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # Advanced Logger
  #
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"nabu"
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerBase|0x9FF00000
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerPages|0x80

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|nabuPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|nabuPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNU
  #
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  AdvancedLoggerAccessLib|AdvLoggerPkg/Library/AdvancedLoggerAccessLib/AdvancedLoggerAccessLib.inf
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf
  AdvancedLoggerHdwPortLib|AdvLoggerPkg/Library/AdvancedLoggerHdwPortLibNull/AdvancedLoggerHdwPortLibNull.inf
  AssertLib|AdvLoggerPkg/Library/AssertLib/AssertLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf

[PcdsFeatureFlag]
  gAdvLoggerPkgTokenSpaceGuid.PcdAdvancedLoggerFixedInRAM|TRUE

[LibraryClasses.common.SEC]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf

[LibraryClasses.common.DXE_CORE]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/DxeCore/AdvancedLoggerLib.inf

[LibraryClasses.common]
  AdvancedLoggerLib|AdvLoggerPkg/Library/AdvancedLoggerLib/Dxe/AdvancedLoggerLib.inf
  DebugLib|AdvLoggerPkg/Library/BaseDebugLibAdvancedLogger/BaseDebugLibAdvancedLogger.inf

[Components]
  #
  # ACPI Tables
  #
  nabu/AcpiTables.inf

  #
  # Payload filesystem
  #
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  #
  # UniXNU boot manager and XNU loader
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

  #
  # Firmware log exporter
  #
  AdvLoggerPkg/AdvancedSerialLogger/Dxe/AdvancedSerialLoggerDxe.inf
  UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf
  #
  # Nabu APSS watchdog plus the ESP boot letter. The profile comes from the UEFI
  # variable NabuWdt; NABU_WDT_DEFAULT is only the fallback when it is absent.
  #
  nabuPkg/Drivers/NabuWdtDxe/NabuWdtDxe.inf {
    <BuildOptions>
      *:*_*_*_CC_FLAGS = -DNABU_WDT_DEFAULT=0
  }
