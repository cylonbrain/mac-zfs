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

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/zfs_context.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/dmu.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_dataset.h>
#include <sys/spa.h>
#include <sys/zio.h>
#include <sys/dmu_zfetch.h>

static int free_range_compar(const void *node1, const void *node2);

static kmem_cache_t *dnode_cache;

static dnode_phys_t dnode_phys_zero;

int zfs_default_bs = SPA_MINBLOCKSHIFT;
int zfs_default_ibs = DN_MAX_INDBLKSHIFT;

/* ARGSUSED */
static int
dnode_cons(void *arg, void *unused, int kmflag)
{
	int i;
	dnode_t *dn = arg;
	bzero(dn, sizeof (dnode_t));

	rw_init(&dn->dn_struct_rwlock, NULL, RW_DEFAULT, NULL);
	mutex_init(&dn->dn_mtx, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&dn->dn_dbufs_mtx, NULL, MUTEX_DEFAULT, NULL);
	refcount_create(&dn->dn_holds);
	refcount_create(&dn->dn_tx_holds);

	for (i = 0; i < TXG_SIZE; i++) {
		avl_create(&dn->dn_ranges[i], free_range_compar,
		    sizeof (free_range_t),
		    offsetof(struct free_range, fr_node));
		list_create(&dn->dn_dirty_dbufs[i],
		    sizeof (dmu_buf_impl_t),
		    offsetof(dmu_buf_impl_t, db_dirty_node[i]));
	}

	list_create(&dn->dn_dbufs, sizeof (dmu_buf_impl_t),
	    offsetof(dmu_buf_impl_t, db_link));

	return (0);
}

/* ARGSUSED */
static void
dnode_dest(void *arg, void *unused)
{
	int i;
	dnode_t *dn = arg;

	rw_destroy(&dn->dn_struct_rwlock);
	mutex_destroy(&dn->dn_mtx);
	mutex_destroy(&dn->dn_dbufs_mtx);
	refcount_destroy(&dn->dn_holds);
	refcount_destroy(&dn->dn_tx_holds);

	for (i = 0; i < TXG_SIZE; i++) {
		avl_destroy(&dn->dn_ranges[i]);
		list_destroy(&dn->dn_dirty_dbufs[i]);
	}

	list_destroy(&dn->dn_dbufs);
}

void
dnode_init(void)
{
	dnode_cache = kmem_cache_create("dnode_t",
	    sizeof (dnode_t),
	    0, dnode_cons, dnode_dest, NULL, NULL, NULL, 0);
}

void
dnode_fini(void)
{
	kmem_cache_destroy(dnode_cache);
}


void
dnode_verify(dnode_t *dn)
{
#ifdef ZFS_DEBUG
	int drop_struct_lock = FALSE;

	ASSERT(dn->dn_phys);
	ASSERT(dn->dn_objset);

	ASSERT(dn->dn_phys->dn_type < DMU_OT_NUMTYPES);

	if (!(zfs_flags & ZFS_DEBUG_DNODE_VERIFY))
		return;

	if (!RW_WRITE_HELD(&dn->dn_struct_rwlock)) {
		rw_enter(&dn->dn_struct_rwlock, RW_READER);
		drop_struct_lock = TRUE;
	}
	if (dn->dn_phys->dn_type != DMU_OT_NONE || dn->dn_allocated_txg != 0) {
		int i;
		ASSERT3U(dn->dn_indblkshift, >=, 0);
		ASSERT3U(dn->dn_indblkshift, <=, SPA_MAXBLOCKSHIFT);
		if (dn->dn_datablkshift) {
			ASSERT3U(dn->dn_datablkshift, >=, SPA_MINBLOCKSHIFT);
			ASSERT3U(dn->dn_datablkshift, <=, SPA_MAXBLOCKSHIFT);
			ASSERT3U(1<<dn->dn_datablkshift, ==, dn->dn_datablksz);
		}
		ASSERT3U(dn->dn_nlevels, <=, 30);
		ASSERT3U(dn->dn_type, <=, DMU_OT_NUMTYPES);
		ASSERT3U(dn->dn_nblkptr, >=, 1);
		ASSERT3U(dn->dn_nblkptr, <=, DN_MAX_NBLKPTR);
		ASSERT3U(dn->dn_bonuslen, <=, DN_MAX_BONUSLEN);
		ASSERT3U(dn->dn_datablksz, ==,
		    dn->dn_datablkszsec << SPA_MINBLOCKSHIFT);
		ASSERT3U(ISP2(dn->dn_datablksz), ==, dn->dn_datablkshift != 0);
		ASSERT3U((dn->dn_nblkptr - 1) * sizeof (blkptr_t) +
		    dn->dn_bonuslen, <=, DN_MAX_BONUSLEN);
		for (i = 0; i < TXG_SIZE; i++) {
			ASSERT3U(dn->dn_next_nlevels[i], <=, dn->dn_nlevels);
		}
	}
	if (dn->dn_phys->dn_type != DMU_OT_NONE)
		ASSERT3U(dn->dn_phys->dn_nlevels, <=, dn->dn_nlevels);
	ASSERT(IS_DNODE_DNODE(dn->dn_object) || dn->dn_dbuf);
	if (dn->dn_dbuf != NULL) {
		ASSERT3P(dn->dn_phys, ==,
		    (dnode_phys_t *)dn->dn_dbuf->db.db_data +
		    (dn->dn_object % (dn->dn_dbuf->db.db_size >> DNODE_SHIFT)));
	}
	if (drop_struct_lock)
		rw_exit(&dn->dn_struct_rwlock);
#endif
}

