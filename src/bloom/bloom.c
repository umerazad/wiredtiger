/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "bloom.h"

#define	WT_BLOOM_TABLE_CONFIG "key_format=r,value_format=1t,exclusive=true"

static int __bloom_init(
    WT_SESSION_IMPL *, const char *, const char *, WT_BLOOM **);
static int __bloom_setup(WT_BLOOM *, uint64_t, uint64_t, uint32_t, uint32_t);

static int
__bloom_init(WT_SESSION_IMPL *session,
    const char *uri, const char *config, WT_BLOOM **bloomp)
{
	WT_BLOOM *bloom;
	WT_DECL_RET;
	size_t len;

	bloom = NULL;
	WT_ERR(__wt_calloc(session, 1, sizeof(WT_BLOOM), &bloom));
	WT_ERR(__wt_strdup(session, uri, &bloom->uri));
	WT_ERR(__wt_strdup(session, config, &bloom->config));
	len = strlen(WT_BLOOM_TABLE_CONFIG) + 2;
	if (config != NULL)
		len += strlen(config);
	WT_ERR(__wt_calloc(session, len, sizeof(char), &bloom->config));
	/* Add the standard config at the end, so it overrides user settings. */
	(void)snprintf(bloom->config, len,
	    "%s,%s", config == NULL ? "" : config, WT_BLOOM_TABLE_CONFIG);

	bloom->session = session;

	*bloomp = bloom;
	return (0);

err:	if (bloom->uri != NULL)
		__wt_free(session, bloom->uri);
	if (bloom->config != NULL)
		__wt_free(session, bloom->uri);
	if (bloom->bitstring != NULL)
		__wt_free(session, bloom->bitstring);
	if (bloom != NULL)
		__wt_free(session, bloom);
	return (ret);
}

/*
 * Populate the bloom structure.
 * Setup is passed in either the count of items expected (n), or the length
 * of the bitstring (m). Depends on whether the function is called via create
 * or open.
 */
static int
__bloom_setup(
    WT_BLOOM *bloom, uint64_t n, uint64_t m, uint32_t factor, uint32_t k)
{
	WT_ASSERT(bloom->session, k > 1);

	bloom->k = k;
	bloom->factor = factor;
	if (n != 0) {
		bloom->n = n;
		bloom->m = bloom->n * bloom->factor;
	} else {
		bloom->m = m;
		bloom->n = bloom->m / bloom->factor;
	}
	return (0);
}

/*
 * __wt_bloom_create --
 *
 * Creates and configures a WT_BLOOM handle, allocates a bitstring in memory
 * to use while populating the bloom filter.
 *
 * count  - is the expected number of inserted items
 * factor - is the number of bits to use per inserted item
 * k      - is the number of hash values to set or test per item
 */
int
__wt_bloom_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config,
    uint64_t count, uint32_t factor, uint32_t k, WT_BLOOM **bloomp)
{
	WT_BLOOM *bloom;
	WT_RET(__bloom_init(session, uri, config, &bloom));
	WT_RET(__bloom_setup(bloom, count, 0, factor, k));

	WT_RET(__bit_alloc(session, bloom->m, &bloom->bitstring));

	*bloomp = bloom;
	return (0);
}

/*
 * __wt_bloom_open --
 * Open a Bloom filter object for use by a single session. The filter must have
 * been created and finalized.
 */
int
__wt_bloom_open(WT_SESSION_IMPL *session,
    const char *uri, uint32_t factor, uint32_t k, WT_BLOOM **bloomp)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	WT_SESSION *wt_session;
	uint64_t size;

	wt_session = (WT_SESSION *)session;

	WT_RET(__bloom_init(session, uri, NULL, &bloom));

	/* Find the largest key, to get the size of the filter. */
	WT_RET(wt_session->open_cursor(wt_session, bloom->uri, NULL, NULL, &c));
	WT_RET(c->prev(c));
	WT_RET(c->get_key(c, &size));
	WT_RET(c->close(c));

	WT_RET(__bloom_setup(bloom, 0, size, factor, k));

	*bloomp = bloom;
	return (0);
}

/*
 * __wt_bloom_insert --
 * Adds the given key to the Bloom filter.
 */
