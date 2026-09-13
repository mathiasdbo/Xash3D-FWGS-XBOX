/*
msvc_rtti_dynamic_cast.cpp - a real __RTDynamicCast, for nxdk

Part of compat/xbox; see that directory's README.

nxdk's toolchain (clang, -target i386-pc-win32) genuinely emits every
real, correct MSVC-ABI RTTI data structure for every polymorphic class -
verified directly, not assumed: `llvm-nm` on a real compiled freevgui
object shows a complete, correctly-linked set of ??_R0 (TypeDescriptor),
??_R1 (BaseClassDescriptor), ??_R2 (BaseClassArray), ??_R3
(ClassHierarchyDescriptor) and ??_R4 (CompleteObjectLocator) symbols for
every class `3rdparty/freevgui` declares. What is genuinely missing from
nxdk's prebuilt C++ runtime libraries (confirmed via `llvm-nm
--defined-only` against every one of them, the same check divergence
#37 already made for this exact symbol on the server SDK) is only the
RUNTIME WALKER - the one function that reads this already-correct,
already-emitted metadata at `dynamic_cast<T*>()` call sites. This file
provides that function, and nothing else - no new RTTI codegen, no
change to what the compiler already does for every class in this
project.

This is a clean-room implementation, written from public, documented
knowledge of the MSVC RTTI ABI's data layout (the struct shapes below
are corroborated by multiple independent, published sources: Lukasz
Lipski's "RTTI Internals in MSVC" and Quarkslab's "Visual C++ RTTI
Inspection" both describe the same TypeDescriptor / PMD /
RTTIBaseClassDescriptor / RTTIClassHierarchyDescriptor /
RTTICompleteObjectLocator layout independently) and from the C++
standard's own dynamic_cast semantics (a linear search for a target base
class, unambiguous-match required, matching in spirit the general
algorithm every real dynamic_cast implementation - MSVC's or the
Itanium ABI's __dynamic_cast, described publicly in the original Sun
RTTI specification - has to perform) - not transcribed from Microsoft's
own proprietary CRT source, which this project has no license to
reproduce.

Deliberately, honestly scoped rather than attempting a byte-for-byte
match of Microsoft's own more elaborate, multi-function implementation
(FindSITargetTypeInstance/FindMITargetTypeInstance/FindVITargetTypeInstance,
a real internal split this file does not need to replicate to be
correct - a single linear scan over the base class array is a valid,
simpler implementation of the same required semantics for every
non-virtual-inheritance case, which is every case this project's own
code actually reaches):

- Non-virtual inheritance (single OR multiple): fully, correctly
  handled. Verified this is the only case this project's real code
  needs: `grep -rn ": public.*,.*public" 3rdparty/freevgui` finds real
  multiple-inheritance classes (TextEntry, TrayClock, ...), and this
  function is exercised by real dynamic_cast<> calls whose SOURCE
  objects can genuinely be one of those (e.g. `frame.cpp:104`'s
  `dynamic_cast<Frame*>(panel->getParent())` can receive any real Panel
  subclass, including a multiply-inherited TextEntry) - handled
  correctly by the same linear-scan algorithm below, which does not
  special-case single vs. multiple inheritance the way Microsoft's own
  split implementation does.
- Virtual inheritance: deliberately, honestly NOT implemented - see the
  `CHD_VIRTINH` check in `__RTDynamicCast` below. Verified unreachable
  by anything this project's own code actually calls today (`grep -rn
  "virtual public" 3rdparty/freevgui` finds exactly one case, DataInputStream :
  virtual public InputStream, nowhere near Panel/LayoutInfo's own
  hierarchy - confirmed by grepping every `: public` declaration
  reachable from Panel/LayoutInfo). The MSVC PMD-to-offset conversion
  for a virtual base requires reading a real vbtable at runtime, whose
  exact vdisp semantics (byte offset vs. table index) could not be
  confirmed against an authoritative primary source and could not be
  verified against a real, constructible test case in this codebase
  either. Matching this project's own established standard (divergence
  #42: an honest, blocking failure is safer than a silently wrong
  success), this path returns nullptr rather than guess - if a future
  caller anywhere in this codebase genuinely needs a dynamic_cast
  through a virtual base, that is real, separate, verifiable work,
  not something to risk getting silently wrong here.
- Reference casts (isReference != 0, which the C++ standard requires to
  throw std::bad_cast rather than return null on failure): no real
  caller in this codebase currently compiles a reference dynamic_cast
  (verified: every real dynamic_cast<> site found is a pointer cast) -
  nxdk's own C++ exception support is not established either way for
  this target, so rather than risk a wrong or crashing throw path for a
  case nothing currently reaches, this returns nullptr instead of
  throwing - a real, documented, deliberately deferred gap, not a
  silent behaviour change for any call site that exists today.

Verified: real, compiled dynamic_cast<> calls in 3rdparty/freevgui
(frame.cpp, table.cpp, treefolder.cpp, layout.cpp) now link (`___RTDynamicCast`
was the last of the 527 originally-undefined VGUI1 symbols, divergence
#42) and were exercised directly with constructed real objects - see
this divergence's own real/measured verification for the exact cases
checked (a positive single-inheritance downcast, MessageBox -> Frame;
a negative multiple-inheritance non-match, TextEntry is not a Frame;
an unrelated-type negative match).
*/

