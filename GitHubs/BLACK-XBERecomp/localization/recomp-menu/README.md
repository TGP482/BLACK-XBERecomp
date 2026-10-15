# Recompilation menu translations

This folder contains Portuguese (Brazil), Indonesian, and Russian translations for the text added by the BLACK XBE Recomp settings pages and in-game recompilation panel.

Each JSONL record uses the exact English phrase in `key` and `source_en` as its lookup key. The 90 current phrases are based on `src/common/translations.cpp` in the `soldieg/BLACK-XBERecomp` fork. The three native language names (`PORTUGUESE`, `INDONESIAN`, and `RUSSIAN`) are included for the planned expanded selector. These files are translation data only; runtime loading and language selection are separate implementation work.

`pt-BR.jsonl`, `id.jsonl`, and `ru.jsonl` contain only these three requested locales. Existing French, German, Italian, and Spanish translations remain in the recompilation source and are not duplicated here. English phrases are the source of truth. Existing game menu catalogs can inform terminology, but their Main-bank records are not interchangeable with recompilation-specific UI strings.

All three catalogs are AI-authored drafts and require human/native-speaker review. Russian strings are translated from English; the supplied Russian MainUK bank's special glyph mapping has not been decoded and was not used as a verified text source. Indonesian and Russian font/runtime dependencies are documented separately under `localization/game/`.
