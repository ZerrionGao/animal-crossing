#pragma once

#include <stddef.h>

typedef enum {
    ACNH_MSL_UNCHANGED = 0,
    ACNH_MSL_CHANGED = 1,
    ACNH_MSL_ERROR = -1
} acnh_msl_result;

/* The caller owns *output and must free it. On ERROR, *output is NULL. */
acnh_msl_result acnh_msl_normalize(const char *source, size_t source_length,
                                   char **output, size_t *output_length,
                                   char *error, size_t error_capacity);