int
__wt_bloom_insert(WT_BLOOM *bloom, WT_ITEM *key)
{
	uint64_t h1, h2;
	uint32_t i;

	h1 = __wt_hash_fnv64(key->data, key->size);
	h2 = __wt_hash_city64(key->data, key->size);
	for (i = 0; i < bloom->k; i++, h1 += h2) {
		__bit_set(bloom->bitstring, (uint32_t)(h1 % bloom->m));
	}
	return (0);
}

/*
 * __wt_bloom_finalize --
 * Writes the Bloom filter to stable storage. After calling finalize, only
 * read operations can be performed on the bloom filter.
 */
int
__wt_bloom_finalize(WT_BLOOM *bloom)
{
	WT_SESSION *wt_session;
	WT_CURSOR *c;
	uint64_t i;

	wt_session = (WT_SESSION *)bloom->session;

	/*
	 * Create a bit table to store the bloom filter in.
	 * TODO: should this call __wt_schema_create directly?
	 */
	WT_RET(wt_session->create(wt_session, bloom->uri, bloom->config));

	WT_RET(wt_session->open_cursor(
	    wt_session, bloom->uri, NULL, "bulk", &c));
	/* Add the entries from the array into the table. */
	for (i = 0; i < bloom->m; i++) {
		c->set_value(c, __bit_test(bloom->bitstring, i));
		WT_RET(c->insert(c));
	}
	WT_RET(c->close(c));
	__wt_free(bloom->session, bloom->bitstring);
	bloom->bitstring = NULL;

	return (0);
}

/*
 * __wt_bloom_get --
 * Tests whether the given key is in the Bloom filter.
 * Returns zero if found, WT_NOTFOUND if not.
 */
int
__wt_bloom_get(WT_BLOOM *bloom, WT_ITEM *key)
{
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	uint32_t i;
	uint64_t h1, h2;
	uint8_t bit;

	WT_ASSERT(bloom->session, bloom->bitstring == NULL);

	wt_session = (WT_SESSION *)bloom->session;

	/* Create a cursor on the first time through. */
	if (bloom->c == NULL) {
		WT_RET(wt_session->open_cursor(
		    wt_session, bloom->uri, NULL, NULL, &c));
		bloom->c = c;
	} else
		c = bloom->c;

	/*
	 * This comparison code is complex to avoid calculating the second
	 * hash if possible.
	 */
	h1 = __wt_hash_fnv64(key->data, key->size);

	/*
	 * Add 1 to the hash because Wired Tiger tables are 1 based, and the
	 * original bitstring array was 0 based.
	 */
	c->set_key(c, (h1 % bloom->m) + 1);
	WT_RET(c->search(c));
	WT_RET(c->get_value(c, &bit));
	if (bit == 0)
		return (WT_NOTFOUND);
	h2 = __wt_hash_city64(key->data, key->size);
	for (i = 0, h1 += h2; i < bloom->k - 1; i++, h1 += h2) {
		c->set_key(c, (h1 % bloom->m) + 1);
		WT_ERR(c->search(c));
		WT_ERR(c->get_value(c, &bit));

		if (bit == 0)
			return (WT_NOTFOUND);
	}
	return (0);

err:	/* Don't return WT_NOTFOUND from a failed search. */
	if (ret == WT_NOTFOUND)
		ret = WT_ERROR;
	__wt_err(bloom->session, ret, "Failed lookup in bloom filter.");
	return (ret);
}

/*
 * __wt_bloom_close --
 * Close the Bloom filter, release any resources.
 */
int
__wt_bloom_close(WT_BLOOM *bloom)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = bloom->session;

	if (bloom->c != NULL)
		ret = bloom->c->close(bloom->c);
	__wt_free(session, bloom->uri);
	__wt_free(session, bloom->config);
	__wt_free(session, bloom->bitstring);
	__wt_free(session, bloom);

	return (ret);
}

/*
 * __wt_bloom_drop --
 * Drop a Bloom filter, release any resources.
 */
int
__wt_bloom_drop(WT_BLOOM *bloom, const char *config)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;

	wt_session = (WT_SESSION *)bloom->session;
	if (bloom->c != NULL) {
		ret = bloom->c->close(bloom->c);
		bloom->c = NULL;
	}
	WT_TRET(wt_session->drop(wt_session, bloom->uri, config));
	WT_TRET(__wt_bloom_close(bloom));

	return (ret);
}