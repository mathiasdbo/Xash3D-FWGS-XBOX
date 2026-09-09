/*
new.h - the legacy global-namespace placement-new header, for nxdk

Part of compat/xbox; see that directory's README.

Real MSVC's own <new.h> is a thin, pre-standard compatibility wrapper around
<new> - the same declarations, no separate implementation. nxdk's clang
target genuinely predefines _MSC_VER (x0-probe-results.md's own measurement:
1950), so any vendored code with `#ifdef _MSC_VER #include <new.h>` takes
this branch for real, not hypothetically - the same recurring class of trap
divergence #17 already names (celt.c's _set_abort_behavior). nxdk's libcxx
has no <new.h> of its own, only the standard <new>, so this reaches TUs
that ask for the legacy name by matching what real MSVC's own header does:
including <new> and nothing else.

compat/xbox precedes nxdk's own include directories on every tools/xbox-*.sh
invocation, so this is found before clang would otherwise report the header
missing.
*/
#ifndef R3D_COMPAT_NEW_H
#define R3D_COMPAT_NEW_H

#include <new>

#endif /* R3D_COMPAT_NEW_H */
