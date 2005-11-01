/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_METASLAB_H
#define	_SYS_METASLAB_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/spa.h>
#include <sys/space_map.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/avl.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct metaslab_class metaslab_class_t;
typedef struct metaslab_group metaslab_group_t;

extern void metaslab_init(metaslab_group_t *mg, space_map_obj_t *smo,
    metaslab_t **mspp, uint64_t offset, uint64_t size, uint64_t txg);
extern void metaslab_fini(metaslab_t *msp);
extern void metaslab_sync(metaslab_t *msp, uint64_t txg);
extern void metaslab_sync_done(metaslab_t *msp, uint64_t txg);

extern int metaslab_alloc(spa_t *spa, uint64_t size, dva_t *dva, uint64_t txg);
extern void metaslab_free(spa_t *spa, dva_t *dva, uint64_t txg);
extern int metaslab_claim(spa_t *spa, dva_t *dva, uint64_t txg);

extern metaslab_class_t *metaslab_class_create(void);
extern void metaslab_class_destroy(metaslab_class_t *mc);
extern void metaslab_class_add(metaslab_class_t *mc, metaslab_group_t *mg);
extern void metaslab_class_remove(metaslab_class_t *mc, metaslab_group_t *mg);

extern metaslab_group_t *metaslab_group_create(metaslab_class_t *mc,
    vdev_t *vd);
extern void metaslab_group_destroy(metaslab_group_t *mg);
extern void metaslab_group_add(metaslab_group_t *mg, metaslab_t *msp,
    uint64_t weight);
extern void metaslab_group_remove(metaslab_group_t *mg, metaslab_t *msp);
extern void metaslab_group_sort(metaslab_group_t *mg, metaslab_t *msp,
    uint64_t weight);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_METASLAB_H */