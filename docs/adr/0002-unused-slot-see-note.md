# ADR-0002 — Slot unused: no decision was recorded under this number

Nygard numbering in this repo allocates `0001`, `0003`, `0004`, `0005`, `0006`
(model IDs and pricing sources). No architectural decision ever carried the
number `0002` — the gap is an artifact of early drafting, not a withdrawn or
superseded record. Do not renumber existing ADRs; the next decision takes the
next free number (`0006`).