void
dnode_byteswap(dnode_phys_t *dnp)
{
	uint64_t *buf64 = (void*)&dnp->dn_blkptr;
	int i;

	if (dnp->dn_type == DMU_OT_NONE) {
		bzero(dnp, sizeof (dnode_phys_t));
		return;
	}

	dnp->dn_type = BSWAP_8(dnp->dn_type);
	dnp->dn_indblkshift = BSWAP_8(dnp->dn_indblkshift);
	dnp->dn_nlevels = BSWAP_8(dnp->dn_nlevels);
	dnp->dn_nblkptr = BSWAP_8(dnp->dn_nblkptr);
	dnp->dn_bonustype = BSWAP_8(dnp->dn_bonustype);
	dnp->dn_checksum = BSWAP_8(dnp->dn_checksum);
	dnp->dn_compress = BSWAP_8(dnp->dn_compress);
	dnp->dn_datablkszsec = BSWAP_16(dnp->dn_datablkszsec);
	dnp->dn_bonuslen = BSWAP_16(dnp->dn_bonuslen);
	dnp->dn_maxblkid = BSWAP_64(dnp->dn_maxblkid);
	dnp->dn_secphys = BSWAP_64(dnp->dn_secphys);

	/*
	 * dn_nblkptr is only one byte, so it's OK to read it in either
	 * byte order.  We can't read dn_bouslen.
	 */
	ASSERT(dnp->dn_indblkshift <= SPA_MAXBLOCKSHIFT);
	ASSERT(dnp->dn_nblkptr <= DN_MAX_NBLKPTR);
	for (i = 0; i < dnp->dn_nblkptr * sizeof (blkptr_t)/8; i++)
		buf64[i] = BSWAP_64(buf64[i]);

	/*
	 * OK to check dn_bonuslen for zero, because it won't matter if
	 * we have the wrong byte order.  This is necessary because the
	 * dnode dnode is smaller than a regular dnode.
	 */
	if (dnp->dn_bonuslen != 0) {
		/*
		 * Note that the bonus length calculated here may be
		 * longer than the actual bonus buffer.  This is because
		 * we always put the bonus buffer after the last block
		 * pointer (instead of packing it against the end of the
		 * dnode buffer).
		 */
		int off = (dnp->dn_nblkptr-1) * sizeof (blkptr_t);
		size_t len = DN_MAX_BONUSLEN - off;
		dmu_ot[dnp->dn_bonustype].ot_byteswap(dnp->dn_bonus + off, len);
	}
}

void
dnode_buf_byteswap(void *vbuf, size_t size)
{
	dnode_phys_t *buf = vbuf;
	int i;

	ASSERT3U(sizeof (dnode_phys_t), ==, (1<<DNODE_SHIFT));
	ASSERT((size & (sizeof (dnode_phys_t)-1)) == 0);

	size >>= DNODE_SHIFT;
	for (i = 0; i < size; i++) {
		dnode_byteswap(buf);
		buf++;
	}
}

static int
free_range_compar(const void *node1, const void *node2)
{
	const free_range_t *rp1 = node1;
	const free_range_t *rp2 = node2;

	if (rp1->fr_blkid < rp2->fr_blkid)
		return (-1);
	else if (rp1->fr_blkid > rp2->fr_blkid)
		return (1);
	else return (0);
}

static void
dnode_setdblksz(dnode_t *dn, int size)
{
	ASSERT3U(P2PHASE(size, SPA_MINBLOCKSIZE), ==, 0);
	ASSERT3U(size, <=, SPA_MAXBLOCKSIZE);
	ASSERT3U(size, >=, SPA_MINBLOCKSIZE);
	ASSERT3U(size >> SPA_MINBLOCKSHIFT, <,
	    1<<(sizeof (dn->dn_phys->dn_datablkszsec) * 8));
	dn->dn_datablksz = size;
	dn->dn_datablkszsec = size >> SPA_MINBLOCKSHIFT;
	dn->dn_datablkshift = ISP2(size) ? highbit(size - 1) : 0;
}

static dnode_t *
dnode_create(objset_impl_t *os, dnode_phys_t *dnp, dmu_buf_impl_t *db,
    uint64_t object)
{
	dnode_t *dn = kmem_cache_alloc(dnode_cache, KM_SLEEP);
	(void) dnode_cons(dn, NULL, 0); /* XXX */

	dn->dn_objset = os;
	dn->dn_object = object;
	dn->dn_dbuf = db;
	dn->dn_phys = dnp;

	if (dnp->dn_datablkszsec)
		dnode_setdblksz(dn, dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT);
	dn->dn_indblkshift = dnp->dn_indblkshift;
	dn->dn_nlevels = dnp->dn_nlevels;
	dn->dn_type = dnp->dn_type;
	dn->dn_nblkptr = dnp->dn_nblkptr;
	dn->dn_checksum = dnp->dn_checksum;
	dn->dn_compress = dnp->dn_compress;
	dn->dn_bonustype = dnp->dn_bonustype;
	dn->dn_bonuslen = dnp->dn_bonuslen;
	dn->dn_maxblkid = dnp->dn_maxblkid;

	dmu_zfetch_init(&dn->dn_zfetch, dn);

	ASSERT(dn->dn_phys->dn_type < DMU_OT_NUMTYPES);
	mutex_enter(&os->os_lock);
	list_insert_head(&os->os_dnodes, dn);
	mutex_exit(&os->os_lock);

	return (dn);
}

static void
dnode_destroy(dnode_t *dn)
{
	objset_impl_t *os = dn->dn_objset;

	mutex_enter(&os->os_lock);
	list_remove(&os->os_dnodes, dn);
	mutex_exit(&os->os_lock);

	if (dn->dn_dirtyctx_firstset) {
		kmem_free(dn->dn_dirtyctx_firstset, 1);
		dn->dn_dirtyctx_firstset = NULL;
	}
	dmu_zfetch_rele(&dn->dn_zfetch);
	kmem_cache_free(dnode_cache, dn);
}

