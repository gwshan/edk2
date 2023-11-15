/** @file

  Copyright (c) 2014-2017, Linaro Limited. All rights reserved.
  Copyright (c) 2023, Arm Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Pi/PiMultiPhase.h>
#include <Library/ArmCcaLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>

// Number of Virtual Memory Map Descriptors
#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  7

//
// mach-virt's core peripherals such as the UART, the GIC and the RTC are
// all mapped in the 'miscellaneous device I/O' region, which we just map
// in its entirety rather than device by device. Note that it does not
// cover any of the NOR flash banks or PCI resource windows.
//
#define MACH_VIRT_PERIPH_BASE  0x08000000
#define MACH_VIRT_PERIPH_SIZE  SIZE_128MB
//
// The remaining is mapped lazily, but we need to register the memory
// attributes now if we're a Realm.
#define MACH_VIRT_LOWIO_SIZE   (SIZE_1GB - MACH_VIRT_PERIPH_BASE)

// The PCIe and extra redistributor regions are placed after DRAM. These
// definitions are only correct with less than 256GiB of RAM. Otherwise they are
// moved up during virt platform creation, aligned on their own size.
#define MACH_VIRT_GIC_REDIST2_BASE      SIZE_256GB
#define MACH_VIRT_GIC_REDIST2_SIZE      SIZE_64MB
#define MACH_VIRT_PCIE_ECAM_BASE        (SIZE_256GB + SIZE_256MB)
#define MACH_VIRT_PCIE_ECAM_SIZE        SIZE_256MB
#define MACH_VIRT_PCIE_MMIO_BASE        SIZE_512GB
#define MACH_VIRT_PCIE_MMIO_SIZE        SIZE_512GB

/**
  Default library constructor that obtains the memory size from a PCD.

  @return  Always returns RETURN_SUCCESS

**/
RETURN_STATUS
EFIAPI
QemuVirtMemInfoLibConstructor (
  VOID
  )
{
  UINT64  Size;
  VOID    *Hob;

  Size = PcdGet64 (PcdSystemMemorySize);
  Hob  = BuildGuidDataHob (&gArmVirtSystemMemorySizeGuid, &Size, sizeof Size);
  ASSERT (Hob != NULL);

  return RETURN_SUCCESS;
}

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU
  on your platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR
                                    describing a Physical-to-Virtual Memory
                                    mapping. This array must be ended by a
                                    zero-filled entry. The allocated memory
                                    will not be freed.

