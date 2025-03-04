#ifndef SRC_MOD_NAT64_POOL4_DB_H_
#define SRC_MOD_NAT64_POOL4_DB_H_

/*
 * @file
 * The pool of IPv4 addresses. Stateful NAT64 Jool uses this to figure out
 * which packets should be translated.
 */

#include <linux/net.h>
#include "mod/common/types.h"
#include "mod/common/config.h"
#include "mod/common/route.h"

struct pool4;

/*
 * Write functions (Caller must prevent concurrence)
 */

struct pool4 *pool4db_alloc(void);
void pool4db_get(struct pool4 *pool);
void pool4db_put(struct pool4 *pool);

int pool4db_add(struct pool4 *pool, const struct pool4_entry_usr *entry);
int pool4db_update(struct pool4 *pool, const struct pool4_update *update);
int pool4db_rm(struct pool4 *pool, const __u32 mark, enum l4_protocol proto,
		struct ipv4_range *range);
int pool4db_rm_usr(struct pool4 *pool, struct pool4_entry_usr *entry);
void pool4db_flush(struct pool4 *pool);

/*
 * Read functions (Legal to use anywhere)
 */

bool pool4db_contains(struct pool4 *pool, struct net *ns,
		enum l4_protocol proto, struct ipv4_transport_addr *addr);

typedef int (*pool4db_foreach_sample_cb)(struct pool4_sample const *, void *);
int pool4db_foreach_sample(struct pool4 *pool, l4_protocol proto,
		pool4db_foreach_sample_cb cb, void *arg,
		struct pool4_sample *offset);

struct mask_domain;

struct mask_domain *mask_domain_find(struct pool4 *pool, struct tuple *tuple6,
		__u8 f_args, struct route4_args *route_args);
void mask_domain_put(struct mask_domain *masks);
int mask_domain_next(struct mask_domain *masks,
		struct ipv4_transport_addr *addr,
		bool *consecutive);
void mask_domain_commit(struct mask_domain *masks);
bool mask_domain_matches(struct mask_domain *masks,
		struct ipv4_transport_addr *addr);
bool mask_domain_is_dynamic(struct mask_domain *masks);
__u32 mask_domain_get_mark(struct mask_domain *masks);

/*
 * Test functions (Illegal in production code)
 */
void pool4db_print(struct pool4 *pool);

#endif /* SRC_MOD_NAT64_POOL4_DB_H_ */
