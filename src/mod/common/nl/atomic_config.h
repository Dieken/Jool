#ifndef _JOOL_MOD_NL_ATOMIC_CONFIG_H
#define _JOOL_MOD_NL_ATOMIC_CONFIG_H

#include <net/genetlink.h>
#include "mod/common/xlator.h"

int handle_atomconfig_request(struct genl_info *info);

#endif /* _JOOL_MOD_NL_ATOMIC_CONFIG_H */
