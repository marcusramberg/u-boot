/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _UFS_H
#define _UFS_H

struct udevice;

/**
 * ufs_probe() - initialize all devices in the UFS uclass
 *
 * Return: 0 if Ok, -ve on error
 */
int ufs_probe(void);

/**
 * ufs_probe_dev() - initialize a particular device in the UFS uclass
 *
 * @index: index in the uclass sequence
 *
 * Return: 0 if successfully probed, -ve on error
 */
int ufs_probe_dev(int index);

/**
 * ufs_stop_all() - quiesce every UFS host controller
 *
 * Halts the request lists, masks interrupts and puts the host controller into
 * reset, so nothing DMAs into U-Boot memory after control is handed over.
 */
void ufs_stop_all(void);

#endif
