## Downloads

| System | File | |
|---|---|---|
| macOS, Apple silicon | `OurNotes-*-macos-arm64.dmg` | open it and drag OurNotes to Applications |
| macOS, Intel | `OurNotes-*-macos-x86_64.dmg` | |
| Windows 10/11 | `OurNotes-*-windows-x64.zip` | unzip, then run `OurNotes\bin\ournotes.exe` |
| Linux | `OurNotes-*-x86_64.AppImage` | `chmod +x` it, then run it |
| Linux (folder) | `ournotes-*-linux-x86_64.tar.gz` | `bin/ournotes`, `bin/ournotes-cli`, `bin/ournotesd` |

**macOS:** the app is not notarized by Apple. On first start macOS refuses to open it;
choose *System Settings → Privacy & Security → Open Anyway*, or run
`xattr -dr com.apple.quarantine /Applications/OurNotes.app`. `ournotes-cli` and
`ournotesd` are in `OurNotes.app/Contents/MacOS/`.

**Linux:** the packages use the system's OpenSSL 3 (`libssl3`, present on Ubuntu 22.04,
Debian 12, Fedora 36 and newer).

`SHA256SUMS.txt` lists the checksums of all files.