void
dnode_allocate(dnode_t *dn, dmu_object_type_t ot, int blocksize, int ibs,
	dmu_object_type_t bonustype, int bonuslen, dmu_tx_t *tx)
{
	int i;

	if (blocksize == 0)
		blocksize = 1 << zfs_default_bs;

	blocksize = MIN(MAX(blocksize, SPA_MINBLOCKSIZE), SPA_MAXBLOCKSIZE);

	if (ibs == 0)
		ibs = zfs_default_ibs;

	ibs = MIN(MAX(ibs, DN_MIN_INDBLKSHIFT), DN_MAX_INDBLKSHIFT);

	dprintf("os=%p obj=%llu txg=%llu blocksize=%d ibs=%d\n", dn->dn_objset,
	    dn->dn_object, tx->tx_txg, blocksize, ibs);

	ASSERT(dn->dn_type == DMU_OT_NONE);
	ASSERT(bcmp(dn->dn_phys, &dnode_phys_zero, sizeof (dnode_phys_t)) == 0);
	ASSERT(dn->dn_phys->dn_type == DMU_OT_NONE);
	ASSERT(ot != DMU_OT_NONE);
	ASSERT3U(ot, <, DMU_OT_NUMTYPES);
	ASSERT((bonustype == DMU_OT_NONE && bonuslen == 0) ||
	    (bonustype != DMU_OT_NONE && bonuslen != 0));
	ASSERT3U(bonustype, <, DMU_OT_NUMTYPES);
	ASSERT3U(bonuslen, <=, DN_MAX_BONUSLEN);
	ASSERT(dn->dn_type == DMU_OT_NONE);
	ASSERT3U(dn->dn_maxblkid, ==, 0);
	ASSERT3U(dn->dn_allocated_txg, ==, 0);
	ASSERT3U(dn->dn_assigned_txg, ==, 0);
	ASSERT(refcount_is_zero(&dn->dn_tx_holds));
	ASSERT3U(refcount_count(&dn->dn_holds), <=, 1);
	ASSERT3P(list_head(&dn->dn_dbufs), ==, NULL);

	for (i = 0; i < TXG_SIZE; i++) {
		ASSERT3U(dn->dn_next_nlevels[i], ==, 0);
		ASSERT3U(dn->dn_next_indblkshift[i], ==, 0);
		ASSERT3U(dn->dn_dirtyblksz[i], ==, 0);
		ASSERT3P(list_head(&dn->dn_dirty_dbufs[i]), ==, NULL);
		ASSERT3U(avl_numnodes(&dn->dn_ranges[i]), ==, 0);
	}

	dn->dn_type = ot;
	dnode_setdblksz(dn, blocksize);
	dn->dn_indblkshift = ibs;
	dn->dn_nlevels = 1;
	dn->dn_nblkptr = 1 + ((DN_MAX_BONUSLEN - bonuslen) >> SPA_BLKPTRSHIFT);
	dn->dn_bonustype = bonustype;
	dn->dn_bonuslen = bonuslen;
	dn->dn_checksum = ZIO_CHECKSUM_INHERIT;
	dn->dn_compress = ZIO_COMPRESS_INHERIT;
	dn->dn_dirtyctx = 0;

	dn->dn_free_txg = 0;
	if (dn->dn_dirtyctx_firstset) {
		kmem_free(dn->dn_dirtyctx_firstset, 1);
		dn->dn_dirtyctx_firstset = NULL;
	}

	dn->dn_allocated_txg = tx->tx_txg;
	dnode_setdirty(dn, tx);
}

void
dnode_reallocate(dnode_t *dn, dmu_object_type_t ot, int blocksize,
    dmu_object_type_t bonustype, int bonuslen, dmu_tx_t *tx)
{
	dmu_buf_impl_t *db = NULL;

	ASSERT3U(blocksize, >=, SPA_MINBLOCKSIZE);
	ASSERT3U(blocksize, <=, SPA_MAXBLOCKSIZE);
	ASSERT3U(blocksize % SPA_MINBLOCKSIZE, ==, 0);
	ASSERT3P(list_head(&dn->dn_dbufs), ==, NULL);
	ASSERT(!(dn->dn_object & DMU_PRIVATE_OBJECT) || dmu_tx_private_ok(tx));
	ASSERT(tx->tx_txg != 0);
	ASSERT((bonustype == DMU_OT_NONE && bonuslen == 0) ||
	    (bonustype != DMU_OT_NONE && bonuslen != 0));
	ASSERT3U(bonustype, <, DMU_OT_NUMTYPES);
	ASSERT3U(bonuslen, <=, DN_MAX_BONUSLEN);
	ASSERT(dn->dn_dirtyblksz[0] == 0);
	ASSERT(dn->dn_dirtyblksz[1] == 0);
	ASSERT(dn->dn_dirtyblksz[2] == 0);
	ASSERT(dn->dn_dirtyblksz[3] == 0);

	/*
	 * XXX I should really have a generation number to tell if we
	 * need to do this...
	 */
	if (blocksize != dn->dn_datablksz ||
	    dn->dn_bonustype != bonustype || dn->dn_bonuslen != bonuslen) {
		/* free all old data */
		dnode_free_range(dn, 0, -1ULL, tx);
	}

	/* change blocksize */
	rw_enter(&dn->dn_struct_rwlock, RW_WRITER);
	dnode_setdblksz(dn, blocksize);
	dnode_setdirty(dn, tx);
	/* don't need dd_dirty_mtx, dnode is already dirty */
	ASSERT(dn->dn_dirtyblksz[tx->tx_txg&TXG_MASK] != 0);
	dn->dn_dirtyblksz[tx->tx_txg&TXG_MASK] = blocksize;
	rw_exit(&dn->dn_struct_rwlock);

	/* change type */
	dn->dn_type = ot;

	if (dn->dn_bonuslen != bonuslen) {
		/* change bonus size */
		if (bonuslen == 0)
			bonuslen = 1; /* XXX */
		db = dbuf_hold_bonus(dn, FTAG);
		dbuf_read(db);
		mutex_enter(&db->db_mtx);
		ASSERT3U(db->db.db_size, ==, dn->dn_bonuslen);
		ASSERT(db->db.db_data != NULL);
		db->db.db_size = bonuslen;
		mutex_exit(&db->db_mtx);
		dbuf_dirty(db, tx);
	}

	/* change bonus size and type */
	mutex_enter(&dn->dn_mtx);
	dn->dn_bonustype = bonustype;
	dn->dn_bonuslen = bonuslen;
	dn->dn_nblkptr = 1 + ((DN_MAX_BONUSLEN - bonuslen) >> SPA_BLKPTRSHIFT);
	dn->dn_checksum = ZIO_CHECKSUM_INHERIT;
	dn->dn_compress = ZIO_COMPRESS_INHERIT;
	ASSERT3U(dn->dn_nblkptr, <=, DN_MAX_NBLKPTR);

	dn->dn_allocated_txg = tx->tx_txg;
	mutex_exit(&dn->dn_mtx);

	if (db)
		dbuf_remove_ref(db, FTAG);
}

