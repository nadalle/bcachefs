// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"
#include "bkey_methods.h"
#include "btree_update.h"
#include "error.h"
#include "extents.h"
#include "inode.h"
#include "str_hash.h"

#include <linux/random.h>

#include <asm/unaligned.h>

const char * const bch2_inode_opts[] = {
#define x(name, ...)	#name,
	BCH_INODE_OPTS()
#undef  x
	NULL,
};

static const u8 byte_table[8] = { 1, 2, 3, 4, 6, 8, 10, 13 };
static const u8 bits_table[8] = {
	1  * 8 - 1,
	2  * 8 - 2,
	3  * 8 - 3,
	4  * 8 - 4,
	6  * 8 - 5,
	8  * 8 - 6,
	10 * 8 - 7,
	13 * 8 - 8,
};

static int inode_encode_field(u8 *out, u8 *end, u64 hi, u64 lo)
{
	__be64 in[2] = { cpu_to_be64(hi), cpu_to_be64(lo), };
	unsigned shift, bytes, bits = likely(!hi)
		? fls64(lo)
		: fls64(hi) + 64;

	for (shift = 1; shift <= 8; shift++)
		if (bits < bits_table[shift - 1])
			goto got_shift;

	BUG();
got_shift:
	bytes = byte_table[shift - 1];

	BUG_ON(out + bytes > end);

	memcpy(out, (u8 *) in + 16 - bytes, bytes);
	*out |= (1 << 8) >> shift;

	return bytes;
}

static int inode_decode_field(const u8 *in, const u8 *end,
			      u64 out[2], unsigned *out_bits)
{
	__be64 be[2] = { 0, 0 };
	unsigned bytes, shift;
	u8 *p;

	if (in >= end)
		return -1;

	if (!*in)
		return -1;

	/*
	 * position of highest set bit indicates number of bytes:
	 * shift = number of bits to remove in high byte:
	 */
	shift	= 8 - __fls(*in); /* 1 <= shift <= 8 */
	bytes	= byte_table[shift - 1];

	if (in + bytes > end)
		return -1;

	p = (u8 *) be + 16 - bytes;
	memcpy(p, in, bytes);
	*p ^= (1 << 8) >> shift;

	out[0] = be64_to_cpu(be[0]);
	out[1] = be64_to_cpu(be[1]);
	*out_bits = out[0] ? 64 + fls64(out[0]) : fls64(out[1]);

	return bytes;
}

void bch2_inode_pack(struct bkey_inode_buf *packed,
		     const struct bch_inode_unpacked *inode)
{
	u8 *out = packed->inode.v.fields;
	u8 *end = (void *) &packed[1];
	u8 *last_nonzero_field = out;
	unsigned nr_fields = 0, last_nonzero_fieldnr = 0;
	unsigned bytes;

	bkey_inode_init(&packed->inode.k_i);
	packed->inode.k.p.inode		= inode->bi_inum;
	packed->inode.v.bi_hash_seed	= inode->bi_hash_seed;
	packed->inode.v.bi_flags	= cpu_to_le32(inode->bi_flags);
	packed->inode.v.bi_mode		= cpu_to_le16(inode->bi_mode);

#define x(_name, _bits)					\
	out += inode_encode_field(out, end, 0, inode->_name);		\
	nr_fields++;							\
									\
	if (inode->_name) {						\
		last_nonzero_field = out;				\
		last_nonzero_fieldnr = nr_fields;			\
	}

	BCH_INODE_FIELDS()
#undef  x

	out = last_nonzero_field;
	nr_fields = last_nonzero_fieldnr;

	bytes = out - (u8 *) &packed->inode.v;
	set_bkey_val_bytes(&packed->inode.k, bytes);
	memset_u64s_tail(&packed->inode.v, 0, bytes);

	SET_INODE_NR_FIELDS(&packed->inode.v, nr_fields);

	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		struct bch_inode_unpacked unpacked;

		int ret = bch2_inode_unpack(inode_i_to_s_c(&packed->inode),
					   &unpacked);
		BUG_ON(ret);
		BUG_ON(unpacked.bi_inum		!= inode->bi_inum);
		BUG_ON(unpacked.bi_hash_seed	!= inode->bi_hash_seed);
		BUG_ON(unpacked.bi_mode		!= inode->bi_mode);

#define x(_name, _bits)	BUG_ON(unpacked._name != inode->_name);
		BCH_INODE_FIELDS()
#undef  x
	}
}

