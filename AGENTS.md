# Local code retrieval

AvoryOS sources and documents are indexed in the local Xerj service under
the `avory` prefix. Use short retrieved passages to orient code investigations
and avoid loading large files into context unnecessarily.

- Search concepts: `/home/offihito/.local/bin/avory-search "DMA mapping cache coherency"`
- Find a definition: `/home/offihito/.local/bin/avory-search --def "dma_map_page"`
- Refresh after source changes: `/home/offihito/.local/bin/avory-search --update`

The helper reads the API key from a private file; never print the key or commit it.
The index respects `.gitignore` and omits hidden files and build outputs.
Search covers indexed content only. Check the current source around returned
locations before editing; use `rg` for exact text and filename searches, ignored
dependencies, missing results, or when Xerj is unavailable. Keep reads bounded.

Refresh before relying on the index in a new session or after relevant edits.
Refresh reconciles added, modified and deleted files; no watch process is running.
The default embedding mode is local lexical hashing, not a neural model.
