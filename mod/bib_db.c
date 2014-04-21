#include "nat64/mod/bib_db.h"

#include <net/ipv6.h>
#include "nat64/mod/rbtree.h"
#include "nat64/mod/bib.h"
#include "nat64/mod/pool4.h"
#include "nat64/mod/packet.h"
#include "nat64/mod/icmp_wrapper.h"

/** The BIB table for UDP connections. */
static struct bib_table bib_udp;
/** The BIB table for TCP connections. */
static struct bib_table bib_tcp;
/** The BIB table for ICMP connections. */
static struct bib_table bib_icmp;

/**
 * BIB table definition.
 * Holds two hash tables, one for each indexing need (IPv4 and IPv6).
 */
struct bib_table {
	/** Indexes the entries using their IPv6 identifiers. */
	struct rb_root tree6;
	/** Indexes the entries using their IPv4 identifiers. */
	struct rb_root tree4;
	/* Number of BIB entries in this table. */
	u64 count;
	/* spinlock for the table */
	spinlock_t bib_table_lock;
};

/********************************************
 * Private (helper) functions.
 ********************************************/

static int get_bibdb_table(l4_protocol l4_proto, struct bib_table **result)
{
	switch (l4_proto) {
	case L4PROTO_UDP:
		*result = &bib_udp;
		return 0;
	case L4PROTO_TCP:
		*result = &bib_tcp;
		return 0;
	case L4PROTO_ICMP:
		*result = &bib_icmp;
		return 0;
	case L4PROTO_NONE:
		log_crit(ERR_ILLEGAL_NONE, "There's no BIB for the 'NONE' protocol.");
		return -EINVAL;
	}

	log_crit(ERR_L4PROTO, "Unsupported transport protocol: %u.", l4_proto);
	return -EINVAL;
}

static int compare_addr6(struct bib_entry *bib, struct in6_addr *addr)
{
	return ipv6_addr_cmp(&bib->ipv6.address, addr);
}

static int compare_full6(struct bib_entry *bib, struct ipv6_tuple_address *addr)
{
	int gap;

	gap = compare_addr6(bib, &addr->address);
	if (gap != 0)
		return gap;

	gap = bib->ipv6.l4_id - addr->l4_id;
	return gap;
}

static int compare_full4(struct bib_entry *bib, struct ipv4_tuple_address *addr)
{
	int gap;

	gap = ipv4_addr_cmp(&bib->ipv4.address, &addr->address);
	if (gap != 0)
		return gap;

	gap = bib->ipv4.l4_id - addr->l4_id;
	return gap;
}


struct iteration_args {
	struct tuple *tuple;
	struct ipv4_tuple_address *result;
};

/**
 * Evaluates "bib", and returns whether it is a perfect match to "void_args"'s tuple.
 *
 * See allocate_ipv4_transport_address().
 */
static int find_perfect_addr4(struct bib_entry *bib, void *void_args)
{
	struct iteration_args *args = void_args;
	struct ipv4_tuple_address tuple_addr;
	int error;

	tuple_addr.address = bib->ipv4.address;
	tuple_addr.l4_id = args->tuple->src.l4_id;

	error = pool4_get_match(args->tuple->l4_proto, &tuple_addr, &args->result->l4_id);
	if (error)
		return 0; /* Not a satisfactory match; keep looking.*/

	args->result->address = bib->ipv4.address;
	return 1; /* Found a match; break the iteration with a no-error (but still non-zero) status. */
}

/**
 * Evaluates "bib", and returns whether it is an acceptable match to "void_args"'s tuple.
 *
 * See allocate_ipv4_transport_address().
 */
static int find_runnerup_addr4(struct bib_entry *bib, void *void_args)
{
	struct iteration_args *args = void_args;
	int error;

	error = pool4_get_any_port(args->tuple->l4_proto, &bib->ipv4.address, &args->result->l4_id);
	if (error)
		return 0; /* Not a satisfactory match; keep looking.*/

	args->result->address = bib->ipv4.address;
	return 1; /* Found a match; break the iteration with a no-error (but still non-zero) status. */
}