int bch2_inode_unpack(struct bkey_s_c_inode inode,
		      struct bch_inode_unpacked *unpacked)
{
	const u8 *in = inode.v->fields;
	const u8 *end = (void *) inode.v + bkey_val_bytes(inode.k);
	u64 field[2];
	unsigned fieldnr = 0, field_bits;
	int ret;

	unpacked->bi_inum	= inode.k->p.inode;
	unpacked->bi_hash_seed	= inode.v->bi_hash_seed;
	unpacked->bi_flags	= le32_to_cpu(inode.v->bi_flags);
	unpacked->bi_mode	= le16_to_cpu(inode.v->bi_mode);

#define x(_name, _bits)					\
	if (fieldnr++ == INODE_NR_FIELDS(inode.v)) {			\
		memset(&unpacked->_name, 0,				\
		       sizeof(*unpacked) -				\
		       offsetof(struct bch_inode_unpacked, _name));	\
		return 0;						\
	}								\
									\
	ret = inode_decode_field(in, end, field, &field_bits);		\
	if (ret < 0)							\
		return ret;						\
									\
	if (field_bits > sizeof(unpacked->_name) * 8)			\
		return -1;						\
									\
	unpacked->_name = field[1];					\
	in += ret;

	BCH_INODE_FIELDS()
#undef  x

	/* XXX: signal if there were more fields than expected? */

	return 0;
}

struct btree_iter *bch2_inode_peek(struct btree_trans *trans,
				   struct bch_inode_unpacked *inode,
				   u64 inum, unsigned flags)
{
	struct btree_iter *iter;
	struct bkey_s_c k;
	int ret;

	iter = bch2_trans_get_iter(trans, BTREE_ID_INODES, POS(inum, 0),
				   BTREE_ITER_SLOTS|flags);
	if (IS_ERR(iter))
		return iter;

	k = bch2_btree_iter_peek_slot(iter);
	ret = bkey_err(k);
	if (ret)
		goto err;

	ret = k.k->type == KEY_TYPE_inode ? 0 : -EIO;
	if (ret)
		goto err;

	ret = bch2_inode_unpack(bkey_s_c_to_inode(k), inode);
	if (ret)
		goto err;

	return iter;
err:
	bch2_trans_iter_put(trans, iter);
	return ERR_PTR(ret);
}

int bch2_inode_write(struct btree_trans *trans,
		     struct btree_iter *iter,
		     struct bch_inode_unpacked *inode)
{
	struct bkey_inode_buf *inode_p;

	inode_p = bch2_trans_kmalloc(trans, sizeof(*inode_p));
	if (IS_ERR(inode_p))
		return PTR_ERR(inode_p);

	bch2_inode_pack(inode_p, inode);
	bch2_trans_update(trans, iter, &inode_p->inode.k_i);
	return 0;
}

const char *bch2_inode_invalid(const struct bch_fs *c, struct bkey_s_c k)
{
		struct bkey_s_c_inode inode = bkey_s_c_to_inode(k);
		struct bch_inode_unpacked unpacked;

	if (k.k->p.offset)
		return "nonzero offset";

	if (bkey_val_bytes(k.k) < sizeof(struct bch_inode))
		return "incorrect value size";

	if (k.k->p.inode < BLOCKDEV_INODE_MAX)
		return "fs inode in blockdev range";

	if (INODE_STR_HASH(inode.v) >= BCH_STR_HASH_NR)
		return "invalid str hash type";

	if (bch2_inode_unpack(inode, &unpacked))
		return "invalid variable length fields";

	if (unpacked.bi_data_checksum >= BCH_CSUM_OPT_NR + 1)
		return "invalid data checksum type";

	if (unpacked.bi_compression >= BCH_COMPRESSION_OPT_NR + 1)
		return "invalid data checksum type";

	if ((unpacked.bi_flags & BCH_INODE_UNLINKED) &&
	    unpacked.bi_nlink != 0)
		return "flagged as unlinked but bi_nlink != 0";

	return NULL;
}

void bch2_inode_to_text(struct printbuf *out, struct bch_fs *c,
		       struct bkey_s_c k)
{
	struct bkey_s_c_inode inode = bkey_s_c_to_inode(k);
	struct bch_inode_unpacked unpacked;

	if (bch2_inode_unpack(inode, &unpacked)) {
		pr_buf(out, "(unpack error)");
		return;
	}

#define x(_name, _bits)						\
	pr_buf(out, #_name ": %llu ", (u64) unpacked._name);
	BCH_INODE_FIELDS()
#undef  x
}

const char *bch2_inode_generation_invalid(const struct bch_fs *c,
					  struct bkey_s_c k)
{
	if (k.k->p.offset)
		return "nonzero offset";

