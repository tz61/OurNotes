# OurNotes sync protocol (API v1)

OurNotes synchronises libraries through a *transport*: an ournotesd server over
HTTP(S) (`HttpTransport`) or a plain folder (`LocalTransport`, e.g. a USB drive
or NAS share). Both implement the same model; this document describes it and the
HTTP API. The C++ wire types live in `src/core/sync/protocol.h`.

## Data model

**Library** — identified by a client-chosen `uid` (1–64 characters of
`A-Z a-z 0-9 _ -`). It has a `name`, a `type` (`user` or `group`), an `owner`
and optional members with role `read` or `write`. Every library has a
**version** counter that starts at 0.

**Objects** — the library content, addressed by `(type, key)`:

| type         | content                         |
|--------------|---------------------------------|
| `item`       | bibliographic item, note, attachment |
| `collection` | collection                      |
| `search`     | saved search                    |
| `setting`    | synced setting                  |

`key` uses the same alphabet as `uid`. `data` is an arbitrary JSON object
(stored verbatim, key order preserved). Each object records the library version
at which it was last written. Deleting an object leaves a **tombstone**
(`deleted: true`, no data) so other clients learn about the deletion.

**Versions** — every push that writes at least one object increments the library
version by exactly one, and every object written by that push gets the new
version. A push that writes nothing (only conflicts, errors or repeated
deletions) leaves the version unchanged.

**Files** — at most one file per `(library, key)`, where `key` is the key of the
attachment item it belongs to. A file has `md5`, `size`, `mtime` (milliseconds
since the epoch), `filename` and `contentType`. Files are not versioned:
clients compare `md5` with their local copy. Deleting an `item` object deletes
its file; uploading a file for an item that is a tombstone is refused (409).

## Client algorithm

1. `ensureLibrary(uid, name, type)` — creates the library (caller becomes owner)
   or returns the existing one with the caller's `role`.
   - Names: the client stores the name it last agreed on with the target. If only
     the local name changed since, the owner sends it with `ensureLibrary`; if the
     target's changed (or both did, or the caller is not the owner), the local
     library takes the target's name.
   - A library the client synced with this target before that is no longer
     listed was deleted there (or is no longer shared): an empty local copy is
     removed, one with entries is uploaded again.
   - A fresh install's empty default library is not uploaded when the target
     already has a `user` library owned by the caller.
2. **Pull**: request `changes(since)` with `since` = the version stored after the
   last successful pull (0 at first). Follow `next` until it is `null`, applying
   every object. Then store the `version` of the **last** page as the new
   `since`. With `since = 0` tombstones are omitted (a fresh client has nothing
   to delete).
   - Pages are ordered by `(version, type, key)`. Objects changed while you page
     re-appear on a later page with their new version, so nothing is missed.
   - If a page reports `version < since`, the server lost history (e.g. it was
     restored from a backup): reset `since` to 0, pull everything and push your
     local state again.
3. **Push** local changes in batches of at most 1000 objects. `baseVersion` is the
   version the client last saw for that object (0 for a new object). Each entry
   of the result corresponds to the request entry at the same index:
   - `ok`: written; record `version` as the object's new version.
   - `conflict`: the server has a newer version (`current`, possibly a
     tombstone). Merge or pick a side, then push again with
     `baseVersion = current.version`.
   - `error`: the object is invalid (`message` says why); the rest of the batch
     is unaffected.
4. Do not advance `since` from push results: other clients may have written
   versions in between. Your own pushed objects come back in the next pull
   (their version is newer than `since`); skip those whose version you already
   recorded.
5. **Files**: compare `files(uid)` with local attachments by `md5`; upload missing
   or changed ones, download the others.

Pushes are not idempotent. If a push response is lost and the client retries,
already-written objects come back as `conflict` with `current.data` equal to
what was sent — treat that as success.

## HTTP API

All endpoints are under `<base>/api/v1`, where `<base>` is e.g.
`http://host:8642` or `https://example.org/ournotes` behind a reverse proxy.
Requests and responses are JSON (`application/json`) unless noted. Everything
except `/info` and `/login` needs `Authorization: Bearer <token>`.

Errors use the HTTP status and a body `{"error": "message"}`:

| status | meaning |
|--------|---------|
| 400 | malformed request, invalid key/cursor, checksum mismatch, push batch > 1000 |
| 401 | missing, invalid or revoked token; wrong password |
| 403 | read-only member writing; `PUT /libraries/{uid}` on someone else's library |
| 404 | unknown route, library missing or not accessible, file missing |
| 409 | upload for a deleted item; deleting a library that has entries |
| 413 | body larger than the server allows |
| 429 | too many failed logins from this address (see `Retry-After`) |
| 500, 507 | server error, disk full |

Limits: push ≤ 1000 objects and ≤ 64 MiB of JSON; `changes` returns at most
5000 objects per page (default 1000); uploads ≤ the server's `--max-upload-mb`
(default 1024).

### `GET /info` (no auth)

```json
{"server": "ournotesd", "version": "0.1.12", "api": 1}
```

### `POST /login` (no auth)

```json
{"username": "alice", "password": "secret", "device": "laptop (OurNotes 0.1)"}
```
→ `{"token": "64 hex chars", "username": "alice"}`. Tokens do not expire; they
are revoked by `/logout`, by a password change, or by deleting the user. After
10 failed attempts from one address within 10 minutes, logins from it get 429.

### `POST /logout`

Revokes the presented token → `{"ok": true}`.

### `GET /status`

