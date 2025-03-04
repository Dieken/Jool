#include "mod/common/nl/bib.h"

#include "mod/common/log.h"
#include "mod/common/nl/nl_common.h"
#include "mod/common/nl/nl_core.h"
#include "mod/nat64/pool4/db.h"
#include "mod/nat64/bib/db.h"

static int bib_entry_to_userspace(struct bib_entry const *entry, bool is_static,
		void *arg)
{
	struct nlcore_buffer *buffer = (struct nlcore_buffer *)arg;
	struct bib_entry_usr entry_usr;

	entry_usr.addr4 = entry->ipv4;
	entry_usr.addr6 = entry->ipv6;
	entry_usr.l4_proto = entry->l4_proto;
	entry_usr.is_static = is_static;

	return nlbuffer_write(buffer, &entry_usr, sizeof(entry_usr));
}

static int handle_bib_display(struct bib *db, struct genl_info *info,
		struct request_bib *request)
{
	struct nlcore_buffer buffer;
	struct bib_foreach_func func = {
			.cb = bib_entry_to_userspace,
			.arg = &buffer,
	};
	struct ipv4_transport_addr *offset;
	int error;

	log_debug("Sending BIB to userspace.");

	error = nlbuffer_init_response(&buffer, info, nlbuffer_response_max_size());
	if (error)
		return nlcore_respond(info, error);

	offset = request->foreach.addr4_set ? &request->foreach.addr4 : NULL;
	error = bib_foreach(db, request->l4_proto, &func, offset);
	nlbuffer_set_pending_data(&buffer, error > 0);
	error = (error >= 0)
			? nlbuffer_send(info, &buffer)
			: nlcore_respond(info, error);

	nlbuffer_clean(&buffer);
	return error;
}

static int handle_bib_add(struct xlator *jool, struct request_bib *request)
{
	struct bib_entry new;

	log_debug("Adding BIB entry.");

	if (!pool4db_contains(jool->nat64.pool4, jool->ns, request->l4_proto,
			&request->add.addr4)) {
		log_err("The transport address '%pI4#%u' does not belong to pool4.\n"
				"Please add it there first.",
				&request->add.addr4.l3, request->add.addr4.l4);
		return -EINVAL;
	}

	new.ipv6 = request->add.addr6;
	new.ipv4 = request->add.addr4;
	new.l4_proto = request->l4_proto;
	return bib_add_static(jool, &new);
}

static int handle_bib_rm(struct xlator *jool, struct request_bib *request)
{
	struct bib_entry bib;
	int error;

	log_debug("Removing BIB entry.");

	if (request->rm.addr6_set && request->rm.addr4_set) {
		bib.ipv6 = request->rm.addr6;
		bib.ipv4 = request->rm.addr4;
		bib.l4_proto = request->l4_proto;
		error = 0;
	} else if (request->rm.addr6_set) {
		error = bib_find6(jool->nat64.bib, request->l4_proto,
				&request->rm.addr6, &bib);
	} else if (request->rm.addr4_set) {
		error = bib_find4(jool->nat64.bib, request->l4_proto,
				&request->rm.addr4, &bib);
	} else {
		log_err("You need to provide an address so I can find the entry you want to remove.");
		return -EINVAL;
	}

	if (error == -ESRCH)
		goto esrch;
	if (error)
		return error;

	error = bib_rm(jool, &bib);
	if (error == -ESRCH) {
		if (request->rm.addr6_set && request->rm.addr4_set)
			goto esrch;
		/* It died on its own between the find and the rm. */
		return 0;
	}

	return error;

esrch:
	log_err("The entry wasn't in the database.");
	return -ESRCH;
}

int handle_bib_config(struct xlator *jool, struct genl_info *info)
{
	struct request_hdr *hdr = get_jool_hdr(info);
	struct request_bib *request = (struct request_bib *)(hdr + 1);
	int error;

	if (xlat_is_siit()) {
		log_err("SIIT doesn't have BIBs.");
		return nlcore_respond(info, -EINVAL);
	}

	error = validate_request_size(info, sizeof(*request));
	if (error)
		return nlcore_respond(info, error);

	switch (hdr->operation) {
	case OP_FOREACH:
		return handle_bib_display(jool->nat64.bib, info, request);
	case OP_ADD:
		error = handle_bib_add(jool, request);
		break;
	case OP_REMOVE:
		error = handle_bib_rm(jool, request);
		break;
	default:
		log_err("Unknown operation: %u", hdr->operation);
		error = -EINVAL;
	}

	return nlcore_respond(info, error);
}
