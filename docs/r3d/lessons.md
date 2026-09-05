# Lessons

Rules earned by getting something wrong. Every entry exists because a mistake
was made, found, and generalised.

**This file is read at the start of every session.** It is the only mechanism
that stops the same mistake being made twice, so it is worth keeping short and
sharp. See `/AGENTS.md`, "Learning from mistakes", for when and how to add to
it.

## Meta-rules for writing entries here

Follow these or the file degrades into notes nobody reads.

1. **Lead with the rule, in the imperative.** ALWAYS or NEVER where the rule
   really is absolute. The reasoning comes after, in one sentence.
2. **Generalise past the specific bug.** "Check the header field order" is a
   note. "NEVER trust a format's field order from a comment — read the struct
   that writes it" is a rule.
3. **Keep the evidence.** One `file:line` or one measurement, so a future
   reader can tell whether the rule still applies.
4. **One rule per entry.** If it needs "and", it is two.
5. **Delete rules that stop being true.** A stale rule costs more than a
   missing one, because it gets followed.
6. **Do not add a rule for a one-off slip.** Only for a mistake with a shape
   that will recur.

---

## Rules

### NEVER conclude an asset is present because the pipeline should have included it

The previous project's baker discovered map assets by regex-scraping the BSP
entity lump. Anything precached by a fixed string in game code was invisible
to it: five combat sprites were silently missing, and the feature compiled,
passed its unit tests, booted, and drew nothing. A code review found it, not
the tests.

**Verify presence by observing what the engine requests at runtime**, not by
reasoning about what the pipeline should have found.

### NEVER derive display aspect ratio from pixel dimensions

`V_AdjustFov` (`engine/client/cl_view.c:252`) returns early when
`width * 3 == 4 * height`. On anamorphic Xbox widescreen the framebuffer is
640x480, so the test is true and no correction is applied — precisely the case
that needs it. Pixel aspect ratio and display aspect ratio are independent
quantities on any console.

### NEVER assume an optimisation transfers because the engine it came from is similar

Ironwail is a Quake source port and its techniques look portable. They are
compute shaders, indirect multi-draw, bindless textures and persistent
mapping, none of which exist on the NV2A or the RSX. Check the *hardware
feature*, not the *engine lineage*, before planning work around a technique.

### NEVER assess a body of work from its headline feature list

Ironwail's README leads with compute shaders, indirect multi-draw, bindless
textures and persistent mapping. None of those exist on the NV2A, and the
conclusion drawn was that none of Ironwail transfers to the console targets.

That was wrong, and it would have cost most of the available performance.
Those four are the *marketing* of the project, not the bulk of it. The bulk is
sign-bit frustum rejection, radix-sorted batching, zero-allocation ring
buffers, distance-sorted transparency, PVS caching and threaded I/O — all of
which port unchanged.

**Read the work, not the summary of the work.** A feature list is written to
impress; it is a bad basis for a portability judgement. And when a mechanism
does not transfer, ask what *destination* it was moving work to, and whether
another destination is available — here, bake time was, and nobody had looked.

### ALWAYS re-check a target spec against the current compiler before trusting it

The previous project's Xbox target JSON accumulated six fields that modern
rustc rejects, over seven months, because nothing exercised the build. A
configuration nothing runs is a configuration that is already broken.

**Corollary:** add a target to CI the moment it compiles, even while the
binary does nothing useful.

### NEVER let a tool rewrite a file's line endings

Rewriting `.gitignore` through a text-processing script converted the whole
file from CRLF to LF, turning a 14-line addition into a 766-line diff. On a
fork, that hides the real change and poisons every future merge. Preserve the
original bytes and append.

### ALWAYS state which question an answer is answering when the scope has moved

"Rewrite the engine in C++" and "start from the complete upstream engine
instead of a partial reimplementation" are different questions with opposite
answers. An answer carried over from the earlier framing is wrong without
being incorrect, which is the hardest kind to catch.
