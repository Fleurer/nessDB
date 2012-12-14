/*
 * Copyright (c) 2012, BohuTANG <overred.shuttler at gmail dot com>
 * All rights reserved.
 * Code is licensed with GPL. See COPYING.GPL file.
 *
 * The main algorithm described is here: spec/sst-algorithms.txt
 */

#include "index.h"
#include "sst.h"
#include "hashs.h"
#include "debug.h"
#include "xmalloc.h"

#define DB_MAGIC (20121212)

void *_merge_job(void *arg)
{
	struct index *idx;

	idx = (struct index*)arg;
	(void)idx;

	return NULL;
}


char *index_read_data(struct index *idx, struct ol_pair *pair)
{
	int res;
	char *data = NULL;

	if (pair->offset > 0UL && pair->vlen > 0) {
		char iscompress = 0;
		short crc = 0;
		short db_crc = 0;
		char *dest = NULL;

		n_lseek(idx->read_fd, pair->offset, SEEK_SET);

		/* read compress flag */
		res = read(idx->read_fd, &iscompress, sizeof(char));
		if (res == -1) {
			__ERROR("read iscompress flag error");

			goto RET;
		}

		/* read crc flag */
		res = read(idx->read_fd, &crc, sizeof(crc));
		if (res == -1) {
			__ERROR("read crc error #%d", 
					crc);

			goto RET;
		}

		/* read data */
		data = xcalloc(1, pair->vlen + 1);
		res = read(idx->read_fd, data, pair->vlen);
		if (res == -1) {
			__ERROR("read data error");

			goto RET;
		}

		/* decompressed */
		if (iscompress) {
			int vsize = qlz_size_decompressed(data);

			dest = xcalloc(1, vsize);
			pair->vlen = qlz_decompress(data, dest, &idx->destate);
			xfree(data);

			data = dest;
		} 

		db_crc = _crc16(data, pair->vlen);
		if (crc != db_crc) {
			idx->stats->STATS_CRC_ERRS++;
			__ERROR("read data crc#%d, db_crc#%d, data [%s]", 
					crc, 
					db_crc, 
					data);

			goto RET;
		}
	}

RET:
	return data;
}

struct index *index_new(const char *path, int mtb_size, struct stats *stats)
{
	int magic;
	char db_name[NESSDB_PATH_SIZE];
	char tow_name[NESSDB_PATH_SIZE];
	struct index *idx = xcalloc(1, sizeof(struct index));

	idx->stats = stats;
	idx->meta = meta_new(path, stats);
	idx->buf = buffer_new(NESSDB_MAX_VAL_SIZE);

	memset(db_name, 0, NESSDB_PATH_SIZE);
	snprintf(db_name, NESSDB_PATH_SIZE, "%s/%s", path, NESSDB_DB);
	memset(tow_name, 0, NESSDB_PATH_SIZE);
	snprintf(tow_name, NESSDB_PATH_SIZE, "%s/tower.SST", path);

	idx->fd = n_open(db_name, N_OPEN_FLAGS, 0644);
	if (idx->fd == -1) {
		idx->fd = n_open(db_name, N_CREAT_FLAGS, 0644);
		if (idx->fd == -1) 
			__PANIC("db error, name#%s", 
					db_name);

		magic = DB_MAGIC;
		if (write(idx->fd, &magic, sizeof(magic)) < 0)
			__PANIC("write db magic error");
		idx->db_alloc += sizeof(magic);
	}

	idx->read_fd = n_open(db_name, N_OPEN_FLAGS);
	idx->db_alloc = n_lseek(idx->fd, 0, SEEK_END);
	idx->max_mtb_size = mtb_size;
	memset(&idx->enstate, 0, sizeof(qlz_state_compress));
	memset(&idx->destate, 0, sizeof(qlz_state_decompress));

	idx->sst = sst_new(tow_name, idx->meta->cpt, idx->stats);

	return idx;
}