void
dnode_special_close(dnode_t *dn)
{
	dnode_destroy(dn);
}

dnode_t *
dnode_special_open(objset_impl_t *os, dnode_phys_t *dnp, uint64_t object)
{
	dnode_t *dn = dnode_create(os, dnp, NULL, object);
	dnode_verify(dn);
	return (dn);
}

static void
dnode_buf_pageout(dmu_buf_t *db, void *arg)
{
	dnode_t **children_dnodes = arg;
	int i;
	int epb = db->db_size >> DNODE_SHIFT;

	for (i = 0; i < epb; i++) {
		dnode_t *dn = children_dnodes[i];
		int n;

		if (dn == NULL)
			continue;
#ifdef ZFS_DEBUG
		/*
		 * If there are holds on this dnode, then there should
		 * be holds on the dnode's containing dbuf as well; thus
		 * it wouldn't be eligable for eviction and this function
		 * would not have been called.
		 */
		ASSERT(refcount_is_zero(&dn->dn_holds));
		ASSERT(list_head(&dn->dn_dbufs) == NULL);
		ASSERT(refcount_is_zero(&dn->dn_tx_holds));

		for (n = 0; n < TXG_SIZE; n++)
			ASSERT(dn->dn_dirtyblksz[n] == 0);
#endif
		children_dnodes[i] = NULL;
		dnode_destroy(dn);
	}
	kmem_free(children_dnodes, epb * sizeof (dnode_t *));
}

/*
 * Returns held dnode if the object number is valid, NULL if not.
 * Note that this will succeed even for free dnodes.
 */
dnode_t *
dnode_hold_impl(objset_impl_t *os, uint64_t object, int flag, void *ref)
{
	int epb, idx;
	int drop_struct_lock = FALSE;
	uint64_t blk;
	dnode_t *mdn, *dn;
	dmu_buf_impl_t *db;
	dnode_t **children_dnodes;

	if (object == 0 || object >= DN_MAX_OBJECT)
		return (NULL);

	mdn = os->os_meta_dnode;

	dnode_verify(mdn);

	if (!RW_WRITE_HELD(&mdn->dn_struct_rwlock)) {
		rw_enter(&mdn->dn_struct_rwlock, RW_READER);
		drop_struct_lock = TRUE;
	}

	blk = dbuf_whichblock(mdn, object * sizeof (dnode_phys_t));

	db = dbuf_hold(mdn, blk);
	if (drop_struct_lock)
		rw_exit(&mdn->dn_struct_rwlock);
	dbuf_read(db);

	ASSERT3U(db->db.db_size, >=, 1<<DNODE_SHIFT);
	epb = db->db.db_size >> DNODE_SHIFT;

	idx = object & (epb-1);

	children_dnodes = dmu_buf_get_user(&db->db);
	if (children_dnodes == NULL) {
		dnode_t **winner;
		children_dnodes = kmem_zalloc(epb * sizeof (dnode_t *),
		    KM_SLEEP);
		if (winner = dmu_buf_set_user(&db->db, children_dnodes, NULL,
		    dnode_buf_pageout)) {
			kmem_free(children_dnodes, epb * sizeof (dnode_t *));
			children_dnodes = winner;
		}
	}

	if ((dn = children_dnodes[idx]) == NULL) {
		dnode_t *winner;
		dn = dnode_create(os, (dnode_phys_t *)db->db.db_data+idx,
			db, object);
		winner = atomic_cas_ptr(&children_dnodes[idx], NULL, dn);
		if (winner != NULL) {
			dnode_destroy(dn);
			dn = winner;
		}
	}

	mutex_enter(&dn->dn_mtx);
	if (dn->dn_free_txg ||
	    ((flag & DNODE_MUST_BE_ALLOCATED) && dn->dn_type == DMU_OT_NONE) ||
	    ((flag & DNODE_MUST_BE_FREE) && dn->dn_type != DMU_OT_NONE)) {
		mutex_exit(&dn->dn_mtx);
		dbuf_rele(db);
		return (NULL);
	}
	mutex_exit(&dn->dn_mtx);

	if (refcount_add(&dn->dn_holds, ref) == 1)
		dbuf_add_ref(db, dn);

	dnode_verify(dn);
	ASSERT3P(dn->dn_dbuf, ==, db);
	ASSERT3U(dn->dn_object, ==, object);
	dbuf_rele(db);

	return (dn);
}

/*
 * Return held dnode if the object is allocated, NULL if not.
 */
dnode_t *
dnode_hold(objset_impl_t *os, uint64_t object, void *ref)
{
	return (dnode_hold_impl(os, object, DNODE_MUST_BE_ALLOCATED, ref));
}

void
dnode_add_ref(dnode_t *dn, void *ref)
{
	ASSERT(refcount_count(&dn->dn_holds) > 0);
	(void) refcount_add(&dn->dn_holds, ref);
}

void
dnode_rele(dnode_t *dn, void *ref)
{
	uint64_t refs;

	refs = refcount_remove(&dn->dn_holds, ref);
	/* NOTE: the DNODE_DNODE does not have a dn_dbuf */
	if (refs == 0 && dn->dn_dbuf)
		dbuf_remove_ref(dn->dn_dbuf, dn);
}