static int for_each_bib_ipv6(struct bib_table *table, l4_protocol l4_proto, struct in6_addr *addr,
		int (*func)(struct bib_entry *, void *), void *arg)
{
	struct bib_entry *bib;
	struct rb_node *node;
	int error;
	bool found;

	/* Sanitize */
	if (!addr)
		return -EINVAL;

	/* Find the top-most node in the tree whose IPv6 address is addr. */
	bib = rbtree_find(addr, &table->tree6, compare_addr6, struct bib_entry, tree6_hook);
	if (!bib)
		return 0; /* _Successfully_ iterated through no entries. */

	/* Keep moving left until we find the first node whose IPv6 address is addr. */
	found = false;
	do {
		node = rb_prev(&bib->tree6_hook);

		if (node) {
			struct bib_entry *tmp = rb_entry(node, struct bib_entry, tree6_hook);
			if (compare_addr6(tmp, addr))
				found = true;
			else
				bib = tmp;
		} else {
			found = true;
		}
	} while (!found);

	/*
	 * Keep moving right until the address changes.
	 * (The nodes are sorted by address first.)
	 */
	do {
		error = func(bib, arg);
		if (error)
			return error;

		node = rb_next(&bib->tree6_hook);
		if (!node)
			break;

		bib = rb_entry(node, struct bib_entry, tree6_hook);
	} while (ipv6_addr_equals(addr, &bib->ipv6.address));

	return 0;
}

/**
 * "Allocates" from the IPv4 pool a new transport address. Attemps to make this address as similar
 * to "tuple"'s contents as possible.
 *
 * Sorry, we're using the term "allocate" because the RFC does. A more appropriate name in this
 * context would be "borrow (from the IPv4 pool)".
 *
 * RFC6146 - Sections 3.5.1.1 and 3.5.2.3.
 *
 * @param[in] The table to iterate through.
 * @param[in] base this should contain the IPv6 source address you want the IPv4 address for.
 * @param[out] result the transport address we borrowed from the pool.
 * @return true if everything went OK, false otherwise.
 */
static int allocate_ipv4_transport_address(struct bib_table *table, struct tuple *base, struct ipv4_tuple_address *result)
{
	int error;
	struct iteration_args args = {
			.tuple = base,
			.result = result
	};

	/* First, try to find a perfect match (Same address and a compatible port or id). */
	error = for_each_bib_ipv6(table, base->l4_proto, &base->src.addr.ipv6, find_perfect_addr4, &args);
	if (error < 0)
		return error; /* Something failed, report.*/
	else if (error > 0)
		return 0; /* A match was found and "result" is already populated, so report success. */

	/*
	 * Else, iteration ended with no perfect match. Find a good match instead...
	 * (good match = same address, any port or id)
	 */
	error = for_each_bib_ipv6(table, base->l4_proto, &base->src.addr.ipv6, find_runnerup_addr4, &args);
	if (error < 0)
		return error;
	else if (error > 0)
		return 0;

	/* There are no good matches. Just use any available IPv4 address and hope for the best. */
	return pool4_get_any_addr(base->l4_proto, base->src.l4_id, result);
}

/*******************************
 * Public functions.
 *******************************/

int bibdb_init(void)
{
	struct bib_table *tables[] = { &bib_udp, &bib_tcp, &bib_icmp };
	int i;

	if (bib_init()) {
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(tables); i++) {
		tables[i]->tree6 = RB_ROOT;
		tables[i]->tree4 = RB_ROOT;
		tables[i]->count = 0;
		spin_lock_init(&tables[i]->bib_table_lock);
	}

	return 0;
}

int bibdb_get(struct tuple *tuple, struct bib_entry **result)
{
	struct ipv6_tuple_address addr6;
	struct ipv4_tuple_address addr4;

	if (!tuple) {
		log_err(ERR_NULL, "There's no BIB entry mapped to NULL.");
		return -EINVAL;
	}

	switch (tuple->l3_proto) {
	case L3PROTO_IPV6:
		addr6.address = tuple->src.addr.ipv6;
		addr6.l4_id = tuple->src.l4_id;
		return bibdb_get_by_ipv6(&addr6, tuple->l4_proto, result);
	case L3PROTO_IPV4:
		addr4.address = tuple->dst.addr.ipv4;
		addr4.l4_id = tuple->dst.l4_id;
		return bibdb_get_by_ipv4(&addr4, tuple->l4_proto, result);
	}

	log_crit(ERR_L3PROTO, "Unsupported network protocol: %u.", tuple->l3_proto);
	return -EINVAL;
}

