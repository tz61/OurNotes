# Running an OurNotes sync server (ournotesd)

`ournotesd` keeps libraries, notes, annotations and attachment files in sync
between OurNotes installations. It is a single self-contained binary storing
everything in one data directory (SQLite + plain files). The protocol is
described in [sync-protocol.md](sync-protocol.md).

## Building only the daemon

The daemon needs neither Qt nor MuPDF; a C++17 compiler, CMake ≥ 3.21 and
(for built-in HTTPS) OpenSSL ≥ 3.0 are enough.

```sh
cmake -S . -B build -DOURNOTES_BUILD_GUI=OFF -DOURNOTES_BUILD_CLI=OFF
cmake --build build -j
ctest --test-dir build          # optional
sudo cmake --install build      # installs /usr/local/bin/ournotesd
```

The SQLite amalgamation is downloaded at configure time; for offline builds pass
`-DFETCHCONTENT_SOURCE_DIR_SQLITE=/path/to/sqlite-amalgamation`. Without
OpenSSL the build still works, but `--cert/--key` are unavailable (use a reverse
proxy for TLS). `-DOURNOTES_WITH_OPENSSL=OFF` forces that.

## Quick start

```sh
ournotesd useradd alice              # prompts for a password
ournotesd serve --port 8642
```

In OurNotes, add a sync target with the URL `http://SERVER:8642`, the user name
and the password. The client exchanges the password for a token and stores only
the token.

Without TLS, passwords and tokens cross the network in clear text: use plain
HTTP only on `localhost` or a trusted LAN, otherwise enable TLS (below).

## Commands

Every command accepts `--data DIR`. The default is `$OURNOTESD_DATA`, else
`/var/lib/ournotesd` when run as root, else `~/.local/share/ournotesd` (Linux,
or `$XDG_DATA_HOME/ournotesd`), `~/Library/Application Support/ournotesd` (macOS),
`%LOCALAPPDATA%\ournotesd` (Windows).

| command | |
|---|---|
| `serve [--host ADDR] [--port N] [--cert FILE --key FILE] [--max-upload-mb N] [--trusted-proxy ADDR]` | run the server (default `0.0.0.0:8642`, uploads up to 1024 MiB) |
| `useradd NAME [--admin] [--password PW]` | create a user; prompts twice without echo, or reads one line from a pipe |
| `passwd NAME [--password PW]` | change a password; revokes all of the user's tokens |
| `userdel NAME [--delete-libraries]` | delete a user (refused while they own libraries, unless the flag is given) |
| `users` | list users |
| `token NAME [--device D]` | print a new access token (for scripts or clients without a login dialog) |
| `libraries [--user NAME]` | list all libraries with their members, or those a user can access |
| `share UID USER [--role read\|write]` | give a user access to a library (default `write`) |
| `unshare UID USER` | remove access |
| `version` | print the version |

Administration commands work while the server is running. When run as root on
a data directory owned by another account (e.g. the `ournotesd` service user),
ournotesd switches to that account first, so `sudo ournotesd useradd bob` works
without leaving root-owned files behind.

`serve` logs one line per request to stdout
(`time address method path status user duration`) and shuts down cleanly on
SIGINT/SIGTERM (Ctrl-C on Windows). Failed logins are throttled per client
address (10 per 10 minutes).

## TLS

### Built-in

```sh
ournotesd serve --cert /etc/ournotesd/fullchain.pem --key /etc/ournotesd/privkey.pem --port 8642
```

The certificate file may contain the full chain. Restart the server after
renewing certificates.

### Reverse proxy (recommended for public servers)

Run ournotesd on localhost and let the proxy terminate TLS (and renew
certificates). `--trusted-proxy 127.0.0.1` makes ournotesd use the proxy's
`X-Forwarded-For` header for login throttling and logs.

```sh
ournotesd serve --host 127.0.0.1 --port 8642 --trusted-proxy 127.0.0.1
```

**Caddy** (automatic HTTPS, no body size limit by default):

```
sync.example.org {
    reverse_proxy 127.0.0.1:8642
}

# or under a path prefix; clients then use https://example.org/ournotes
example.org {
    handle_path /ournotes/* {
        reverse_proxy 127.0.0.1:8642
    }
}
```

**nginx**:

```nginx
server {
    listen 443 ssl;
    server_name sync.example.org;
    ssl_certificate     /etc/letsencrypt/live/sync.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/sync.example.org/privkey.pem;

    client_max_body_size 1024m;      # at least ournotesd's --max-upload-mb
    proxy_request_buffering off;     # stream uploads instead of spooling them
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    location / {                     # or: location /ournotes/ { proxy_pass http://127.0.0.1:8642/; ... }
        proxy_pass http://127.0.0.1:8642;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

With a path prefix the proxy must strip it (the trailing `/` in `proxy_pass`
does that for nginx, `handle_path` for Caddy); ournotesd always serves
`/api/v1/...`.

## systemd

`packaging/ournotesd.service` runs the server as the system user `ournotesd`
with its data in `/var/lib/ournotesd`, on localhost behind a reverse proxy, with
a strict sandbox (`ProtectSystem=strict`, no capabilities, system-call filter).

```sh
sudo useradd --system --home-dir /var/lib/ournotesd --shell /usr/sbin/nologin ournotesd
sudo install -m 644 packaging/ournotesd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ournotesd
sudo ournotesd useradd alice
journalctl -u ournotesd -f
```

If ournotesd is installed elsewhere than `/usr/local/bin`, or should serve TLS
itself, override `ExecStart` with `sudo systemctl edit ournotesd`. Certificates
readable only by root can be passed in with `LoadCredential=`:

```ini
[Service]
LoadCredential=cert:/etc/letsencrypt/live/sync.example.org/fullchain.pem
LoadCredential=key:/etc/letsencrypt/live/sync.example.org/privkey.pem
ExecStart=
ExecStart=/usr/local/bin/ournotesd serve --data /var/lib/ournotesd --port 8642 \
    --cert ${CREDENTIALS_DIRECTORY}/cert --key ${CREDENTIALS_DIRECTORY}/key
```

## Data directory and backups

```
store.sqlite        users, token hashes, libraries, members, objects (WAL mode)
store.sqlite-wal    } SQLite write-ahead log, part of the database
store.sqlite-shm    }
files/<uid>/<key>   attachment files (lowercase letters are stored as ".x")
tmp/                uploads in progress
```

Passwords are stored as salted PBKDF2-SHA256 hashes and tokens only as SHA-256
hashes; still, keep the directory private. ournotesd runs with umask 077, so
everything it creates is accessible only to its own user.

**Simple backup** — stop the server, copy the directory, start it again:

```sh
sudo systemctl stop ournotesd
sudo tar -C /var/lib -czf ournotesd-$(date +%F).tar.gz ournotesd
sudo systemctl start ournotesd
```

**Online backup** — snapshot the database first, then copy the files:

```sh
sudo -u ournotesd sqlite3 /var/lib/ournotesd/store.sqlite ".backup '/backup/ournotesd/store.sqlite'"
sudo rsync -a --delete /var/lib/ournotesd/files/ /backup/ournotesd/files/
```

Never copy `store.sqlite` alone while the server runs (the WAL holds recent
commits). Files changed between the two steps simply get re-uploaded by
clients; a file whose checksum no longer matches is detected on download.

**Restoring** an older backup rolls the library versions back. Clients notice
(the server reports a version lower than theirs), download everything again and
re-upload their newer changes, so nothing is lost as long as the clients still
have them.
