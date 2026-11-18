/* Exec policy: refuses what DynamoRIO cannot run, follows execveat, shows the app its own executable. */
#ifndef V3COMPAT_EXECPOLICY_H
#define V3COMPAT_EXECPOLICY_H
#include "dr_api.h"
/* False if a registration failed: nothing would then judge the execs. */
bool exec_policy_init(void);
#endif