	if (bkey_val_bytes(k.k) != sizeof(struct bch_inode_generation))
		return "incorrect value size";

	return NULL;
}

void bch2_inode_generation_to_text(struct printbuf *out, struct bch_fs *c,
				   struct bkey_s_c k)
{
	struct bkey_s_c_inode_generation gen = bkey_s_c_to_inode_generation(k);

	pr_buf(out, "generation: %u", le32_to_cpu(gen.v->bi_generation));
}

void bch2_inode_init_early(struct bch_fs *c,
			   struct bch_inode_unpacked *inode_u)
{
	enum bch_str_hash_type str_hash =
		bch2_str_hash_opt_to_type(c, c->opts.str_hash);

	memset(inode_u, 0, sizeof(*inode_u));

	/* ick */
	inode_u->bi_flags |= str_hash << INODE_STR_HASH_OFFSET;
	get_random_bytes(&inode_u->bi_hash_seed,
			 sizeof(inode_u->bi_hash_seed));
}

void bch2_inode_init_late(struct bch_inode_unpacked *inode_u, u64 now,
			  uid_t uid, gid_t gid, umode_t mode, dev_t rdev,
			  struct bch_inode_unpacked *parent)
{
	inode_u->bi_mode	= mode;
	inode_u->bi_uid		= uid;
	inode_u->bi_gid		= gid;
	inode_u->bi_dev		= rdev;
	inode_u->bi_atime	= now;
	inode_u->bi_mtime	= now;
	inode_u->bi_ctime	= now;
	inode_u->bi_otime	= now;

	if (parent && parent->bi_mode & S_ISGID) {
		inode_u->bi_gid = parent->bi_gid;
		if (S_ISDIR(mode))
			inode_u->bi_mode |= S_ISGID;
	}

	if (parent) {
#define x(_name, ...)	inode_u->bi_##_name = parent->bi_##_name;
		BCH_INODE_OPTS()
#undef x
	}
}

void bch2_inode_init(struct bch_fs *c, struct bch_inode_unpacked *inode_u,
		     uid_t uid, gid_t gid, umode_t mode, dev_t rdev,
		     struct bch_inode_unpacked *parent)
{
	bch2_inode_init_early(c, inode_u);
	bch2_inode_init_late(inode_u, bch2_current_time(c),
			     uid, gid, mode, rdev, parent);
}

static inline u32 bkey_generation(struct bkey_s_c k)
{
	switch (k.k->type) {
	case KEY_TYPE_inode:
		BUG();
	case KEY_TYPE_inode_generation:
		return le32_to_cpu(bkey_s_c_to_inode_generation(k).v->bi_generation);
	default:
		return 0;
	}
}

int bch2_inode_create(struct btree_trans *trans,
		      struct bch_inode_unpacked *inode_u,
		      u64 min, u64 max, u64 *hint)
{
	struct bch_fs *c = trans->c;
	struct bkey_inode_buf *inode_p;
	struct btree_iter *iter;
	u64 start;
	int ret;

	if (!max)
		max = ULLONG_MAX;

	if (c->opts.inodes_32bit)
		max = min_t(u64, max, U32_MAX);

	start = READ_ONCE(*hint);

	if (start >= max || start < min)
		start = min;

	inode_p = bch2_trans_kmalloc(trans, sizeof(*inode_p));
	if (IS_ERR(inode_p))
		return PTR_ERR(inode_p);

	iter = bch2_trans_get_iter(trans,
			BTREE_ID_INODES, POS(start, 0),
			BTREE_ITER_SLOTS|BTREE_ITER_INTENT);
	if (IS_ERR(iter))
		return PTR_ERR(iter);
again:
	while (1) {
		struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);

		ret = bkey_err(k);
		if (ret)
			return ret;

		switch (k.k->type) {
		case KEY_TYPE_inode:
			/* slot used */
			if (iter->pos.inode >= max)
				goto out;

			bch2_btree_iter_next_slot(iter);
			break;

		default:
			*hint			= k.k->p.inode;
			inode_u->bi_inum	= k.k->p.inode;
			inode_u->bi_generation	= bkey_generation(k);

			bch2_inode_pack(inode_p, inode_u);
			bch2_trans_update(trans, iter, &inode_p->inode.k_i);
			return 0;
		}
	}
out:
	if (start != min) {
		/* Retry from start */
		start = min;
		bch2_btree_iter_set_pos(iter, POS(start, 0));
		goto again;
	}

	return -ENOSPC;
}

