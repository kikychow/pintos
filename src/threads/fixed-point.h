#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stddef.h>

#define P  17
#define Q  14
#define F  16384

/* X and Y are fixed point numbers, N is integer. */
/* Returns fixed point number representations. */

#define TO_FIXED_POINT(N) ((N) * F)

#define TO_INTEGER_ROUNDING_TO_ZERO(X) ((X) / F)

#define TO_INTEGER_ROUNDING_TO_NEAREST(X) \
        ((X) >= 0 ? (((X) + (F / 2)) / (F)) : (((X) - (F / 2)) / (F)))

#define ADD(X, Y) ((X) + (Y))

#define SUBTRACT(X, Y) ((X) - (Y))

#define ADD_WHEN_N_NOT_FIXED_POINT(X, N) ((X) + TO_FIXED_POINT (N))

#define SUBTRACT_WHEN_N_NOT_FIXED_POINT(X, N) ((X) - TO_FIXED_POINT (N))

#define MULTIPLY(X, Y) ((int64_t) (X) * Y / F)

#define MULTIPLY_WHEN_N_NOT_FIXED_POINT(X, N) (X * N)

#define DIVIDE(X, Y) ((int64_t) (X) * F / Y)

#define DIVIDE_WHEN_N_NOT_FIXED_POINT(X, N) (X / N)

#endif /* threads/fixed-point.h */
