/*
    i2c-viapro.c - Part of lm_sensors, Linux kernel modules for hardware
              monitoring
    Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl>, 
    Philip Edelbrock <phil@netroedge.com>, Ky�sti M�lkki <kmalkki@cc.hut.fi>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* Note: we assume there can only be one VT596A/B, with one SMBus interface */

#include <linux/module.h>
#include <linux/pci.h>
#include <asm/io.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include "version.h"
#include "compat.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,1,54))
#include <linux/bios32.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,53)
#include <linux/init.h>
#else
#define __init
#define __initdata
#endif


#ifndef PCI_DEVICE_ID_VIA_82C596_3
#define PCI_DEVICE_ID_VIA_82C596_3 0x3050
#endif
#ifndef PCI_DEVICE_ID_VIA_82C686_4
#define PCI_DEVICE_ID_VIA_82C686_4 0x3057
#endif

/* SMBus address offsets */
#define SMBHSTSTS (0 + vt596_smba)
#define SMBHSLVSTS (1 + vt596_smba)
#define SMBHSTCNT (2 + vt596_smba)
#define SMBHSTCMD (3 + vt596_smba)
#define SMBHSTADD (4 + vt596_smba)
#define SMBHSTDAT0 (5 + vt596_smba)
#define SMBHSTDAT1 (6 + vt596_smba)
#define SMBBLKDAT (7 + vt596_smba)
#define SMBSLVCNT (8 + vt596_smba)
#define SMBSHDWCMD (9 + vt596_smba)
#define SMBSLVEVT (0xA + vt596_smba)
#define SMBSLVDAT (0xC + vt596_smba)

/* PCI Address Constants */

/* SMBus data in configuration space can be found in two places,
   We try to select the better one*/

static unsigned short smb_cf_base;
static unsigned short smb_cf_hstcfg;

#define SMBBA1	    0x90
#define SMBBA2      0x80
#define SMBBA       (smb_cf_base)
#define SMBHSTCFG   (smb_cf_hstcfg)
#define SMBSLVC     (SMBHSTCFG+1)
#define SMBSHDW1    (SMBHSTCFG+2)
#define SMBSHDW2    (SMBHSTCFG+3)
#define SMBREV      (SMBHSTCFG+4)

/* Other settings */
#define MAX_TIMEOUT 500
#define  ENABLE_INT9 0

/* VT82C596 constants */
#define VT596_QUICK      0x00
#define VT596_BYTE       0x04
#define VT596_BYTE_DATA  0x08
#define VT596_WORD_DATA  0x0C
#define VT596_BLOCK_DATA 0x14

/* insmod parameters */

/* If force is set to anything different from 0, we forcibly enable the
   VT596. DANGEROUS! */
static int force = 0;
MODULE_PARM(force,"i");
MODULE_PARM_DESC(force,"Forcibly enable the VT82C596 SMBus. DANGEROUS!");

/* If force_addr is set to anything different from 0, we forcibly enable
   the VT596 at the given address. VERY DANGEROUS! */
static int force_addr = 0;
MODULE_PARM(force_addr,"i");
MODULE_PARM_DESC(force_addr,"Forcibly enable the VT82C596 SMBus at the given address. "
                            "EXTREMELY DANGEROUS!");

#ifdef MODULE
static
#else
extern
#endif
       int __init i2c_vt596_init(void);
static int __init vt596_cleanup(void);
static int vt596_setup(void);
static s32 vt596_access(struct i2c_adapter *adap, u8 addr, char read_write,
                        u8 command, int size, union i2c_smbus_data * data);
static void vt596_do_pause( unsigned int amount );
static int vt596_transaction(void);
static void vt596_inc(struct i2c_adapter *adapter);
static void vt596_dec(struct i2c_adapter *adapter);
static u32 vt596_func(struct i2c_adapter *adapter);

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

static struct i2c_algorithm smbus_algorithm = {
  /* name */		"Non-I2C SMBus adapter",
  /* id */		I2C_ALGO_SMBUS,
  /* master_xfer */	NULL,
  /* smbus_access */    vt596_access,
  /* slave_send */	NULL,
  /* slave_rcv */	NULL,
  /* algo_control */	NULL,
  /* functionality */   vt596_func,
};

static struct i2c_adapter vt596_adapter = {
  "unset",
  I2C_ALGO_SMBUS | I2C_HW_SMBUS_VIA2,
  &smbus_algorithm,
  NULL,
  vt596_inc,
  vt596_dec,
  NULL,
  NULL,
};

static int __initdata vt596_initialized;
static unsigned short vt596_smba = 0;


