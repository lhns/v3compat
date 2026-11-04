/* Exec policy: refuses what DynamoRIO cannot run, follows execveat, shows the app its own executable. */
#ifndef V3COMPAT_EXECPOLICY_H
#define V3COMPAT_EXECPOLICY_H
void exec_policy_init(void);
#endif
