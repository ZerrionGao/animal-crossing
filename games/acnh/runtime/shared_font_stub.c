/* ACNH 阶段 A 尚未接入系统共享字体；收到字体服务时保留明确的未实现结果。 */
#include "runtime.h"

void *runtime_font_override(const char *name) { (void)name; return NULL; }
