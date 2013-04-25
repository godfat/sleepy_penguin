#ifndef VALUE2TIMESPEC_H
#define VALUE2TIMESPEC_H

#include <ruby.h>
#include <math.h>
#include <time.h>

#ifndef NUM2TIMET
#  define NUM2TIMET(n) NUM2LONG(n)
#endif

#ifndef RFLOAT_VALUE
#  define RFLOAT_VALUE(v) (RFLOAT(v)->value)
#endif

static struct timespec *value2timespec(struct timespec *ts, VALUE num)
{
	switch (TYPE(num)) {
	case T_FIXNUM:
	case T_BIGNUM:
		ts->tv_sec = NUM2TIMET(num);
		ts->tv_nsec = 0;
		return ts;
	case T_FLOAT: {
		double orig = RFLOAT_VALUE(num);
		double f, d;

		d = modf(orig, &f);
		if (d >= 0) {
			ts->tv_nsec = (long)(d * 1e9 + 0.5);
		} else {
			ts->tv_nsec = (long)(-d * 1e9 + 0.5);
			if (ts->tv_nsec > 0) {
				ts->tv_nsec = (long)1e9 - ts->tv_nsec;
				f -= 1;
			}
		}
		ts->tv_sec = (time_t)f;
		if (f != ts->tv_sec)
			rb_raise(rb_eRangeError, "%f out of range", orig);
		return ts;
	}}
	{
		VALUE tmp = rb_inspect(num);
		rb_raise(rb_eTypeError, "can't convert %s into timespec",
			 StringValuePtr(tmp));
	}
	rb_bug("rb_raise() failed, timespec failed");
	return NULL;
}

#ifndef TIMET2NUM
#  define TIMET2NUM(n) LONG2NUM(n)
#endif

static inline VALUE timespec2num(struct timespec *ts)
{
	if (ts->tv_nsec == 0)
		return TIMET2NUM(ts->tv_sec);

	return rb_float_new(ts->tv_sec + ((double)ts->tv_nsec / 1e9));
}

#endif /* VALUE2TIMESPEC_H */
