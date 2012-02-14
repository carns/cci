/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 */

#ifndef CCI_CORE_MX_H
#define CCI_CORE_MX_H

#include "cci/config.h"

BEGIN_C_DECLS int cci_core_mx_post_load(cci_plugin_t * me);
int cci_core_mx_pre_unload(cci_plugin_t * me);

END_C_DECLS
#endif /* CCI_CORE_MX_H */
