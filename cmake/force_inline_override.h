/* 
 * Override FORCE_INLINE to avoid compiler-specific issues
 * This file is force-included before all other headers via -include
 * 
 * The problem: Ymir's inline.hpp defines FORCE_INLINE as [[gnu::always_inline]]
 * which causes GCC to error if a function can't be inlined. We override it to
 * just 'inline' to let the compiler decide.
 * 
 * Since inline.hpp will redefine FORCE_INLINE, we need to make sure our definition
 * survives. We do this by:
 * 1. Defining it here first (via -include)
 * 2. Adding -Wno-macro-redefined to suppress warnings about redefinition
 * 3. The compiler will use the LAST definition, which will be ours again after
 *    inline.hpp tries to redefine it (we'll undef and redefine it)
 */

#ifndef BRIMIR_FORCE_INLINE_OVERRIDE
#define BRIMIR_FORCE_INLINE_OVERRIDE

/* Undefine if already defined */
#ifdef FORCE_INLINE
#undef FORCE_INLINE
#endif

#if defined(__GNUC__) && !defined(_MSC_VER)
    /* GCC/Clang on Linux: Use regular inline, let compiler decide when to inline */
    #define FORCE_INLINE inline __attribute__((unused))
#elif defined(_MSC_VER)
    /* MSVC on Windows: Use __forceinline for better performance */
    #define FORCE_INLINE __forceinline
#else
    /* Fallback */
    #define FORCE_INLINE inline
#endif

#endif /* BRIMIR_FORCE_INLINE_OVERRIDE */