void
dnode_setdirty(dnode_t *dn, dmu_tx_t *tx)
{
	objset_impl_t *os = dn->dn_objset;
	uint64_t txg = tx->tx_txg;

	if (IS_DNODE_DNODE(dn->dn_object))
		return;

	dnode_verify(dn);

#ifdef ZFS_DEBUG
	mutex_enter(&dn->dn_mtx);
	ASSERT(dn->dn_phys->dn_type || dn->dn_allocated_txg);
	/* ASSERT(dn->dn_free_txg == 0 || dn->dn_free_txg >= txg); */
	mutex_exit(&dn->dn_mtx);
#endif

	mutex_enter(&os->os_lock);

	/*
	 * If we are already marked dirty, we're done.
	 */
	if (dn->dn_dirtyblksz[txg&TXG_MASK] > 0) {
		mutex_exit(&os->os_lock);
		return;
	}

	ASSERT(!refcount_is_zero(&dn->dn_holds) || list_head(&dn->dn_dbufs));
	ASSERT(dn->dn_datablksz != 0);
	dn->dn_dirtyblksz[txg&TXG_MASK] = dn->dn_datablksz;

	dprintf_ds(os->os_dsl_dataset, "obj=%llu txg=%llu\n",
	    dn->dn_object, txg);

	if (dn->dn_free_txg > 0 && dn->dn_free_txg <= txg) {
		list_insert_tail(&os->os_free_dnodes[txg&TXG_MASK], dn);
	} else {
		list_insert_tail(&os->os_dirty_dnodes[txg&TXG_MASK], dn);
	}

	mutex_exit(&os->os_lock);

	/*
	 * The dnode maintains a hold on its containing dbuf as
	 * long as there are holds on it.  Each instantiated child
	 * dbuf maintaines a hold on the dnode.  When the last child
	 * drops its hold, the dnode will drop its hold on the
	 * containing dbuf. We add a "dirty hold" here so that the
	 * dnode will hang around after we finish processing its
	 * children.
	 */
	(void) refcount_add(&dn->dn_holds, (void *)(uintptr_t)tx->tx_txg);

	dbuf_dirty(dn->dn_dbuf, tx);

	dsl_dataset_dirty(os->os_dsl_dataset, tx);
}

void
dnode_free(dnode_t *dn, dmu_tx_t *tx)
{
	dprintf("dn=%p txg=%llu\n", dn, tx->tx_txg);

	/* we should be the only holder... hopefully */
	/* ASSERT3U(refcount_count(&dn->dn_holds), ==, 1); */

	mutex_enter(&dn->dn_mtx);
	if (dn->dn_type == DMU_OT_NONE || dn->dn_free_txg) {
		mutex_exit(&dn->dn_mtx);
		return;
	}
	dn->dn_free_txg = tx->tx_txg;
	mutex_exit(&dn->dn_mtx);

	/*
	 * If the dnode is already dirty, it needs to be moved from
	 * the dirty list to the free list.
	 */
	mutex_enter(&dn->dn_objset->os_lock);
	if (dn->dn_dirtyblksz[tx->tx_txg&TXG_MASK] > 0) {
		list_remove(
		    &dn->dn_objset->os_dirty_dnodes[tx->tx_txg&TXG_MASK], dn);
		list_insert_tail(
		    &dn->dn_objset->os_free_dnodes[tx->tx_txg&TXG_MASK], dn);
		mutex_exit(&dn->dn_objset->os_lock);
	} else {
		mutex_exit(&dn->dn_objset->os_lock);
		dnode_setdirty(dn, tx);
	}
}

/*
 * Try to change the block size for the indicated dnode.  This can only
 * succeed if there are no blocks allocated or dirty beyond first block
 */
int
dnode_set_blksz(dnode_t *dn, uint64_t size, int ibs, dmu_tx_t *tx)
{
	dmu_buf_impl_t *db, *db_next;
	int have_db0 = FALSE;
	int err = ENOTSUP;

	if (size == 0)
		size = SPA_MINBLOCKSIZE;
	if (size > SPA_MAXBLOCKSIZE)
		size = SPA_MAXBLOCKSIZE;
	else
		size = P2ROUNDUP(size, SPA_MINBLOCKSIZE);

	if (ibs == 0)
		ibs = dn->dn_indblkshift;

	if (size >> SPA_MINBLOCKSHIFT == dn->dn_datablkszsec &&
	    ibs == dn->dn_indblkshift)
		return (0);

	rw_enter(&dn->dn_struct_rwlock, RW_WRITER);

	/* Check for any allocated blocks beyond the first */
	if (dn->dn_phys->dn_maxblkid != 0)
		goto end;

	/*
	 * Any buffers allocated for blocks beyond the first
	 * must be evictable/evicted, because they're the wrong size.
	 */
	mutex_enter(&dn->dn_dbufs_mtx);
	/*
	 * Since we have the dn_dbufs_mtx, nothing can be
	 * removed from dn_dbufs.  Since we have dn_struct_rwlock/w,
	 * nothing can be added to dn_dbufs.
	 */
	for (db = list_head(&dn->dn_dbufs); db; db = db_next) {
		db_next = list_next(&dn->dn_dbufs, db);

		if (db->db_blkid == 0) {
			have_db0 = TRUE;
		} else if (db->db_blkid != DB_BONUS_BLKID) {
			mutex_exit(&dn->dn_dbufs_mtx);
			goto end;
		}
	}
	mutex_exit(&dn->dn_dbufs_mtx);

	/* Fast-track if there is no data in the file */
	if (BP_IS_HOLE(&dn->dn_phys->dn_blkptr[0]) && !have_db0) {
		dnode_setdblksz(dn, size);
		dn->dn_indblkshift = ibs;
		dnode_setdirty(dn, tx);
		/* don't need dd_dirty_mtx, dnode is already dirty */
		dn->dn_dirtyblksz[tx->tx_txg&TXG_MASK] = size;
		dn->dn_next_indblkshift[tx->tx_txg&TXG_MASK] = ibs;
		rw_exit(&dn->dn_struct_rwlock);
		return (0);
	}

	/* obtain the old block */
	db = dbuf_hold(dn, 0);

	/* Not allowed to decrease the size if there is data present */
	if (size < db->db.db_size) {
		dbuf_rele(db);
		goto end;
	}

	dbuf_new_size(db, size, tx);

	dnode_setdblksz(dn, size);
	dn->dn_indblkshift = ibs;
	/* don't need dd_dirty_mtx, dnode is already dirty */
	dn->dn_dirtyblksz[tx->tx_txg&TXG_MASK] = size;
	dn->dn_next_indblkshift[tx->tx_txg&TXG_MASK] = ibs;
	dbuf_rele(db);

	err = 0;
end:
	rw_exit(&dn->dn_struct_rwlock);
	return (err);
}

