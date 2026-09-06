# Specialist agents

One per real risk area in this project. Pick the ones a change actually
touches; never ask permission first (see `/AGENTS.md`).

They exist because the failure modes here are domain-specific and mostly
silent: a symbol collision that links, a texture that swizzles wrong only on
the console path, a budget that counts half the memory, a divergence from
upstream nobody recorded.

One of them is a check on the others. `reference-renderer-specialist` confirms
or refutes any statement about what the reference renderer does, by reading
it and citing the line. It was added after five of eighteen planned "ports
from Ironwail" turned out to already be in the tree, and one claim about the
reference had been copied into another specialist's prompt while false.
