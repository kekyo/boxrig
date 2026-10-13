# Optional Microsoft Windows Console ConPTY package

The normal build and installed SDK use Windows-provided ConPTY and need no
Microsoft runtime files from this directory.

Run `make conpty-runtime-assets` only when explicitly testing or distributing
the packaged implementation. It downloads the unmodified signed
`Microsoft.Windows.Console.ConPTY.1.24.260710001.nupkg` into the generated build
cache and verifies the SHA-256 digest pinned in the top-level `Makefile` before
extracting it. The package is not part of the source or standard SDK distribution.
The application must explicitly select a packaged policy through the PTY API.

The DLL must match the application's architecture. The host executable must
match the operating system's architecture; for example, a 32-bit application
on x64 Windows needs the x86 DLL and x64 host. Keep the host in the appropriate
`x86`, `x64`, or `arm64` subdirectory alongside the DLL.

Source: [Microsoft's NuGet package](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.260710001).
Architecture requirement: [Microsoft's explanation](https://github.com/microsoft/terminal/discussions/20391).

License: MIT. When redistributing package files, include `LICENSE.conpty.txt`
and `NOTICE.conpty.md`, both prepared by `make conpty-runtime-assets`.
The license is copied from this directory's `LICENSE.txt`; the third-party
notice is preserved from the matching
[Windows Terminal v1.24.11911.0 release](https://github.com/microsoft/terminal/blob/v1.24.11911.0/NOTICE.md).
The upstream notice also covers other Windows Terminal configurations.
Its pinned source URL and SHA-256 are recorded in
[`packaging/licenses/sources.json`](../../packaging/licenses/sources.json).
See the [Japanese license notes](../../packaging/licenses/README_ja.md) and
their [English translation](../../packaging/licenses/README.md) for distribution conditions.