uint64_t
dnode_max_nonzero_offset(dnode_t *dn)
{
	if (dn->dn_phys->dn_maxblkid == 0 &&
	    BP_IS_HOLE(&dn->dn_phys->dn_blkptr[0]))
		return (0);
	else
		return ((dn->dn_phys->dn_maxblkid+1) * dn->dn_datablksz);
}

void
dnode_new_blkid(dnode_t *dn, uint64_t blkid, dmu_tx_t *tx)
{
	uint64_t txgoff = tx->tx_txg & TXG_MASK;
	int drop_struct_lock = FALSE;
	int epbs, old_nlevels, new_nlevels;
	uint64_t sz;

	if (blkid == DB_BONUS_BLKID)
		return;

	if (!RW_WRITE_HELD(&dn->dn_struct_rwlock)) {
		rw_enter(&dn->dn_struct_rwlock, RW_WRITER);
		drop_struct_lock = TRUE;
	}

	if (blkid > dn->dn_maxblkid)
		dn->dn_maxblkid = blkid;

	/*
	 * Compute the number of levels necessary to support the
	 * new blkid.
	 */
	new_nlevels = 1;
	epbs = dn->dn_indblkshift - SPA_BLKPTRSHIFT;

	for (sz = dn->dn_nblkptr; sz <= blkid && sz >= dn->dn_nblkptr;
	    sz <<= epbs)
		new_nlevels++;
	old_nlevels = dn->dn_nlevels;

	if (new_nlevels > dn->dn_next_nlevels[txgoff])
		dn->dn_next_nlevels[txgoff] = new_nlevels;

	if (new_nlevels > old_nlevels) {
		dprintf("dn %p increasing nlevels from %u to %u\n",
		    dn, dn->dn_nlevels, new_nlevels);
		dn->dn_nlevels = new_nlevels;
	}

	/*
	 * Dirty the left indirects.
	 * Note: the caller should have just dnode_use_space()'d one
	 * data block's worth, so we could subtract that out of
	 * dn_inflight_data to determine if there is any dirty data
	 * besides this block.
	 * We don't strictly need to dirty them unless there's
	 * *something* in the object (eg. on disk or dirty)...
	 */
	if (new_nlevels > old_nlevels) {
		dmu_buf_impl_t *db = dbuf_hold_level(dn, old_nlevels, 0, FTAG);
		dprintf("dn %p dirtying left indirects\n", dn);
		dbuf_dirty(db, tx);
		dbuf_remove_ref(db, FTAG);
	}
#ifdef ZFS_DEBUG
	else if (old_nlevels > 1 && new_nlevels > old_nlevels) {
		dmu_buf_impl_t *db;
		int i;

		for (i = 0; i < dn->dn_nblkptr; i++) {
			db = dbuf_hold_level(dn, old_nlevels-1, i, FTAG);
			ASSERT(!
			    list_link_active(&db->db_dirty_node[txgoff]));
			dbuf_remove_ref(db, FTAG);
		}
	}
#endif

	dprintf("dn %p done\n", dn);

out:
	if (drop_struct_lock)
		rw_exit(&dn->dn_struct_rwlock);
}

void
dnode_clear_range(dnode_t *dn, uint64_t blkid, uint64_t nblks, dmu_tx_t *tx)
{
	avl_tree_t *tree = &dn->dn_ranges[tx->tx_txg&TXG_MASK];
	avl_index_t where;
	free_range_t *rp;
	free_range_t rp_tofind;
	uint64_t endblk = blkid + nblks;

	ASSERT(MUTEX_HELD(&dn->dn_mtx));
	ASSERT(nblks <= UINT64_MAX - blkid); /* no overflow */

	dprintf_dnode(dn, "blkid=%llu nblks=%llu txg=%llu\n",
	    blkid, nblks, tx->tx_txg);
	rp_tofind.fr_blkid = blkid;
	rp = avl_find(tree, &rp_tofind, &where);
	if (rp == NULL)
		rp = avl_nearest(tree, where, AVL_BEFORE);
	if (rp == NULL)
		rp = avl_nearest(tree, where, AVL_AFTER);

	while (rp && (rp->fr_blkid <= blkid + nblks)) {
		uint64_t fr_endblk = rp->fr_blkid + rp->fr_nblks;
		free_range_t *nrp = AVL_NEXT(tree, rp);

		if (blkid <= rp->fr_blkid && endblk >= fr_endblk) {
			/* clear this entire range */
			avl_remove(tree, rp);
			kmem_free(rp, sizeof (free_range_t));
		} else if (blkid <= rp->fr_blkid &&
		    endblk > rp->fr_blkid && endblk < fr_endblk) {
			/* clear the beginning of this range */
			rp->fr_blkid = endblk;
			rp->fr_nblks = fr_endblk - endblk;
		} else if (blkid > rp->fr_blkid && blkid < fr_endblk &&
		    endblk >= fr_endblk) {
			/* clear the end of this range */
			rp->fr_nblks = blkid - rp->fr_blkid;
		} else if (blkid > rp->fr_blkid && endblk < fr_endblk) {
			/* clear a chunk out of this range */
			free_range_t *new_rp =
			    kmem_alloc(sizeof (free_range_t), KM_SLEEP);

			new_rp->fr_blkid = endblk;
			new_rp->fr_nblks = fr_endblk - endblk;
			avl_insert_here(tree, new_rp, rp, AVL_AFTER);
			rp->fr_nblks = blkid - rp->fr_blkid;
		}
		/* there may be no overlap */
		rp = nrp;
	}
}