int index_add(struct index *idx, struct slice *sk, struct slice *sv)
{
	int ret;
	int buff_len;
	int val_len;
	uint64_t offset;
	uint64_t cpt_offset = 0;

	char *line;
	struct sst_item item;

	if (sk->len >= NESSDB_MAX_KEY_SIZE || (sv && sv->len > NESSDB_MAX_VAL_SIZE)) {
		__ERROR("key or value is too long...#%d:%d", 
				NESSDB_MAX_KEY_SIZE, 
				NESSDB_MAX_VAL_SIZE);

		return 0;
	}

	offset = idx->db_alloc;
	memset(&item, 0, ITEM_SIZE);
	memcpy(item.data, sk->data, sk->len);
	if (sv) {
		idx->stats->STATS_WRITES++;

		val_len = sv->len;
		/* compressed */
		if (sv->len >= NESSDB_COMPRESS_LIMIT) {
			char *dest = xcalloc(1, sv->len + 400);
			int qsize = qlz_compress(sv->data, dest, sv->len, &idx->enstate);

			buffer_putc(idx->buf, COMPRESS);
			buffer_putshort(idx->buf, _crc16(dest, qsize));
			buffer_putnstr(idx->buf, dest, qsize);
			val_len = qsize;
			xfree(dest);
			idx->stats->STATS_COMPRESSES++;
		} else {
			buffer_putc(idx->buf, UNCOMPRESS);
			buffer_putshort(idx->buf, _crc16(sv->data, sv->len));
			buffer_putnstr(idx->buf, sv->data, sv->len);
			val_len = sv->len;
		}

		buff_len = idx->buf->NUL;
		line = buffer_detach(idx->buf);

		/* too many delete holes, we need to use them */
		if (idx->meta->cpt->count > NESSDB_COMPACT_LIMIT) {
			cpt_offset = cpt_get(idx->meta->cpt, val_len);
			offset = cpt_offset;
			idx->stats->STATS_HOLE_REUSES++;
		}

		if (cpt_offset > 0) {
			ret = pwrite(idx->fd, line, buff_len, cpt_offset);
		} else {
			ret = write(idx->fd, line, buff_len);
			if (ret == -1) 
				__PANIC("write db error");

			idx->db_alloc += buff_len;
		}

		item.offset = offset;
		item.vlen = val_len;
		item.opt = (sv==NULL?DEL:ADD);
	}

	if (sst_level_isfull(idx->sst, MAX_LEVEL - 1)) {
		sst_dump(idx->sst);
		__PANIC("...............last full");
		//todo create new thread to flush
	} else
		sst_add(idx->sst, &item);
			

	return 1;
}

int index_get(struct index *idx, struct slice *sk, struct slice *sv) 
{
	struct ol_pair pair;
	struct meta_node *node;

	if (sk->len >= NESSDB_MAX_KEY_SIZE) {
		__ERROR("key length big than MAX#%d", 
				NESSDB_MAX_KEY_SIZE);

		return 0;
	}

	idx->stats->STATS_READS++;
	memset(&pair, 0, sizeof pair);

	node =  meta_get(idx->meta, sk->data);
	if (node) {
		if (bloom_get(node->sst->bf, sk->data)) {
			idx->stats->STATS_R_BF++;
		} else {
			idx->stats->STATS_R_NOTIN_BF++;
			goto RET;
		}

		if (sst_get(node->sst, sk, &pair)) {
			idx->stats->STATS_R_COLA++;
		} else {
			idx->stats->STATS_R_NOTIN_COLA++;
			goto RET;
		}
	}

	char *data = index_read_data(idx, &pair);
	if (data) {
		sv->data = data;
		sv->len = pair.vlen;

		return 1;
	}

RET:
	return 0;
}

int index_remove(struct index *idx, struct slice *sk)
{
	if (sk->len >= NESSDB_MAX_KEY_SIZE) {
		__ERROR("key length big than MAX#%d", 
				NESSDB_MAX_KEY_SIZE);

		return 0;
	}

	idx->stats->STATS_REMOVES++;
	return index_add(idx, sk, NULL);
}

void index_free(struct index *idx)
{
	meta_free(idx->meta);
	buffer_free(idx->buf);
	xfree(idx);
}