/* Detect whether a VT596 can be found, and initialize it, where necessary.
   Note the differences between kernels with the old PCI BIOS interface and
   newer kernels with the real PCI interface. In compat.h some things are
   defined to make the transition easier. */
int vt596_setup(void)
{
  int error_return=0;
  unsigned char temp;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54))
  struct pci_dev *VT596_dev;
#else
  unsigned char VT596_bus, VT596_devfn;
  int i,res;
#endif

  /* First check whether we can access PCI at all */
  if (pci_present() == 0) {
    printk("i2c-viapro.o: Error: No PCI-bus found!\n");
    error_return=-ENODEV;
    goto END;
  }

  /* Look for the VT596 function 3   _or_  VT686 function 4 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54))
  VT596_dev = NULL;
  VT596_dev = pci_find_device(PCI_VENDOR_ID_VIA, 
                              PCI_DEVICE_ID_VIA_82C596_3, VT596_dev);
  if (VT596_dev == NULL)
    VT596_dev = pci_find_device(PCI_VENDOR_ID_VIA, 
                                PCI_DEVICE_ID_VIA_82C686_4, VT596_dev);

  if (VT596_dev == NULL) {
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(2,1,54) */
  for (i = 0; 
       ! (res = pcibios_find_device(PCI_VENDOR_ID_VIA,
                                    PCI_DEVICE_ID_VIA_82C596_3,
                                    i,&VT596_bus, &VT596_devfn)) && 
         PCI_FUNC(VT596_devfn) != 3; 
       i++);
  
  if (res)
    for (i = 0; 
       ! (res = pcibios_find_device(PCI_VENDOR_ID_VIA,
                                    PCI_DEVICE_ID_VIA_82C686_4,
                                    i,&VT596_bus, &VT596_devfn)) && 
         PCI_FUNC(VT596_devfn) != 4; 
       i++);

  if (res) {
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,1,54) */
    printk("i2c-viapro.o: Error: Can't detect vt82c596 or vt82c686");
    error_return=-ENODEV;
    goto END;
  } 

/* Determine the configuration space registers for the SMBus areas */
  if ((! pci_read_config_word_united(VT596_dev, VT596_bus ,VT596_devfn,
                       SMBBA1, &vt596_smba)) && (vt596_smba & 0x1)) {
    smb_cf_base = SMBBA1;
    smb_cf_hstcfg = 0xD2;
  } else if ((! pci_read_config_word_united(VT596_dev, VT596_bus ,VT596_devfn,
                       SMBBA2, &vt596_smba)) && (vt596_smba & 0x1)) {
    smb_cf_base = SMBBA2;
    smb_cf_hstcfg = 0x84;
  } else {
    printk("i2c-viapro.o: Cannot configure SMBus I/O Base address\n");
    error_return=-ENODEV;
    goto END;
  }

/* Determine the address of the SMBus areas */
  if (force_addr) {
    vt596_smba = force_addr & 0xfff0;
    force = 0;
  } else {
    vt596_smba &= 0xfff0;
  }

  if (check_region(vt596_smba, 8)) {
    printk("i2c-viapro.o: vt82c596_smb region 0x%x already in use!\n", vt596_smba);
    error_return=-ENODEV;
    goto END;
  }

  pci_read_config_byte_united(VT596_dev, VT596_bus, VT596_devfn,
                              SMBHSTCFG, &temp);
/* If force_addr is set, we program the new address here. Just to make
   sure, we disable the VT596 first. */
  if (force_addr) {
    pci_write_config_byte_united(VT596_dev, VT596_bus, VT596_devfn,
                                SMBHSTCFG, temp & 0xfe);
    pci_write_config_word_united(VT596_dev, VT596_bus ,VT596_devfn,
                                 SMBBA,vt596_smba);
    pci_write_config_byte_united(VT596_dev, VT596_bus, VT596_devfn,
                                SMBHSTCFG, temp | 0x01);
    printk("i2c-viapro.o: WARNING: VT596 SMBus interface set to new "
           "address %04x!\n",vt596_smba);
  } else if ((temp & 1) == 0) {
    if (force) {
/* NOTE: This assumes I/O space and other allocations WERE
   done by the Bios!  Don't complain if your hardware does weird 
   things after enabling this. :') Check for Bios updates before
   resorting to this.  */
      pci_write_config_byte_united(VT596_dev, VT596_bus, VT596_devfn,
                                       SMBHSTCFG, temp | 1);
      printk("i2c-viapro.o: WARNING: SMBus interface has been FORCEFULLY "
             "ENABLED!\n");
    } else {
      printk("SMBUS: Error: Host SMBus controller not enabled!\n");     
      error_return=-ENODEV;
      goto END;
    }
  }

  /* Everything is happy, let's grab the memory and set things up. */
  request_region(vt596_smba, 8, "via2-smbus");

