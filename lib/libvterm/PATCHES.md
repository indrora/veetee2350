# Local changes to vendored libvterm

Upstream: https://www.leonerd.org.uk/code/libvterm/, release 0.3.3
(sha256 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0).

Vendored: `src/` (including the generated `encoding/*.inc` and
`fullwidth.inc` shipped in the release tarball), `include/`, `LICENSE`.
Left out: `t/` (upstream tests), `bin/`, the Makefile.

Built as gnu11 (`library.json`). It is upstream's own C99 code, and we
don't want C23 keyword changes to start mattering in vendored code.

No source modifications.