int bch2_inode_rm(struct bch_fs *c, u64 inode_nr)
{
	struct btree_trans trans;
	struct btree_iter *iter;
	struct bkey_i_inode_generation delete;
	struct bpos start = POS(inode_nr, 0);
	struct bpos end = POS(inode_nr + 1, 0);
	int ret;

	/*
	 * If this was a directory, there shouldn't be any real dirents left -
	 * but there could be whiteouts (from hash collisions) that we should
	 * delete:
	 *
	 * XXX: the dirent could ideally would delete whiteouts when they're no
	 * longer needed
	 */
	ret   = bch2_btree_delete_range(c, BTREE_ID_EXTENTS,
					start, end, NULL) ?:
		bch2_btree_delete_range(c, BTREE_ID_XATTRS,
					start, end, NULL) ?:
		bch2_btree_delete_range(c, BTREE_ID_DIRENTS,
					start, end, NULL);
	if (ret)
		return ret;

	bch2_trans_init(&trans, c, 0, 0);

	iter = bch2_trans_get_iter(&trans, BTREE_ID_INODES, POS(inode_nr, 0),
				   BTREE_ITER_SLOTS|BTREE_ITER_INTENT);
	do {
		struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);
		u32 bi_generation = 0;

		ret = bkey_err(k);
		if (ret)
			break;

		bch2_fs_inconsistent_on(k.k->type != KEY_TYPE_inode, c,
					"inode %llu not found when deleting",
					inode_nr);

		switch (k.k->type) {
		case KEY_TYPE_inode: {
			struct bch_inode_unpacked inode_u;

			if (!bch2_inode_unpack(bkey_s_c_to_inode(k), &inode_u))
				bi_generation = inode_u.bi_generation + 1;
			break;
		}
		case KEY_TYPE_inode_generation: {
			struct bkey_s_c_inode_generation g =
				bkey_s_c_to_inode_generation(k);
			bi_generation = le32_to_cpu(g.v->bi_generation);
			break;
		}
		}

		if (!bi_generation) {
			bkey_init(&delete.k);
			delete.k.p.inode = inode_nr;
		} else {
			bkey_inode_generation_init(&delete.k_i);
			delete.k.p.inode = inode_nr;
			delete.v.bi_generation = cpu_to_le32(bi_generation);
		}

		bch2_trans_update(&trans, iter, &delete.k_i);

		ret = bch2_trans_commit(&trans, NULL, NULL,
					BTREE_INSERT_ATOMIC|
					BTREE_INSERT_NOFAIL);
	} while (ret == -EINTR);

	bch2_trans_exit(&trans);
	return ret;
}

int bch2_inode_find_by_inum_trans(struct btree_trans *trans, u64 inode_nr,
				  struct bch_inode_unpacked *inode)
{
	struct btree_iter *iter;
	struct bkey_s_c k;
	int ret;

	iter = bch2_trans_get_iter(trans, BTREE_ID_INODES,
			POS(inode_nr, 0), BTREE_ITER_SLOTS);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	k = bch2_btree_iter_peek_slot(iter);
	ret = bkey_err(k);
	if (ret)
		return ret;

	ret = k.k->type == KEY_TYPE_inode
		? bch2_inode_unpack(bkey_s_c_to_inode(k), inode)
		: -ENOENT;

	bch2_trans_iter_put(trans, iter);

	return ret;
}

int bch2_inode_find_by_inum(struct bch_fs *c, u64 inode_nr,
			    struct bch_inode_unpacked *inode)
{
	return bch2_trans_do(c, NULL, 0,
		bch2_inode_find_by_inum_trans(&trans, inode_nr, inode));
}

#ifdef CONFIG_BCACHEFS_DEBUG
void bch2_inode_pack_test(void)
{
	struct bch_inode_unpacked *u, test_inodes[] = {
		{
			.bi_atime	= U64_MAX,
			.bi_ctime	= U64_MAX,
			.bi_mtime	= U64_MAX,
			.bi_otime	= U64_MAX,
			.bi_size	= U64_MAX,
			.bi_sectors	= U64_MAX,
			.bi_uid		= U32_MAX,
			.bi_gid		= U32_MAX,
			.bi_nlink	= U32_MAX,
			.bi_generation	= U32_MAX,
			.bi_dev		= U32_MAX,
		},
	};

	for (u = test_inodes;
	     u < test_inodes + ARRAY_SIZE(test_inodes);
	     u++) {
		struct bkey_inode_buf p;

		bch2_inode_pack(&p, u);
	}
}
#endif
