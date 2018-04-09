/*
 * Copyright (C) 2011 Google, Inc
 * Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/msm_ion.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <soc/qcom/scm.h>


DEFINE_MUTEX(secure_buffer_mutex);

struct cp2_mem_chunks {
	unsigned int *chunk_list;
	unsigned int chunk_list_size;
	unsigned int chunk_size;
} __attribute__ ((__packed__));

struct cp2_lock_req {
	struct cp2_mem_chunks chunks;
	unsigned int mem_usage;
	unsigned int lock;
} __attribute__ ((__packed__));

#define MEM_PROTECT_LOCK_ID2     0x0A
#define V2_CHUNK_SIZE		SZ_1M
#define FEATURE_ID_CP 12

static int secure_buffer_change_chunk(u32 chunks,
				u32 nchunks,
				u32 chunk_size,
				int lock)
{
	struct cp2_lock_req request;
	u32 resp;

	request.chunks.chunk_list = (unsigned int *)chunks;
	request.chunks.chunk_list_size = nchunks;
	request.chunks.chunk_size = chunk_size;
	/* Usage is now always 0 */
	request.mem_usage = 0;
	request.lock = lock;

	kmap_flush_unused();
	kmap_atomic_flush_unused();
	return scm_call(SCM_SVC_MP, MEM_PROTECT_LOCK_ID2,
			&request, sizeof(request), &resp, sizeof(resp));

}



static int secure_buffer_change_table(struct sg_table *table, int lock)
{
	int i;
	int ret = -EINVAL;
	unsigned long *chunk_list;
	struct scatterlist *sg;

	for_each_sg(table->sgl, sg, table->nents, i) {
		int nchunks;
		int size = sg_dma_len(sg);
		int chunk_list_len;
		phys_addr_t chunk_list_phys;

		/*
		 * This should theoretically be a phys_addr_t but the protocol
		 * indicates this should be an unsigned long.
		 */
		unsigned long base = (unsigned long)sg_dma_address(sg);

		if (unlikely(!size || (size % V2_CHUNK_SIZE))) {
			WARN(1,
				"%s: chunk %d has invalid size: 0x%x. Must be a multiple of 0x%x\n",
				 __func__, i, size, V2_CHUNK_SIZE);
			return -EINVAL;
		}

		nchunks = size / V2_CHUNK_SIZE;
		chunk_list_len = sizeof(unsigned long)*nchunks;

		chunk_list = kzalloc(chunk_list_len, GFP_KERNEL);

		if (!chunk_list)
			return -ENOMEM;

		chunk_list_phys = virt_to_phys(chunk_list);
		for (i = 0; i < nchunks; i++)
			chunk_list[i] = base + i * V2_CHUNK_SIZE;

		/*
		 * Flush the chunk list before sending the memory to the
		 * secure environment to ensure the data is actually present
		 * in RAM
		 */
		dmac_flush_range(chunk_list, chunk_list + chunk_list_len);

		ret = secure_buffer_change_chunk(virt_to_phys(chunk_list),
				nchunks, V2_CHUNK_SIZE, lock);

		if (!ret) {
			/*
			 * Set or clear the private page flag to communicate the
			 * status of the chunk to other entities
			 */
			if (lock)
				SetPagePrivate(sg_page(sg));
			else
				ClearPagePrivate(sg_page(sg));
		}

		kfree(chunk_list);
	}

	return ret;
}

int msm_ion_secure_table(struct sg_table *table)
{
	int ret;

	mutex_lock(&secure_buffer_mutex);
	ret = secure_buffer_change_table(table, 1);
	mutex_unlock(&secure_buffer_mutex);

	return ret;

}

int msm_ion_unsecure_table(struct sg_table *table)
{
	int ret;

	mutex_lock(&secure_buffer_mutex);
	ret = secure_buffer_change_table(table, 0);
	mutex_unlock(&secure_buffer_mutex);
	return ret;

}

#define MAKE_CP_VERSION(major, minor, patch) \
	(((major & 0x3FF) << 22) | ((minor & 0x3FF) << 12) | (patch & 0xFFF))

bool msm_secure_v2_is_supported(void)
{
	int version = scm_get_feat_version(FEATURE_ID_CP);

	/*
	 * if the version is < 1.1.0 then dynamic buffer allocation is
	 * not supported
	 */
	return version >= MAKE_CP_VERSION(1, 1, 0);
}
