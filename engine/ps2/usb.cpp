/* the PS2's files on a USB drive: a program a launcher starts from mass:
 * (wLaunchELF, from a FAT32 or exFAT drive) finds its folder there, as
 * PCSX2's host: gives it the folder of SONICADV.ELF, its settings, its disc
 * and its textures and sounds beside it. the launcher resets the IOP as it
 * starts the program, which drops its own USB drivers, so the program loads
 * ps2sdk's, as its own bytes (engine/mk/ps2.mk): iomanX, through which the
 * EE's file calls reach the drivers' devices, the USB driver, the block
 * devices' manager, the FAT and exFAT file system, and USB mass storage.
 * the drive takes a moment to mount after they load: the program waits for
 * its own file there, up to five seconds. a program started from anywhere
 * else loads none of them */
#include <stdio.h>
#include <string.h>

extern "C" {
#include <delaythread.h>
#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>

extern unsigned char iomanX_irx[], usbd_irx[], bdm_irx[], bdmfs_fatfs_irx[], usbmass_bd_irx[];
extern unsigned int size_iomanX_irx, size_usbd_irx, size_bdm_irx, size_bdmfs_fatfs_irx, size_usbmass_bd_irx;
}

/* the modules, in the order each needs the one before */
static int load(const char *name, unsigned char *irx, unsigned int size)
{
    int ret = 0;
    int id = SifExecModuleBuffer(irx, size, 0, nullptr, &ret);

    if (id < 0 || ret == 1) {
        printf("sa1: the IOP's %s didn't load: %d, %d\n", name, id, ret);
        return 0;
    }
    return 1;
}

/* where program, the path the launcher gave it, lies on a USB drive, the
 * drivers that reach it. it returns 0 where the drive didn't mount */
extern "C" int ps2_usb_open(const char *program)
{
    if (program == nullptr || strncmp(program, "mass", 4) != 0) {
        return 1;
    }
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    if (!load("iomanX", iomanX_irx, size_iomanX_irx) || !load("usbd", usbd_irx, size_usbd_irx) ||
        !load("bdm", bdm_irx, size_bdm_irx) || !load("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx) ||
        !load("usbmass_bd", usbmass_bd_irx, size_usbmass_bd_irx)) {
        return 0;
    }
    /* the drive mounts a moment after its driver loads */
    for (int tries = 0; tries < 50; tries++) {
        FILE *self = fopen(program, "rb");

        if (self != nullptr) {
            fclose(self);
            return 1;
        }
        DelayThread(100000);
    }
    printf("sa1: the USB drive holding %s didn't mount\n", program);
    return 0;
}
