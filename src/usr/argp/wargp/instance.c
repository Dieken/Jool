#include "instance.h"

#include "log.h"
#include "requirements.h"
#include "wargp.h"
#include "common/config.h"
#include "common/constants.h"
#include "usr/util/str_utils.h"
#include "usr/nl/instance.h"
#include "usr/nl/jool_socket.h"
#include "usr/argp/xlator_type.h"

#define ARGP_IPTABLES 1000
#define ARGP_NETFILTER 1001
#define ARGP_POOL6 '6'

struct wargp_iname {
	bool set;
	char value[INAME_MAX_LEN];
};

#define WARGP_INAME(container, field, description) \
	{ \
		.name = "instance name", \
		.key = ARGP_KEY_ARG, \
		.doc = "Name of the instance you want to " description, \
		.offset = offsetof(container, field), \
		.type = &wt_iname, \
	}

static int parse_iname(void *void_field, int key, char *str)
{
	struct wargp_iname *field = void_field;
	int error;

	error = iname_validate(str, false);
	if (error) {
		pr_err(INAME_VALIDATE_ERRMSG, INAME_MAX_LEN - 1);
		return error;
	}

	field->set = true;
	strcpy(field->value, str);
	return 0;
}

struct wargp_type wt_iname = {
	.argument = "<instance name>",
	.parse = parse_iname,
	.candidates = "default",
};

struct display_args {
	struct wargp_bool no_headers;
	struct wargp_bool csv;
};

static struct wargp_option display_opts[] = {
	WARGP_NO_HEADERS(struct display_args, no_headers),
	WARGP_CSV(struct display_args, csv),
	{ 0 },
};

static void print_table_divisor(void)
{
	printf("+--------------------+-----------------+-----------+\n");
}

static void print_entry_csv(struct instance_entry_usr *entry)
{
	printf("%p,%s,", entry->ns, entry->iname);
	if (entry->fw == FW_NETFILTER)
		printf("netfilter");
	else if (entry->fw == FW_IPTABLES)
		printf("iptables");
	else
		printf("unknown");
	printf("\n");
}

static void print_entry_normal(struct instance_entry_usr *entry)
{
	/*
	 * 18 is "0x" plus 16 hexadecimal digits.
	 * Why is it necessary? Because the table headers and stuff assume 18
	 * characters and I'm assuming that 32-bit machines would print smaller
	 * pointers.
	 */
	printf("| %18p | %15s | ", entry->ns, entry->iname);
	if (entry->fw == FW_NETFILTER)
		printf("netfilter");
	else if (entry->fw == FW_IPTABLES)
		printf(" iptables");
	else
		printf("  unknown");
	printf(" |\n");
}

static struct jool_result print_entry(struct instance_entry_usr *instance,
		void *arg)
{
	struct display_args *args = arg;
	if (args->csv.value)
		print_entry_csv(instance);
	else
		print_entry_normal(instance);
	return result_success();
}

int handle_instance_display(char *iname, int argc, char **argv, void *arg)
{
	struct display_args dargs = { 0 };
	struct jool_socket sk;
	struct jool_result result;

	if (iname)
		pr_warn("instance display ignores the instance name argument.");

	result.error = wargp_parse(display_opts, argc, argv, &dargs);
	if (result.error)
		return result.error;

	result = netlink_setup(&sk, xt_get());
	if (result.error)
		return pr_result(&result);

	if (!dargs.no_headers.value) {
		if (dargs.csv.value) {
			printf("Namespace,Name,Framework\n");
		} else {
			print_table_divisor();
			printf("|          Namespace |            Name | Framework |\n");
		}
	}

	if (!dargs.csv.value)
		print_table_divisor();

	result = instance_foreach(&sk, print_entry, &dargs);

	netlink_teardown(&sk);

	if (result.error)
		return pr_result(&result);

	if (!dargs.csv.value)
		print_table_divisor();

	return 0;
}

void autocomplete_instance_display(void *args)
{
	print_wargp_opts(display_opts);
}

struct add_args {
	struct wargp_iname iname;
	struct wargp_bool iptables;
	struct wargp_bool netfilter;
	struct wargp_prefix6 pool6;
};

static struct wargp_option add_opts[] = {
	WARGP_INAME(struct add_args, iname, "add"),
	{
		.name = OPTNAME_IPTABLES,
		.key = ARGP_IPTABLES,
		.doc = "Sit the translator on top of iptables",
		.offset = offsetof(struct add_args, iptables),
		.type = &wt_bool,
	}, {
		.name = OPTNAME_NETFILTER,
		.key = ARGP_NETFILTER,
		.doc = "Sit the translator on top of Netfilter",
		.offset = offsetof(struct add_args, netfilter),
		.type = &wt_bool,
	}, {
		.name = "pool6",
		.key = ARGP_POOL6,
		.doc = "Prefix that will populate the IPv6 Address Pool",
		.offset = offsetof(struct add_args, pool6),
		.type = &wt_prefix6,
	},
	{ 0 },
};