void
dnode_free_range(dnode_t *dn, uint64_t off, uint64_t len, dmu_tx_t *tx)
{
	dmu_buf_impl_t *db;
	uint64_t start, objsize, blkid, nblks;
	int blkshift, blksz, tail, head, epbs;
	int trunc = FALSE;

	rw_enter(&dn->dn_struct_rwlock, RW_WRITER);
	blksz = dn->dn_datablksz;
	blkshift = dn->dn_datablkshift;
	epbs = dn->dn_indblkshift - SPA_BLKPTRSHIFT;

	/* If the range is past the end of the file, this is a no-op */
	objsize = blksz * (dn->dn_maxblkid+1);
	if (off >= objsize)
		goto out;
	if (len == -1ULL) {
		len = UINT64_MAX - off;
		trunc = TRUE;
	}

	/*
	 * First, block align the region to free:
	 */
	if (dn->dn_maxblkid == 0) {
		if (off == 0) {
			head = 0;
		} else {
			head = blksz - off;
			ASSERT3U(head, >, 0);
		}
		start = off;
	} else {
		ASSERT(ISP2(blksz));
		head = P2NPHASE(off, blksz);
		start = P2PHASE(off, blksz);
	}
	/* zero out any partial block data at the start of the range */
	if (head) {
		ASSERT3U(start + head, ==, blksz);
		if (len < head)
			head = len;
		if (dbuf_hold_impl(dn, 0, dbuf_whichblock(dn, off), TRUE,
		    FTAG, &db) == 0) {
			caddr_t data;

			/* don't dirty if it isn't on disk and isn't dirty */
			if (db->db_dirtied ||
			    (db->db_blkptr && !BP_IS_HOLE(db->db_blkptr))) {
				rw_exit(&dn->dn_struct_rwlock);
				dbuf_will_dirty(db, tx);
				rw_enter(&dn->dn_struct_rwlock, RW_WRITER);
				data = db->db.db_data;
				bzero(data + start, head);
			}
			dbuf_remove_ref(db, FTAG);
		}
		off += head;
		len -= head;
	}
	/* If the range was less than one block, we are done */
	if (len == 0)
		goto out;

	/* If the remaining range is past the end of the file, we are done */
	if (off > dn->dn_maxblkid << blkshift)
		goto out;

	if (off + len == UINT64_MAX)
		tail = 0;
	else
		tail = P2PHASE(len, blksz);

	ASSERT3U(P2PHASE(off, blksz), ==, 0);
	/* zero out any partial block data at the end of the range */
	if (tail) {
		if (len < tail)
			tail = len;
		if (dbuf_hold_impl(dn, 0, dbuf_whichblock(dn, off+len),
		    TRUE, FTAG, &db) == 0) {
			/* don't dirty if it isn't on disk and isn't dirty */
			if (db->db_dirtied ||
			    (db->db_blkptr && !BP_IS_HOLE(db->db_blkptr))) {
				rw_exit(&dn->dn_struct_rwlock);
				dbuf_will_dirty(db, tx);
				rw_enter(&dn->dn_struct_rwlock, RW_WRITER);
				bzero(db->db.db_data, tail);
			}
			dbuf_remove_ref(db, FTAG);
		}
		len -= tail;
	}
	/* If the range did not include a full block, we are done */
	if (len == 0)
		goto out;

	/* dirty the left indirects */
	if (dn->dn_nlevels > 1 && off != 0) {
		db = dbuf_hold_level(dn, 1,
		    (off - head) >> (blkshift + epbs), FTAG);
		dbuf_will_dirty(db, tx);
		dbuf_remove_ref(db, FTAG);
	}

	/* dirty the right indirects */
	if (dn->dn_nlevels > 1 && !trunc) {
		db = dbuf_hold_level(dn, 1,
		    (off + len + tail - 1) >> (blkshift + epbs), FTAG);
		dbuf_will_dirty(db, tx);
		dbuf_remove_ref(db, FTAG);
	}

	/*
	 * Finally, add this range to the dnode range list, we
	 * will finish up this free operation in the syncing phase.
	 */
	ASSERT(IS_P2ALIGNED(off, 1<<blkshift));
	ASSERT(off + len == UINT64_MAX || IS_P2ALIGNED(len, 1<<blkshift));
	blkid = off >> blkshift;
	nblks = len >> blkshift;

	if (trunc)
		dn->dn_maxblkid = (blkid ? blkid - 1 : 0);

	mutex_enter(&dn->dn_mtx);
	dnode_clear_range(dn, blkid, nblks, tx);
	{
		free_range_t *rp, *found;
		avl_index_t where;
		avl_tree_t *tree = &dn->dn_ranges[tx->tx_txg&TXG_MASK];

		/* Add new range to dn_ranges */
		rp = kmem_alloc(sizeof (free_range_t), KM_SLEEP);
		rp->fr_blkid = blkid;
		rp->fr_nblks = nblks;
		found = avl_find(tree, rp, &where);
		ASSERT(found == NULL);
		avl_insert(tree, rp, where);
		dprintf_dnode(dn, "blkid=%llu nblks=%llu txg=%llu\n",
		    blkid, nblks, tx->tx_txg);
	}
	mutex_exit(&dn->dn_mtx);

	dbuf_free_range(dn, blkid, nblks, tx);
	dnode_setdirty(dn, tx);
out:
	rw_exit(&dn->dn_struct_rwlock);
}

/* return TRUE if this blkid was freed in a recent txg, or FALSE if it wasn't */
uint64_t
dnode_block_freed(dnode_t *dn, uint64_t blkid)
{
	free_range_t range_tofind;
	void *dp = spa_get_dsl(dn->dn_objset->os_spa);
	int i;

	if (blkid == DB_BONUS_BLKID)
		return (FALSE);

	/*
	 * If we're in the process of opening the pool, dp will not be
	 * set yet, but there shouldn't be anything dirty.
	 */
	if (dp == NULL)
		return (FALSE);

	if (dn->dn_free_txg)
		return (TRUE);

	/*
	 * If dn_datablkshift is not set, then there's only a single
	 * block, in which case there will never be a free range so it
	 * won't matter.
	 */
	range_tofind.fr_blkid = blkid;
	mutex_enter(&dn->dn_mtx);
	for (i = 0; i < TXG_SIZE; i++) {
		free_range_t *range_found;
		avl_index_t idx;

		range_found = avl_find(&dn->dn_ranges[i], &range_tofind, &idx);
		if (range_found) {
			ASSERT(range_found->fr_nblks > 0);
			break;
		}
		range_found = avl_nearest(&dn->dn_ranges[i], idx, AVL_BEFORE);
		if (range_found &&
		    range_found->fr_blkid + range_found->fr_nblks > blkid)
			break;
	}
	mutex_exit(&dn->dn_mtx);
	return (i < TXG_SIZE);
}

