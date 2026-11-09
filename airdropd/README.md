# airdropd

AirDrop-compatible receiver for "Everyone" mode. Specification: [`docs/PROTOCOL.md`](../docs/PROTOCOL.md). Decision record: [`docs/adr/0001-language-and-stack.md`](../docs/adr/0001-language-and-stack.md).

| Project | Content |
|---|---|
| `Mtdrop.Protocol` | Library, no dependencies beyond .NET: binary plist codec, HTTP/1.1 request reader (chunked, no `Host` required), dvzip and cpio odc stream decoders, safe file saving, TLS identity, `AirDropServer`, mDNS responder |
| `Mtdrop.Protocol.Tests` | xUnit tests. `FakeSender` plays the phone with synthetic data shaped like `docs/FINDINGS.md` F-002 |
| `Mtdrop.Airdropd` | Console host for development: `--list-interfaces`, `--interface <name-or-index>`, `--dir`, `--name`, `--yes` |

## Guarantees the tests pin down

- Nothing is written before the user accepts; an upload is tied to the accepted ask on the same connection and to its `TransferID`.
- Completion is decided by the archive trailer, so a withheld final chunk does not block; a stalled upload leaves no file and a retry works.
- Member names cannot escape the destination folder; links and special files are refused; AppleDouble sidecars are dropped; existing files are never overwritten.
- Malformed plists, chunks, dvzip blocks, cpio headers and DNS packets raise `ProtocolFormatException` or are ignored; they do not crash the service.

## Replaying a private capture

`MTDROP_PRIVATE_CAPTURE=<folder with flowN-c2s.bin from tools/capture/tls13_decrypt.py> dotnet test --filter Replays`

Captured data identifies the sender. It stays outside the repository.

## Not done yet

- Detecting that the sender cancelled while the prompt is open.
- Reusing an accept decision for the phone's automatic retry of the same files.
- Link shares, `application/x-cpio` (older senders).
- Decline status code is a guess (403): `docs/PROTOCOL.md` §10 item 1.