How the server is doing, and what it holds of the caller's libraries (servers before
0.1.11 answer 404):
```json
{"server": "ournotesd", "version": "0.1.11", "api": 1, "uptime": 86400,
 "disk": {"capacity": 107374182400, "free": 53687091200, "available": 48318382080},
 "database": 12582912,
 "files": {"count": 1234, "bytes": 5368709120},
 "libraries": [{"uid": "LIB1", "name": "My Library", "type": "user", "role": "owner",
                "owner": "alice", "version": 42, "items": 310, "notes": 25,
                "attachments": 290, "annotations": 1800, "files": 280,
                "fileBytes": 1073741824}]}
```
`disk` is the disk the server's data is on (`null` if it cannot be told); `database`
and `files` are the server's, everyone's; the libraries' counts leave out what is
deleted.

### `GET /me`

→ `{"username": "alice", "admin": false}`

### `GET /libraries?state=S&wait=N`

Libraries the user owns or is a member of, and their `state`:
```json
{"libraries": [{"uid": "LIB1", "name": "My Library", "type": "user",
                "role": "owner", "owner": "alice", "version": 42}],
 "state": "9e107d9d372bb6826bd81d3542a419d6"}
```
`state` is an opaque fingerprint of the list: it changes whenever a library in it
does (a push that writes, a new name or role, a library added or removed).

With `state` and `wait` (seconds, at most 120) the server holds the request while
the state is still `S`, and answers as soon as it is not, or after `wait` seconds
with it unchanged. Clients keep such a request open (50 s, then again) to sync as
soon as another client has pushed. What a client pushed itself also changes the
state: it compares the versions with those it has pulled before syncing again. A
server with too many requests held answers at once; a client then waits a while
before asking again. Servers before 0.1.10 answer at once, without `state`.

### `PUT /libraries/{uid}`

Body `{"name": "My Library", "type": "user"}`. Creates the library owned by the
caller, or returns the existing one (the owner's `name` is updated if it
differs). → a library object as above. 403 if it exists and the caller has no
access. `GET /libraries/{uid}` returns the library without changing it.

### `DELETE /libraries/{uid}`

Deletes an empty library with its files (owner only) → `{"deleted": true}`. 409
while it has items or collections that are not deleted, 403 for members. Servers
before 0.1.3 answer 404 (unknown route): a client tells that apart from a missing
library by `GET /libraries` still listing it.

### `GET /libraries/{uid}/changes?since=N&limit=L&cursor=C`

```json
{"version": 42,
 "objects": [
   {"type": "item", "key": "ABCD2345", "version": 41, "data": {"itemType": "book", "title": "..."}},
   {"type": "item", "key": "EFGH6789", "version": 42, "deleted": true}],
 "next": "42.item.EFGH6789"}
```
`next` is an opaque cursor (pass it back unchanged with the same `since`), or
`null` on the last page.

### `POST /libraries/{uid}/push`

Needs role `owner` or `write`.
```json
{"objects": [
   {"type": "item", "key": "ABCD2345", "baseVersion": 41, "data": {"itemType": "book", "title": "New"}},
   {"type": "collection", "key": "COLL2345", "baseVersion": 0, "data": {"name": "Reading"}},
   {"type": "item", "key": "EFGH6789", "baseVersion": 40, "deleted": true}]}
```
→
```json
{"version": 43,
 "results": [
   {"type": "item", "key": "ABCD2345", "status": "ok", "version": 43},
   {"type": "collection", "key": "COLL2345", "status": "ok", "version": 43},
   {"type": "item", "key": "EFGH6789", "status": "conflict", "version": 42,
    "current": {"type": "item", "key": "EFGH6789", "version": 42, "data": {"...": "..."}}}]}
```
Conflict rule: an object conflicts when the server copy's version is greater
than `baseVersion`. Deleting an already deleted object is `ok` without a write.
The same key twice in one batch: the second entry conflicts with the first.

### `GET /libraries/{uid}/files`

```json
{"files": [{"key": "ATTA2345", "md5": "…", "filename": "paper.pdf",
            "contentType": "application/pdf", "size": 123456, "mtime": 1767225600000}]}
```

### `GET /libraries/{uid}/files/{key}`

The raw file. Headers: `Content-Type`, `Content-Length`, `X-OurNotes-MD5`,
`X-OurNotes-Mtime` (ms), `X-OurNotes-Filename` (percent-encoded UTF-8). Range
requests are supported. Clients should write to a temporary file and verify the
MD5 before using it.

### `PUT /libraries/{uid}/files/{key}?md5=&filename=&mtime=&contentType=`

The raw file as the request body (`Content-Length` required by proxies; clients
should send `Expect: 100-continue` for large files so rejected uploads are not
transmitted). All query parameters are optional and percent-encoded; if `md5` is
given the server verifies it (400 on mismatch). Replaces any existing file.
Needs role `owner` or `write`. → the stored file object (as in the list above).

### `DELETE /libraries/{uid}/files/{key}`

Needs role `owner` or `write`. → `{"deleted": true}` (`false` if there was no file).

## Local folder transport

A folder target contains the same store a server uses (`store.sqlite` with a
rollback journal instead of WAL, so it also works on network shares, plus
`files/` and `tmp/`). The transport acts as the user `local`, which owns every
library created through it. Several machines may use one folder; SQLite's file
locking serialises their pushes. Semantics, limits and conflict rules are
identical to the HTTP API; waiting for a change of the libraries' state looks at
the folder every 3 seconds.