/* call from syncing context when we actually write/free space for this dnode */
void
dnode_diduse_space(dnode_t *dn, int64_t space)
{
	uint64_t sectors;

	dprintf_dnode(dn, "dn=%p dnp=%p secphys=%llu space=%lld\n",
	    dn, dn->dn_phys,
	    (u_longlong_t)dn->dn_phys->dn_secphys,
	    (longlong_t)space);

	ASSERT(P2PHASE(space, 1<<DEV_BSHIFT) == 0);

	mutex_enter(&dn->dn_mtx);
	if (space > 0) {
		sectors = space >> DEV_BSHIFT;
		ASSERT3U(dn->dn_phys->dn_secphys + sectors, >=,
		    dn->dn_phys->dn_secphys);
		dn->dn_phys->dn_secphys += sectors;
	} else {
		sectors = -space >> DEV_BSHIFT;
		ASSERT3U(dn->dn_phys->dn_secphys, >=, sectors);
		dn->dn_phys->dn_secphys -= sectors;
	}
	mutex_exit(&dn->dn_mtx);
}

/*
 * Call when we think we're going to write/free space in open context.
 * Be conservative (ie. OK to write less than this or free more than
 * this, but don't write more or free less).
 */
void
dnode_willuse_space(dnode_t *dn, int64_t space, dmu_tx_t *tx)
{
	objset_impl_t *os = dn->dn_objset;
	dsl_dataset_t *ds = os->os_dsl_dataset;

	if (space > 0)
		space = spa_get_asize(os->os_spa, space);

	if (ds)
		dsl_dir_willuse_space(ds->ds_dir, space, tx);

	dmu_tx_willuse_space(tx, space);
}

static int
dnode_next_offset_level(dnode_t *dn, boolean_t hole, uint64_t *offset,
	int lvl, uint64_t blkfill)
{
	dmu_buf_impl_t *db = NULL;
	void *data = NULL;
	uint64_t epbs = dn->dn_phys->dn_indblkshift - SPA_BLKPTRSHIFT;
	uint64_t epb = 1ULL << epbs;
	uint64_t minfill, maxfill;
	int i, error, span;

	dprintf("probing object %llu offset %llx level %d of %u\n",
	    dn->dn_object, *offset, lvl, dn->dn_phys->dn_nlevels);

	if (lvl == dn->dn_phys->dn_nlevels) {
		error = 0;
		epb = dn->dn_phys->dn_nblkptr;
		data = dn->dn_phys->dn_blkptr;
	} else {
		uint64_t blkid = dbuf_whichblock(dn, *offset) >> (epbs * lvl);
		error = dbuf_hold_impl(dn, lvl, blkid, TRUE, FTAG, &db);
		if (error) {
			if (error == ENOENT)
				return (hole ? 0 : ESRCH);
			return (error);
		}
		dbuf_read_havestruct(db);
		data = db->db.db_data;
	}

	if (lvl == 0) {
		dnode_phys_t *dnp = data;
		span = DNODE_SHIFT;
		ASSERT(dn->dn_type == DMU_OT_DNODE);

		for (i = (*offset >> span) & (blkfill - 1); i < blkfill; i++) {
			if (!dnp[i].dn_type == hole)
				break;
			*offset += 1ULL << span;
		}
		if (i == blkfill)
			error = ESRCH;
	} else {
		blkptr_t *bp = data;
		span = (lvl - 1) * epbs + dn->dn_datablkshift;
		minfill = 0;
		maxfill = blkfill << ((lvl - 1) * epbs);

		if (hole)
			maxfill--;
		else
			minfill++;

		for (i = (*offset >> span) & ((1ULL << epbs) - 1);
		    i < epb; i++) {
			if (bp[i].blk_fill >= minfill &&
			    bp[i].blk_fill <= maxfill)
				break;
			*offset += 1ULL << span;
		}
		if (i >= epb)
			error = ESRCH;
	}

	if (db)
		dbuf_remove_ref(db, FTAG);

	return (error);
}

/*
 * Find the next hole, data, or sparse region at or after *offset.
 * The value 'blkfill' tells us how many items we expect to find
 * in an L0 data block; this value is 1 for normal objects,
 * DNODES_PER_BLOCK for the meta dnode, and some fraction of
 * DNODES_PER_BLOCK when searching for sparse regions thereof.
 * Examples:
 *
 * dnode_next_offset(dn, hole, offset, 1, 1);
 *	Finds the next hole/data in a file.
 *	Used in dmu_offset_next().
 *
 * dnode_next_offset(mdn, hole, offset, 0, DNODES_PER_BLOCK);
 *	Finds the next free/allocated dnode an objset's meta-dnode.
 *	Used in dmu_object_next().
 *
 * dnode_next_offset(mdn, TRUE, offset, 2, DNODES_PER_BLOCK >> 2);
 *	Finds the next L2 meta-dnode bp that's at most 1/4 full.
 *	Used in dmu_object_alloc().
 */
int
dnode_next_offset(dnode_t *dn, boolean_t hole, uint64_t *offset,
    int minlvl, uint64_t blkfill)
{
	int lvl, maxlvl;
	int error = 0;
	uint64_t initial_offset = *offset;

	rw_enter(&dn->dn_struct_rwlock, RW_READER);

	if (dn->dn_phys->dn_nlevels == 0) {
		rw_exit(&dn->dn_struct_rwlock);
		return (ESRCH);
	}

	if (dn->dn_datablkshift == 0) {
		if (*offset < dn->dn_datablksz) {
			if (hole)
				*offset = dn->dn_datablksz;
		} else {
			error = ESRCH;
		}
		rw_exit(&dn->dn_struct_rwlock);
		return (error);
	}

	maxlvl = dn->dn_phys->dn_nlevels;

	for (lvl = minlvl; lvl <= maxlvl; lvl++) {
		error = dnode_next_offset_level(dn, hole, offset, lvl, blkfill);
		if (error == 0)
			break;
	}

	while (--lvl >= minlvl && error == 0)
		error = dnode_next_offset_level(dn, hole, offset, lvl, blkfill);

	rw_exit(&dn->dn_struct_rwlock);

	if (initial_offset > *offset)
		return (ESRCH);

	return (error);
}