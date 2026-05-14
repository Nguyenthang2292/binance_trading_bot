# Market Scanner Strategy Engine

## Goal

Implement the approved v1.0 market scanner, strategy framework, and sequential signal engine without replacing the existing single-symbol `TradingEngine`.

## Tasks

- [ ] Add `src/scanner/kline_cache.h/.cpp` with fixed-size per-symbol/per-interval buffers and snapshot reads -> Verify: `test_kline_cache.cpp` covers insert, replacement, buffer rotation, and empty lookup.
- [ ] Add `src/strategy/strategy_config.h`, `src/strategy/istrategy.h`, and `src/strategy/strategy_registry.h/.cpp` -> Verify: `test_strategy_interface.cpp` compiles a mock strategy and registry interval filtering passes.
- [ ] Add `src/strategy/indicators/atr.h/.cpp` using Wilder ATR -> Verify: `test_atr_indicator.cpp` checks known candles plus insufficient-data behavior.
- [ ] Add `src/engine/position_tracker.h/.cpp`, `sizing_policy.h/.cpp`, and `work_queue.h/.cpp` -> Verify: `test_position_tracker.cpp`, `test_sizing_policy.cpp`, and `test_work_queue.cpp` pass with one-way-position and round-robin cases.
- [ ] Add `src/scanner/market_scanner.h/.cpp` around `RestClient`, `WsClient::subscribeKline`, and `KlineCache` -> Verify: unit tests compile with fakes or narrow seams; manual testnet check fills cache for one symbol/interval.
- [ ] Add `src/engine/signal_engine.h/.cpp` to build the queue, skip tracked symbols, evaluate strategies, size positions, and place market plus TP/SL orders -> Verify: `test_signal_engine.cpp` uses mocks to cover no signal, low confidence, ATR zero, existing position, and successful open.
- [ ] Wire the new `.cpp` and `.h` files into `CMakeLists.txt` and extend `config.json` with scanner, engine, and strategies sections -> Verify: `cmake --build --preset windows-msvc-debug` succeeds.
- [ ] Add user-data-stream handling for filled TP/SL client order IDs to remove tracked positions -> Verify: a focused test feeds filled and non-filled events and only tracked TP/SL fills remove positions.
- [ ] Final verification: run `ctest --preset windows-msvc-debug`, then run a testnet smoke with one mock/low-risk strategy and confirm cache warm-up, one order path, TP/SL placement, and time-exit cleanup.

## Plugin Catalog (Phase P1–P4)

Sau khi framework tren on dinh, tiep tuc voi strategy plugin catalog theo `docs/design/2026-05-14-strategy-plugin-catalog-v1.0.md`.

- [ ] **P1** — Add `src/catalog/plugin_handle.h/.cpp` (RAII DLL loader) and `src/catalog/plugin_loader.h/.cpp` (folder scan + C ABI factory). Verify: `test_plugin_loader.cpp` with function-pointer fakes covers successful load, missing exports, and nullptr from createFn.
- [ ] **P2** — Add `src/catalog/strategy_catalog.h/.cpp`. Wire `PluginLoader` into `StrategyRegistry` via `"type"` field matching from `config.json`. Extend `config.json` with `catalog.plugins_dir` and `strategies[].type`. Verify: `test_strategy_catalog.cpp`.
- [ ] **P3** — Add `src/catalog/catalog_reporter.h/.cpp`. Wire `--list-strategies` into `main.cpp`. Wire `logStartupSummary` into startup sequence and `logRuntimeStatus` into `SignalEngine` scan cycle. Verify: `test_catalog_reporter.cpp`.
- [ ] **P4** — Write `docs/sdk/writing-a-strategy-plugin.md` and create a minimal template DLL project. Build first strategy as a real `.dll`, verify end-to-end load and signal path on testnet.

## Done When

- [ ] New modules match `docs/design/2026-05-14-market-scanner-strategy-engine-v1.0.md`.
- [ ] Plugin catalog matches `docs/design/2026-05-14-strategy-plugin-catalog-v1.0.md`.
- [ ] Existing `TradingEngine` remains available and existing tests still pass.
- [ ] New tests cover scanner cache, strategy interface, ATR, work queue, sizing, position tracking, signal engine behavior, plugin loader, catalog, and reporter.

## Notes

- Keep strategy implementations out of this plan except for mocks; concrete strategy work starts after this framework is stable.
- Preserve one-way-mode semantics: skip opening a new position when the symbol is already tracked.
- Do not rely on REST polling for recurring kline reads after scanner warm-up.
- Plugin catalog phases (P1–P4) depend on Phase D (SignalEngine) being complete first.
