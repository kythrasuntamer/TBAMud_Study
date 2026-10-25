# Encrypted MUD connections (TLS 1.3)

tbaMUD speaks plaintext Telnet. The supported initial deployment is an
external TLS 1.3 terminator (stunnel) forwarding to a **loopback-only**
tbaMUD backend. **Do not expose backend port 5000 to the Internet.**

## Threat model and algorithms

- Network traffic between a TLS-capable MUD client and stunnel is encrypted
  and authenticated with standardized TLS 1.3.
- TLS 1.3 uses modern authenticated encryption: AES-256-GCM or
  ChaCha20-Poly1305 (AES-128-GCM fallback). Ephemeral key exchange gives
  forward secrecy in normal TLS 1.3 handshakes.
- Serpent is **not** a standardized TLS 1.3 cipher suite. Adding a custom
  Serpent protocol or layering ciphers would break interoperability and
  create unnecessary cryptographic risk. Do not claim this deployment uses
  Serpent.
- The short loopback hop from stunnel to tbaMUD is still plaintext. Protect
  the local host; don't move the proxy to another host without re-encrypting
  the backend link.
- The existing C server still needs memory-safety, input-validation,
  authentication and resource-exhaustion defenses.

## Installation

On Ubuntu/Debian: `sudo apt install stunnel4 openssl`.

Obtain a valid certificate for your public DNS hostname from a trusted CA
(for example via ACME/Let's Encrypt), and install its full chain and private
key at the two paths in `deploy/stunnel/tbamud.conf`. Grant stunnel
read access to the key but **never commit the private key**.

Start the game *from the repository root* with:
```sh
TBAMUD_TLS_BACKEND_ONLY=1 ./bin/circle -q 5000
```
This environment flag forces the game listener to 127.0.0.1 regardless
of any `dflt_ip` override. Verify with:
```sh
ss -ltnp | grep ':5000'
```
It must say **127.0.0.1:5000**, not `0.0.0.0:5000`.

In another terminal, run:
```sh
sudo stunnel deploy/stunnel/tbamud.conf
```
Ensure the TLS entrypoint 5001 is the only publicly reachable game port.
Also use firewall rules to deny public access to 5000 as defense in depth.
Configure the service manager to always set `TBAMUD_TLS_BACKEND_ONLY=1`;
otherwise the existing default remains public binding.

## Verify encryption and identity

```sh
openssl s_client -connect YOUR_PUBLIC_HOST:5001 \
  -servername YOUR_PUBLIC_HOST -verify_hostname YOUR_PUBLIC_HOST \
  -verify_return_error -tls1_3
```
A successful TLS handshake with a verified, hostname-matching certificate
is necessary, but **not sufficient** to prove that a MUD client verifies
certificates. Use a TLS-capable MUD client that validates the hostname and
CA chain; ordinary plaintext Telnet clients cannot connect to this port.

A real TLS client should see a normal MUD greeting after the handshake.
Test authentication with a disposable account and verify that 5000 is
unreachable from another machine. Do not test password migration on live
player data until persistence safeguards are validated.

## Compatibility

Port 5001 requires TLS 1.3. Old plaintext clients must be upgraded or
use a separate explicitly designated and isolated legacy service. Avoid
silently downgrading authentication to plaintext. Reverse-proxy connections
will appear to the backend as 127.0.0.1: check and adapt IP-based rate
limiting carefully before public production deployment. This config does
not restore the original client IP to the game.

## Stronger-than-classical key exchange

If stunnel is linked against **OpenSSL 3.5 or newer**, its default TLS 1.3
key-exchange groups can offer the hybrid **X25519MLKEM768** exchange when
clients also support it. This combines classical X25519 and the ML-KEM
post-quantum mechanism, rather than layering unrelated bulk ciphers.
Check the negotiated group in an actual TLS handshake; do not claim
post-quantum protection without verifying negotiation and library support.

There is no standardized 512-bit Serpent or 512-bit AES TLS 1.3 cipher suite.
Serpent itself has a 128-bit block size and supports at most 256-bit keys.
A stack of different 256-bit ciphers does **not** add their advertised key
lengths or guarantee more security. Prefer modern, audited AEAD algorithms.