static void bibdb_destroy_aux(struct rb_node *node)
{
	bib_kfree(rb_entry(node, struct bib_entry, tree6_hook));
}

void bibdb_destroy(void)
{
	struct bib_table *tables[] = { &bib_udp, &bib_tcp, &bib_icmp };
	int i;

	log_debug("Emptying the BIB tables...");
	/*
	 * The values need to be released only in one of the trees
	 * because both tables point to the same values.
	 */

	for (i = 0; i < ARRAY_SIZE(tables); i++)
		rbtree_clear(&tables[i]->tree6, bibdb_destroy_aux);

	bib_destroy();
}

int bibdb_get_by_ipv4(struct ipv4_tuple_address *addr, l4_protocol l4_proto,
		struct bib_entry **result)
{
	struct bib_table *table;
	int error;

	/* Sanitize */
	if (!addr) {
		log_warning("The BIBs cannot contain NULL.");
		return -EINVAL;
	}
	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	/* Find it */
	spin_lock_bh(&table->bib_table_lock);
	*result = rbtree_find(addr, &table->tree4, compare_full4, struct bib_entry, tree4_hook);

	if (*result)
		bib_get(*result);

	spin_unlock_bh(&table->bib_table_lock);

	return (*result) ? 0 : -ENOENT;
}

int bibdb_get_by_ipv6(struct ipv6_tuple_address *addr, l4_protocol l4_proto,
		struct bib_entry **result)
{
	struct bib_table *table;
	int error;

	/* Sanitize */
	if (!addr) {
		log_warning("The BIBs cannot contain NULL.");
		return -EINVAL;
	}
	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	/* Find it */
	spin_lock_bh(&table->bib_table_lock);
	*result = rbtree_find(addr, &table->tree6, compare_full6, struct bib_entry, tree6_hook);

	if (*result)
		bib_get(*result);

	spin_unlock_bh(&table->bib_table_lock);

	return (*result) ? 0 : -ENOENT;
}

int bibdb_add(struct bib_entry *entry, l4_protocol l4_proto)
{
	struct bib_table *table;
	int error;

	/* Sanity */
	if (!entry) {
		log_err(ERR_NULL, "NULL is not a valid BIB entry.");
		return -EINVAL;
	}
	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	/* Index */
	spin_lock_bh(&table->bib_table_lock);
	error = rbtree_add(entry, ipv6, &table->tree6, compare_full6, struct bib_entry, tree6_hook);
	if (error) {
		spin_unlock_bh(&table->bib_table_lock);
		return error;
	}

	error = rbtree_add(entry, ipv4, &table->tree4, compare_full4, struct bib_entry, tree4_hook);
	if (error) { /*this is not supposed to happen in a perfect world*/
		log_crit(ERR_ADD_SESSION_FAILED, "The in_bib was inserted in BIB_table_tree6 but exist in BIB_table_tree4");
		rb_erase(&entry->tree6_hook, &table->tree6);
		spin_unlock_bh(&table->bib_table_lock);
		return error;
	}

	table->count++;
	spin_unlock_bh(&table->bib_table_lock);
	return 0;
}

int bibdb_remove(struct bib_entry *entry, l4_protocol l4_proto)
{
	struct bib_table *table;
	int error;

	if (!entry) {
		log_err(ERR_NULL, "The BIB tables do not contain NULL entries.");
		return -EINVAL;
	}
	if (RB_EMPTY_NODE(&entry->tree6_hook) || RB_EMPTY_NODE(&entry->tree4_hook)) {
		log_err(ERR_BIB_NOT_FOUND, "BIB entry does not belong to any trees.");
		return -EINVAL;
	}
	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->bib_table_lock);

	rb_erase(&entry->tree6_hook, &table->tree6);
	rb_erase(&entry->tree4_hook, &table->tree4);

	table->count--;

	log_debug("BIB removed from the BD");
	spin_unlock_bh(&table->bib_table_lock);
	return 0;
}