namespace
{

// ---- MSVC 32-bit RTTI structure layout ----
// 32-bit (this target): real pointers throughout, not the x64 scheme's
// image-relative 32-bit RVAs - confirmed by both independent published
// sources cited above, which describe the 32-bit layout as the direct-
// pointer predecessor the x64/RVA scheme was later derived from.
//
// Plain `long`/`unsigned long` throughout, not <cstdint>'s int32_t/
// uint32_t or <cstddef>'s ptrdiff_t: this target's `long` is already
// exactly 32 bits (verified: this is the same width the real ABI
// requires), and avoiding those two standard headers lets this file
// compile under tools/xbox-cc.sh's plain, C-only include set (no
// libc++ path) the same way every other compat/xbox source file
// already does, rather than needing tools/xbox-cxx.sh's extra
// -I.../lib/libcxx/include - not needed for two integer typedefs.

struct RTTITypeDescriptor
{
	void *vfptr;   // pointer to std::type_info's own vtable
	void *spare;   // runtime-reserved, always null
	char  name[1]; // mangled type name, null-terminated
};

struct PMD
{
	long mdisp; // member displacement
	long pdisp; // vbtable displacement; -1 means "no virtual inheritance involved"
	long vdisp; // displacement inside the vbtable
};

enum : unsigned long
{
	BCD_AMBIGUOUS = 0x2, // the compiler already determined this base is reachable via more than one path - a real, compile-time-computed fact this file trusts rather than re-deriving
};

struct RTTIBaseClassDescriptor
{
	RTTITypeDescriptor *pTypeDescriptor;
	unsigned long        numContainedBases;
	PMD                  where;
	unsigned long        attributes;
	// A pClassDescriptor field exists here in real MSVC builds too;
	// this file never needs it (no recursive base-of-base lookup - the
	// array below is already a full, flattened list of every ancestor),
	// so it is not declared, matching this file's own "not a superset
	// of the ABI, only what dynamic_cast actually needs" scope.
};

enum : unsigned long
{
	CHD_VIRTINH = 0x2,
	// CHD_MULTINH (0x1) is real too, but this file's own unified
	// linear-scan algorithm (FindBaseOffset below) handles single and
	// multiple non-virtual inheritance identically, unlike Microsoft's
	// own split SI/MI implementations - so nothing here ever branches
	// on it, deliberately, not an oversight.
};

struct RTTIClassHierarchyDescriptor
{
	unsigned long              signature;
	unsigned long              attributes;
	unsigned long              numBaseClasses;
	RTTIBaseClassDescriptor **pBaseClassArray;
};

struct RTTICompleteObjectLocator
{
	unsigned long                  signature;
	unsigned long                  offset;   // this vtable's own sub-object's offset within the complete object
	unsigned long                  cdOffset;
	RTTITypeDescriptor            *pTypeDescriptor;
	RTTIClassHierarchyDescriptor *pClassDescriptor;
};

// The compiler places a pointer to the RTTICompleteObjectLocator one
// pointer-width before the vtable's own function-pointer entries -
// real, confirmed layout (both independent sources cited above agree),
// and the exact reason a dynamic_cast needs the object's real vtable
// pointer at all: it is the only place this metadata is reachable from.
inline RTTICompleteObjectLocator *LocatorFromVftable( void *vftable )
{
	return *( reinterpret_cast<RTTICompleteObjectLocator **>( vftable ) - 1 );
}

inline bool SameType( const RTTITypeDescriptor *a, const RTTITypeDescriptor *b )
{
	// TypeDescriptors for the same class COMDAT-fold to one real
	// instance within a single static link (this whole build is one
	// binary, no cross-module boundary at all) - pointer identity is
	// the correct, sufficient comparison here, not a name string
	// comparison (which real MSVC falls back to only for the cross-DLL
	// case this project's static-linked-everything architecture never
	// has - X7's own whole premise).
	return a == b;
}

// The single non-virtual-base case: a flat, compile-time-constant
// displacement. See this file's own header comment for why the virtual
// case (pdisp != -1) is deliberately not attempted here.
inline bool NonVirtualOffset( const PMD &pmd, long *outOffset )
{
	if ( pmd.pdisp != -1 )
		return false;
	*outOffset = pmd.mdisp;
	return true;
}

// Linear scan of the source object's own (real, dynamic-type) base
// class array for a target type match, handling single AND multiple
// non-virtual inheritance correctly with the same code - see this
// file's own header comment for why this project's own real call sites
// never need Microsoft's separate SI/MI/VI-specific implementations to
// get the same, correct result. A class appearing at more than one
// distinct offset (repeated non-virtual inheritance of the same base
// via two different, unrelated parents) is ambiguous, matching real
// dynamic_cast semantics - detected here directly (two matches at
// different offsets), on top of trusting the compiler's own
// BCD_AMBIGUOUS bit for the cases it already flags.
bool FindBaseOffset( const RTTIClassHierarchyDescriptor *hierarchy, const RTTITypeDescriptor *target, long *outOffset )
{
	bool foundOne = false;
	long foundOffset = 0;

	for ( unsigned long i = 0; i < hierarchy->numBaseClasses; i++ )
	{
		const RTTIBaseClassDescriptor *entry = hierarchy->pBaseClassArray[i];
		if ( !SameType( entry->pTypeDescriptor, target ))
			continue;

		if ( entry->attributes & BCD_AMBIGUOUS )
			return false;

		long thisOffset;
		if ( !NonVirtualOffset( entry->where, &thisOffset ))
			return false; // virtual base - see this file's own header comment

		if ( foundOne && thisOffset != foundOffset )
			return false; // two distinct, non-overlapping matches - genuinely ambiguous

		foundOne = true;
		foundOffset = thisOffset;
	}

	if ( !foundOne )
		return false;

	*outOffset = foundOffset;
	return true;
}

} // namespace

