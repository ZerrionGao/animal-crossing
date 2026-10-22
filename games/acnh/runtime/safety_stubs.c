/* ACNH 运行时阶段 A 的链接桩：共享 TOTK 运行时的游戏专用实现暂不启用。 */
#include "runtime.h"
#include "world_survey.h"

#include <stdatomic.h>

_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, 0};

void runtime_survey_init(Module *module) { (void)module; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}

void *runtime_swkbd_override(const char *name) { (void)name; return NULL; }
void runtime_swkbd_selftest(void) {}
void runtime_swkbd_probe(void) { runtime_fail("ACNH swkbd probe disabled: no verified PLT/GOT site"); }