int bibdb_for_each(l4_protocol l4_proto, int (*func)(struct bib_entry *, void *), void *arg)
{
	struct bib_table *table;
	struct rb_node *node;
	int error;

	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->bib_table_lock);

	for (node = rb_first(&table->tree4); node; node = rb_next(node)) {
		error = func(rb_entry(node, struct bib_entry, tree4_hook), arg);
		if (error) {
			spin_unlock_bh(&table->bib_table_lock);
			return error;
		}
	}

	spin_unlock_bh(&table->bib_table_lock);
	return 0;
}


int bibdb_count(l4_protocol proto, u64 *result)
{
	struct bib_table *table;
	int error;

	error = get_bibdb_table(proto, &table);
	if (error)
		return error;

	spin_lock_bh(&table->bib_table_lock);
	*result = table->count;
	spin_unlock_bh(&table->bib_table_lock);
	return 0;
}
/**
 *
 */
int bibdb_get_or_create_ipv6(struct fragment *frag, struct tuple *tuple, struct bib_entry **bib)
{
	struct ipv6_tuple_address addr6;
	struct ipv4_tuple_address addr4;
	struct rb_node **node, *parent;
	struct bib_table *table;
	int error;

	addr6.address = tuple->src.addr.ipv6;
	addr6.l4_id = tuple->src.l4_id;

	/* Sanitize */
	if (!tuple) {
		log_warning("The BIBs cannot contain NULL.");
		return -EINVAL;
	}

	error = get_bibdb_table(tuple->l4_proto, &table);
	if (error)
		return error;

	/* Find it */
	spin_lock_bh(&table->bib_table_lock);

	error = rbtree_find_node(&addr6, &table->tree6, compare_full6, struct bib_entry, tree6_hook, parent, node);
	if (*node) {
		*bib = rb_entry(*node, struct bib_entry, tree6_hook);
		bib_get(*bib);
		spin_unlock_bh(&table->bib_table_lock);
		return 0;
	}
	/*doesn't found a BIB, so we will create one BIB*/
	error = allocate_ipv4_transport_address(table, tuple, &addr4);
	if (error) {
		log_warning("Error code %d while 'allocating' an address for a BIB entry.", error);
		if (tuple->l4_proto != L4PROTO_ICMP) {
			/* I don't know why this is not supposed to happen with ICMP, but the RFC says so... */
			icmp64_send(frag, ICMPERR_ADDR_UNREACHABLE, 0);
		}
		spin_unlock_bh(&table->bib_table_lock);
		return error;
	}

	*bib = bib_create(&addr4, &addr6, false, tuple->l4_proto);
	if (!(*bib)) {
		log_err(ERR_ALLOC_FAILED, "Failed to allocate a BIB entry.");
		spin_unlock_bh(&table->bib_table_lock);
		return -ENOMEM;
	}

	/* add a new node and rebalance the tree */
	rb_link_node(&(*bib)->tree6_hook, parent, node);
	rb_insert_color(&(*bib)->tree6_hook, &table->tree6);

	error = rbtree_add(*bib, ipv4, &table->tree4, compare_full4, struct bib_entry, tree4_hook);
	if (error) {
		log_crit(ERR_ADD_BIB_FAILED, "The entry BIB was inserted in BIB_table_tree6 but exist in BIB_table_tree4");
		rb_erase(&(*bib)->tree6_hook, &table->tree6);
		bib_kfree(*bib);
		spin_unlock_bh(&table->bib_table_lock);
		return error;
	}

	spin_unlock_bh(&table->bib_table_lock);

	return 0;
}


/**
 * this wrappers are for unit testing purpose.
 */
int bibdb_for_each_ipv6(l4_protocol l4_proto, struct in6_addr *addr,
		int (*func)(struct bib_entry *, void *), void *arg)
{
	int error = 0;
	struct bib_table *table;

	error = get_bibdb_table(l4_proto, &table);
	if (error)
		return error;

	error = for_each_bib_ipv6(table, l4_proto, addr, func, arg);
	return error;
}

int bibdb_allocate_ipv4_transport_address(struct tuple *base,
		struct ipv4_tuple_address *result)
{
	int error = 0;
	struct bib_table *table;

	error = get_bibdb_table(base->l4_proto, &table);
	if (error)
		return error;

	error = allocate_ipv4_transport_address(table, base, result);
	return error;
}
