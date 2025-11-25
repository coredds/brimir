/* Override FORCE_INLINE for GCC to avoid always_inline errors */
#ifdef __GNUC__
#ifndef _MSC_VER
#ifdef FORCE_INLINE
#undef FORCE_INLINE
#endif
#define FORCE_INLINE inline __attribute__((unused))
#endif
#endif





