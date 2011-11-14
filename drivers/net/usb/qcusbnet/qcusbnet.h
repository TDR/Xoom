/* qcusbnet.h - gobi network device header
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef QCUSBNET_QCUSBNET_H
#define QCUSBNET_QCUSBNET_H

#include "structs.h"

extern void qcusbnet_put(struct qcusbnet *dev);
extern struct qcusbnet *qcusbnet_get(struct qcusbnet *dev);
extern struct qcusbnet *cdev_to_qcusbnet(struct cdev *cdev);

#endif /* !QCUSBNET_QCUSBNET_H */