#ifdef DEBUG
  if ((temp & 0x0E) == 8)
     printk("i2c-viapro.o: using Interrupt 9 for SMBus.\n");
  else if ((temp & 0x0E) == 0)
     printk("i2c-viapro.o: using Interrupt SMI# for SMBus.\n");
  else 
     printk("i2c-viapro.o: Illegal Interrupt configuration (or code out "
            "of date)!\n");

  pci_read_config_byte_united(VT596_dev, VT596_bus, VT596_devfn, SMBREV, 
                              &temp);
  printk("i2c-viapro.o: SMBREV = 0x%X\n",temp);
  printk("i2c-viapro.o: VT596_smba = 0x%X\n",vt596_smba);
#endif /* DEBUG */

END:
  return error_return;
}


/* Internally used pause function */
void vt596_do_pause( unsigned int amount )
{
      current->state = TASK_INTERRUPTIBLE;
      schedule_timeout(amount);
}

/* Another internally used function */
int vt596_transaction(void) 
{
  int temp;
  int result=0;
  int timeout=0;

#ifdef DEBUG
  printk("i2c-viapro.o: Transaction (pre): CNT=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, "
         "DAT1=%02x\n",
         inb_p(SMBHSTCNT),inb_p(SMBHSTCMD),inb_p(SMBHSTADD),inb_p(SMBHSTDAT0),
         inb_p(SMBHSTDAT1));
#endif

  /* Make sure the SMBus host is ready to start transmitting */
  if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
#ifdef DEBUG
    printk("i2c-viapro.o: SMBus busy (%02x). Resetting... \n",temp);
#endif
    outb_p(temp, SMBHSTSTS);
    if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
#ifdef DEBUG
      printk("i2c-viapro.o: Failed! (%02x)\n",temp);
#endif
      return -1;
    } else {
#ifdef DEBUG
      printk("i2c-viapro.o: Successfull!\n");
#endif
    }
  }

  /* start the transaction by setting bit 6 */
  outb_p(inb(SMBHSTCNT) | 0x040, SMBHSTCNT); 

  /* We will always wait for a fraction of a second! 
     I don't know if VIA needs this, Intel did  */
  do {
    vt596_do_pause(1);
    temp=inb_p(SMBHSTSTS);
  } while ((temp & 0x01) && (timeout++ < MAX_TIMEOUT));

  /* If the SMBus is still busy, we give up */
  if (timeout >= MAX_TIMEOUT) {
#ifdef DEBUG
    printk("i2c-viapro.o: SMBus Timeout!\n"); 
    result = -1;
#endif
  }

  if (temp & 0x10) {
    result = -1;
#ifdef DEBUG
    printk("i2c-viapro.o: Error: Failed bus transaction\n");
#endif
  }

  if (temp & 0x08) {
    result = -1;
    printk("i2c-viapro.o: Bus collision! SMBus may be locked until next hard
           reset. (sorry!)\n");
    /* Clock stops and slave is stuck in mid-transmission */
  }

  if (temp & 0x04) {
    result = -1;
#ifdef DEBUG
    printk("i2c-viapro.o: Error: no response!\n");
#endif
  }

  if (inb_p(SMBHSTSTS) != 0x00)
    outb_p( inb(SMBHSTSTS), SMBHSTSTS);

  if ((temp = inb_p(SMBHSTSTS)) != 0x00) {
#ifdef DEBUG
    printk("i2c-viapro.o: Failed reset at end of transaction (%02x)\n",temp);
#endif
  }
#ifdef DEBUG
  printk("i2c-viapro.o: Transaction (post): CNT=%02x, CMD=%02x, ADD=%02x, "
         "DAT0=%02x, DAT1=%02x\n",
         inb_p(SMBHSTCNT),inb_p(SMBHSTCMD),inb_p(SMBHSTADD),inb_p(SMBHSTDAT0),
         inb_p(SMBHSTDAT1));
#endif
  return result;
}

