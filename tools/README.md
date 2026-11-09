# tools

Python helpers. Development only; nothing here is shipped to users.

| Path | Purpose |
|---|---|
| `proto/airdrop_codec.py` | Reference codecs (chunked, dvzip, cpio odc, safe member names) written from `docs/PROTOCOL.md`; `test_airdrop_codec.py` is the executable specification |
| `capture/tls13_decrypt.py` | Decrypt TLS 1.3 sessions from an `awdl0` capture with an NSS key log |
| `capture/http_shape.py` | Print the *shape* of decrypted exchanges with identifying values masked |
| `capture/tcp_stats.py` | Per-connection throughput, retransmissions and gaps |
| `capture/radiotap_stats.py` | Frame, channel and signal statistics of a radiotap capture |
| `m0/` | Scripts for the Linux proof (M0) |

Inputs and outputs of the `capture/` tools are private data. Keep them outside the repository.
