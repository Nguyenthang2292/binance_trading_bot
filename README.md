# binance_trading_bot

Codex project initialized in WSL at /home/devuser/binance_trading_bot.

## Windows Development Setup

This workspace is configured for local Windows development in VS Code.

Recommended prerequisites:

1. Visual Studio 2022 Build Tools or Visual Studio Community with the C++ workload.
2. CMake 3.23 or newer.
3. Ninja, if you want to use the Ninja preset.
4. Git.

Open the repository in VS Code and choose one of the CMake presets:

1. `windows-msvc-debug` for the Visual Studio generator.
2. `windows-ninja-debug` for a single-config Ninja build.

The workspace also includes ready-made VS Code tasks for configure, build, and test, plus a debug launch configuration for the main executable.

## Runtime Environment

The bot requires:

1. `BINANCE_API_KEY`
2. `BINANCE_SECRET_KEY`

Optional:

1. `BINANCE_TESTNET=1` to use Binance Futures testnet.
2. `BINANCE_SOCKS5_PROXY` to route all REST + WebSocket traffic via SOCKS5 proxy (for example EC2 SSH tunnel):
   - `socks5://127.0.0.1:1080`
   - `socks5h://127.0.0.1:1080`

If `BINANCE_SOCKS5_PROXY` is not set, the bot also checks `ALL_PROXY` and `all_proxy`.
Set `BINANCE_REQUIRE_SOCKS5_PROXY=1` when local runs must fail instead of bypassing the EC2 tunnel.