/* Return -1 on error. See smbus.h for more information */
s32 vt596_access(struct i2c_adapter *adap, u8 addr, char read_write,
                 u8 command, int size, union i2c_smbus_data * data)
{
  int i,len;

  switch(size) {
    case I2C_SMBUS_PROC_CALL:
      printk("i2c-viapro.o: I2C_SMBUS_PROC_CALL not supported!\n");
      return -1;
    case I2C_SMBUS_QUICK:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      size = VT596_QUICK;
      break;
    case I2C_SMBUS_BYTE:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      if (read_write == I2C_SMBUS_WRITE)
        outb_p(command, SMBHSTCMD);
      size = VT596_BYTE;
      break;
    case I2C_SMBUS_BYTE_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE)
        outb_p(data->byte,SMBHSTDAT0);
      size = VT596_BYTE_DATA;
      break;
    case I2C_SMBUS_WORD_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE) {
        outb_p(data->word & 0xff,SMBHSTDAT0);
        outb_p((data->word & 0xff00) >> 8,SMBHSTDAT1);
      }
      size = VT596_WORD_DATA;
      break;
    case I2C_SMBUS_BLOCK_DATA:
      outb_p(((addr & 0x7f) << 1) | (read_write & 0x01), SMBHSTADD);
      outb_p(command, SMBHSTCMD);
      if (read_write == I2C_SMBUS_WRITE) {
        len = data->block[0];
        if (len < 0) 
          len = 0;
        if (len > 32)
          len = 32;
        outb_p(len,SMBHSTDAT0);
        i = inb_p(SMBHSTCNT); /* Reset SMBBLKDAT */
        for (i = 1; i <= len; i ++)
          outb_p(data->block[i],SMBBLKDAT);
      }
      size = VT596_BLOCK_DATA;
      break;
  }

  outb_p((size & 0x1C) + (ENABLE_INT9 & 1), SMBHSTCNT);

  if (vt596_transaction()) /* Error in transaction */ 
    return -1; 
  
  if ((read_write == I2C_SMBUS_WRITE) || (size == VT596_QUICK))
    return 0;
  

  switch(size) {
    case VT596_BYTE: /* Where is the result put? I assume here it is in
                        SMBHSTDAT0 but it might just as well be in the
                        SMBHSTCMD. No clue in the docs */
 
      data->byte = inb_p(SMBHSTDAT0);
      break;
    case VT596_BYTE_DATA:
      data->byte = inb_p(SMBHSTDAT0);
      break;
    case VT596_WORD_DATA:
      data->word = inb_p(SMBHSTDAT0) + (inb_p(SMBHSTDAT1) << 8);
      break;
    case VT596_BLOCK_DATA:
      data->block[0] = inb_p(SMBHSTDAT0);
      i = inb_p(SMBHSTCNT); /* Reset SMBBLKDAT */
      for (i = 1; i <= data->block[0]; i++)
        data->block[i] = inb_p(SMBBLKDAT);
      break;
  }
  return 0;
}

void vt596_inc(struct i2c_adapter *adapter)
{
	MOD_INC_USE_COUNT;
}

void vt596_dec(struct i2c_adapter *adapter)
{

	MOD_DEC_USE_COUNT;
}

u32 vt596_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
               I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
               I2C_FUNC_SMBUS_BLOCK_DATA;
}

int __init i2c_vt596_init(void)
{
  int res;
  printk("via2.o version %s (%s)\n",LM_VERSION,LM_DATE);
#ifdef DEBUG
/* PE- It might be good to make this a permanent part of the code! */
  if (vt596_initialized) {
    printk("i2c-viapro.o: Oops, vt596_init called a second time!\n");
    return -EBUSY;
  }
#endif
  vt596_initialized = 0;
  if ((res = vt596_setup())) {
    printk("i2c-viapro.o: vt82c596 not detected, module not inserted.\n");
    vt596_cleanup();
    return res;
  }
  vt596_initialized ++;
  sprintf(vt596_adapter.name,"SMBus vt82c596 adapter at %04x",vt596_smba);
  if ((res = i2c_add_adapter(&vt596_adapter))) {
    printk("i2c-viapro.o: Adapter registration failed, module not inserted.\n");
    vt596_cleanup();
    return res;
  }
  vt596_initialized++;
  printk("i2c-viapro.o: vt82c596 bus detected and initialized\n");
  return 0;
}

int __init vt596_cleanup(void)
{
  int res;
  if (vt596_initialized >= 2)
  {
    if ((res = i2c_del_adapter(&vt596_adapter))) {
      printk("i2c-viapro.o: i2c_del_adapter failed, module not removed\n");
      return res;
    } else
      vt596_initialized--;
  }
  if (vt596_initialized >= 1) {
    release_region(vt596_smba, 8);
    vt596_initialized--;
  }
  return 0;
}

EXPORT_NO_SYMBOLS;

#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl> and Philip Edelbrock <phil@netroedge.com>");
MODULE_DESCRIPTION("vt82c596 SMBus driver");


int init_module(void)
{
  return i2c_vt596_init();
}

int cleanup_module(void)
{
  return vt596_cleanup();
}

#endif /* MODULE */