**/
VOID
ArmVirtGetMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;
  VOID                          *MemorySizeHob;
  EFI_STATUS                    Status;
  UINT64                        IpaWidth = 0;
  UINT64                        DevMapBit = 0;

  ASSERT (VirtualMemoryMap != NULL);

  MemorySizeHob = GetFirstGuidHob (&gArmVirtSystemMemorySizeGuid);
  ASSERT (MemorySizeHob != NULL);
  if (MemorySizeHob == NULL) {
    return;
  }

  VirtualMemoryTable = AllocatePool (
                         sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
                         MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS
                         );

  if (VirtualMemoryTable == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Error: Failed AllocatePool()\n", __func__));
    return;
  }

  // System DRAM
  VirtualMemoryTable[0].PhysicalBase = PcdGet64 (PcdSystemMemoryBase);
  VirtualMemoryTable[0].VirtualBase  = VirtualMemoryTable[0].PhysicalBase;
  VirtualMemoryTable[0].Length       = *(UINT64 *)GET_GUID_HOB_DATA (MemorySizeHob);
  VirtualMemoryTable[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  DEBUG ((
    DEBUG_INFO,
    "%a: Dumping System DRAM Memory Map:\n"
    "\tPhysicalBase: 0x%lX\n"
    "\tVirtualBase: 0x%lX\n"
    "\tLength: 0x%lX\n",
    __func__,
    VirtualMemoryTable[0].PhysicalBase,
    VirtualMemoryTable[0].VirtualBase,
    VirtualMemoryTable[0].Length
    ));

  if (IsRealm()) {
    Status = GetIpaWidth(&IpaWidth);
    if (Status == RETURN_SUCCESS) {
      DevMapBit = 1ULL << (IpaWidth - 1);
    } else {
      DEBUG ((DEBUG_ERROR, "could not get Realm IPA width\n"));
    }
  }

  // Memory mapped peripherals (UART, RTC, GIC, virtio-mmio, etc)
  VirtualMemoryTable[1].PhysicalBase = MACH_VIRT_PERIPH_BASE | DevMapBit;
  VirtualMemoryTable[1].VirtualBase  = MACH_VIRT_PERIPH_BASE;
  VirtualMemoryTable[1].Length       = MACH_VIRT_LOWIO_SIZE;
  VirtualMemoryTable[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Map the FV region as normal executable memory
  VirtualMemoryTable[2].PhysicalBase = PcdGet64 (PcdFvBaseAddress);
  VirtualMemoryTable[2].VirtualBase  = VirtualMemoryTable[2].PhysicalBase;
  VirtualMemoryTable[2].Length       = FixedPcdGet32 (PcdFvSize);
  VirtualMemoryTable[2].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK_RO;

  // High GIC redistributor region
  /*
   * TODO: These regions' base addresses depend on the amount of RAM, when the
   * VM has more than 256GiB of RAM. Although that may seem like a lot for a
   * VM, larger amounts are possible regardless of the size of host RAM, because
   * QEMU allows to create a large address space in order to enable memory
   * hotplug.
   */
  VirtualMemoryTable[3].PhysicalBase = MACH_VIRT_GIC_REDIST2_BASE | DevMapBit;
  VirtualMemoryTable[3].VirtualBase  = MACH_VIRT_GIC_REDIST2_BASE;
  VirtualMemoryTable[3].Length       = MACH_VIRT_GIC_REDIST2_SIZE;
  VirtualMemoryTable[3].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // High PCIe ECAM region
  VirtualMemoryTable[4].PhysicalBase = MACH_VIRT_PCIE_ECAM_BASE | DevMapBit;
  VirtualMemoryTable[4].VirtualBase  = MACH_VIRT_PCIE_ECAM_BASE;
  VirtualMemoryTable[4].Length       = MACH_VIRT_PCIE_ECAM_SIZE;
  VirtualMemoryTable[4].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // High PCIe MMIO region
  VirtualMemoryTable[5].PhysicalBase = MACH_VIRT_PCIE_MMIO_BASE | DevMapBit;
  VirtualMemoryTable[5].VirtualBase  = MACH_VIRT_PCIE_MMIO_BASE;
  VirtualMemoryTable[5].Length       = MACH_VIRT_PCIE_MMIO_SIZE;
  VirtualMemoryTable[5].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // End of Table
  ZeroMem (&VirtualMemoryTable[6], sizeof (ARM_MEMORY_REGION_DESCRIPTOR));

  *VirtualMemoryMap = VirtualMemoryTable;
}

/**
  Configure the MMIO regions as shared with the VMM.

  Set the protection attribute for the MMIO regions as Unprotected IPA.

  @param[in]    IpaWidth  IPA width of the Realm.

  @retval RETURN_SUCCESS            Success.
  @retval RETURN_INVALID_PARAMETER  A parameter is invalid.
  @retval RETURN_UNSUPPORTED        The execution context is not in a Realm.
**/
RETURN_STATUS
EFIAPI
ArmCcaConfigureMmio (
  IN UINT64  IpaWidth
  )
{
  if (!IsRealm ()) {
    return RETURN_UNSUPPORTED;
  }

  /*
   * ArmVirtGetMemoryMap() already returned all device mappings with the NS bit
   * set.
   */
  return RETURN_SUCCESS;
}