extern "C"
{

// R3D: the real, missing MSVC-ABI runtime function nxdk's toolchain
// never shipped an implementation of - see this file's own header
// comment for the full account (divergence #42, docs/r3d/divergences.md).
// Parameter names/types match the real, documented signature exactly
// (LONG -> long, BOOL -> int on this target) so every real
// compiler-generated call site's argument passing is correct without
// this file needing to know anything about them beyond this contract.
void *__RTDynamicCast( void *inptr, long VfDelta, void *srcType, void *targetType, int isReference )
{
	(void)srcType; // the object's OWN real dynamic-type locator (read below) is authoritative - the compiler-supplied static source type is not needed to answer "is this actually a T", only to select which of Microsoft's real internal functions to call, a split this file does not replicate

	if ( inptr == nullptr )
		return nullptr;

	// VfDelta: every real dynamic_cast<> call site in this codebase
	// casts from a plain, primary-vtable-at-offset-0 pointer (Panel*,
	// LayoutInfo*) - never through a secondary base of a multiply-
	// inherited interface - so the compiler always bakes VfDelta as a
	// literal 0 for every real caller here (verified: grep found no
	// dynamic_cast<> site in this codebase casting from anything but a
	// plain Panel*/LayoutInfo* expression). A nonzero VfDelta would mean
	// the object's real vtable pointer for the STATIC starting type
	// lives at inptr+VfDelta rather than inptr+0; applying that
	// adjustment here before reading the vtable keeps this function
	// correct for that case too, at zero cost for the real, common one.
	void *sourceThis = static_cast<char *>( inptr ) + VfDelta;
	void *vftable = *reinterpret_cast<void **>( sourceThis );
	RTTICompleteObjectLocator *locator = LocatorFromVftable( vftable );

	void *completeObject = static_cast<char *>( sourceThis ) - locator->offset;
	const RTTITypeDescriptor *dynamicType = locator->pTypeDescriptor;
	const RTTITypeDescriptor *target = static_cast<const RTTITypeDescriptor *>( targetType );

	// Exact match: the object's own real, most-derived type already IS
	// the target - the common case for every one of this codebase's
	// real call sites when the cast succeeds (Frame::sIsFrame's own
	// check, TreeFolder's own recursive walk - the object really is
	// exactly that type, not some further, hypothetical subclass of
	// it). Handled first, without needing the base class array at all.
	if ( SameType( dynamicType, target ))
		return completeObject;

	if ( locator->pClassDescriptor->attributes & CHD_VIRTINH )
		return nullptr; // see this file's own header comment

	long offset;
	if ( !FindBaseOffset( locator->pClassDescriptor, target, &offset ))
	{
		// isReference would throw std::bad_cast here in a real MSVC
		// build - see this file's own header comment for why that path
		// is deliberately not implemented (unreached by any real
		// caller in this codebase today).
		(void)isReference;
		return nullptr;
	}

	return static_cast<char *>( completeObject ) + offset;
}

} // extern "C"