int handle_instance_add(char *iname, int argc, char **argv, void *arg)
{
	struct add_args aargs = { 0 };
	struct jool_socket sk;
	struct jool_result result;

	result.error = wargp_parse(add_opts, argc, argv, &aargs);
	if (result.error)
		return result.error;

	/* Validate instance name */
	if (iname && aargs.iname.set && !STR_EQUAL(iname, aargs.iname.value)) {
		pr_err("You entered two different instance names. Please delete one of them.");
		return -EINVAL;
	}
	if (!iname && aargs.iname.set)
		iname = aargs.iname.value;

	/* Validate framework */
	if (!aargs.netfilter.value && !aargs.iptables.value) {
		pr_err("Please specify instance framework. (--"
				OPTNAME_NETFILTER " or --"
				OPTNAME_IPTABLES ".)");
		pr_err("(The Jool 3.5 behavior was --" OPTNAME_NETFILTER ".)");
		return -EINVAL;
	}
	if (aargs.netfilter.value && aargs.iptables.value) {
		pr_err("The translator can only be hooked to one framework.");
		return -EINVAL;
	}

	result = netlink_setup(&sk, xt_get());
	if (result.error)
		return pr_result(&result);

	result = instance_add(&sk,
			aargs.netfilter.value ? FW_NETFILTER : FW_IPTABLES,
			iname, aargs.pool6.set ? &aargs.pool6.prefix : NULL);

	netlink_teardown(&sk);
	return pr_result(&result);
}

void autocomplete_instance_add(void *args)
{
	print_wargp_opts(add_opts);
}

struct rm_args {
	struct wargp_iname iname;
};

static struct wargp_option remove_opts[] = {
	WARGP_INAME(struct rm_args, iname, "remove"),
	{ 0 },
};

int handle_instance_remove(char *iname, int argc, char **argv, void *arg)
{
	struct rm_args rargs = { 0 };
	struct jool_socket sk;
	struct jool_result result;

	result.error = wargp_parse(remove_opts, argc, argv, &rargs);
	if (result.error)
		return result.error;

	if (iname && rargs.iname.set && !STR_EQUAL(iname, rargs.iname.value)) {
		pr_err("You entered two different instance names. Please delete one of them.");
		return -EINVAL;
	}
	if (!iname && rargs.iname.set)
		iname = rargs.iname.value;

	result = netlink_setup(&sk, xt_get());
	if (result.error)
		return pr_result(&result);

	result = instance_rm(&sk, iname);

	netlink_teardown(&sk);
	return pr_result(&result);
}

void autocomplete_instance_remove(void *args)
{
	print_wargp_opts(remove_opts);
}

static struct wargp_option flush_opts[] = {
	{ 0 },
};

int handle_instance_flush(char *iname, int argc, char **argv, void *arg)
{
	struct jool_socket sk;
	struct jool_result result;

	if (iname)
		pr_warn("instance flush ignores the instance name argument.");

	result.error = wargp_parse(flush_opts, argc, argv, NULL);
	if (result.error)
		return result.error;

	result = netlink_setup(&sk, xt_get());
	if (result.error)
		return pr_result(&result);

	result = instance_flush(&sk);

	netlink_teardown(&sk);
	return pr_result(&result);
}

void autocomplete_instance_flush(void *args)
{
	print_wargp_opts(flush_opts);
}

static struct wargp_option status_opts[] = {
	{ 0 },
};

int handle_instance_status(char *iname, int argc, char **argv, void *arg)
{
	/*
	 * Note: If you want to change the labels "Dead" and "Running", do
	 * remember that this change will need to cascade to the init script.
	 */
	static char const *RUNNING_MSG = "Running\n";
	static char const *DEAD_MSG = "Dead\n";
	static char const *UNKNOWN_MSG = "Status unknown\n";

	struct jool_socket sk;
	enum instance_hello_status status;
	struct jool_result result;

	result.error = wargp_parse(flush_opts, argc, argv, NULL);
	if (result.error)
		return result.error;

	result = netlink_setup(&sk, xt_get());
	if (result.error == -ESRCH)
		printf("%s", DEAD_MSG);
	if (result.error)
		return pr_result(&result);

	result = instance_hello(&sk, iname, &status);
	if (result.error) {
		printf("%s", UNKNOWN_MSG);
		goto end;
	}

	switch (status) {
	case IHS_ALIVE:
		printf("%s", RUNNING_MSG);
		break;
	case IHS_DEAD:
		printf("%s", DEAD_MSG);
		printf("(Instance '%s' does not exist.)\n",
				iname ? iname : INAME_DEFAULT);
		break;
	}

end:
	netlink_teardown(&sk);
	return pr_result(&result);
}

void autocomplete_instance_status(void *args)
{
	print_wargp_opts(status_opts);
}
