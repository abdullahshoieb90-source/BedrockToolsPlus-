# Voice Chat

BedrockToolsPlus includes encrypted **room voice chat** and **proximity voice chat** for Android players using LeviLaunchroid. It supports a hold-to-talk screen button, a configurable keyboard key, open-microphone mode, independent input/output volume, and a movable connection-status HUD.

The feature is a native overlay; it does not use Minecraft chat packets and does not require a Minecraft server plugin. Every player who wants to speak or hear voice must run a compatible BedrockToolsPlus build. All players in a call must use the **same relay address, room name, and room key**.

## Requirements

- Android 9 or newer, ARM64.
- LeviLaunchroid **1.6.1 or newer** for the optional microphone permission.
- BedrockToolsPlus with the Voice Chat module.
- A reachable BedrockToolsPlus UDP voice relay. No public relay is provided.
- Microphone permission enabled manually for LeviLauncher.

Voice is 16 kHz mono PCM in 20 ms frames. It uses approximately **36 KB/s (about 288 kbit/s) per speaking player**, before network overhead. This avoids device-specific codec support, but uses more bandwidth than an Opus-based implementation. Each listener receives every other active speaker's stream, so small rooms are recommended.

## 1. Start the relay

The relay uses only the Python 3 standard library; there are no packages to install. Copy `tools/voicechat_relay.py` to a LAN computer, server host, or VPS and run:

```sh
python3 voicechat_relay.py --host 0.0.0.0 --port 39741
```

Allow inbound **UDP 39741** in the firewall. For Internet play, use a host with a public IP or configure UDP port forwarding. Players connect outward to the relay, so players themselves do **not** need port forwarding.

Useful options:

```text
--max-room-clients 32
--max-total-clients 512
--idle-timeout 15
--packets-per-second 80
--log-level INFO
```

The relay expires idle clients, isolates rooms, limits packet rates, validates client IDs and datagram sizes, and forwards encrypted voice unchanged. It cannot decrypt voice or read the positions inside voice packets.

## 2. Generate and share a room key

Use a long random key, not a short password. For example:

```sh
python3 -c 'import secrets; print(secrets.token_urlsafe(24))'
```

Share it with the other players through a private channel. The key must be at least 16 bytes; a generated 24-byte URL-safe token is recommended.

Voice uses **XChaCha20-Poly1305** end-to-end authenticated encryption. The room key is combined with the room name to derive the encryption key. The relay receives the room name and player name, but never receives the room key. A wrong key therefore produces silence rather than playable corrupted audio.

The key is stored in the normal BedrockToolsPlus `config.json` and is visible in the module's text setting. Treat that file and screenshots of the settings as sensitive. Encryption protects voice in transit; it does not protect against a player in the room recording the call or against a compromised device.

## 3. Configure each player

Open **BedrockToolsPlus → Voice Chat** in LeviLauncher's mod menu:

| Setting | Meaning |
| --- | --- |
| **Server** | Relay host and optional UDP port, for example `192.168.1.20:39741`, `voice.example.com:39741`, or `[2001:db8::1]:39741`. Without a port, 39741 is used. |
| **Room** | Room identifier, up to 64 UTF-8 bytes. Use different rooms for calls that must remain separate. |
| **Room Key** | The same random secret on every device. Never post it publicly. |
| **Player Name** | Optional display name sent to the relay. Leave blank to use the current Minecraft name. |
| **Mode** | `Room`: everyone in the room hears at full configured volume. `Proximity`: volume falls with 3D distance and reaches zero at **Max Distance**. |
| **Push To Talk** | On: capture only while holding **TALK** or **Talk Keybind**. Off: capture continuously while connected and the module is enabled. |
| **Talk Keybind** | Keyboard push-to-talk key; default is Android `V` (key code 50). |
| **Max Distance** | Proximity cutoff, 4–128 blocks. Full volume applies within two blocks, then attenuates linearly. |
| **Input Volume / Output Volume** | Microphone gain and received-voice volume. |
| **Show Status** | Shows connection, participant, speaker and microphone state in a movable HUD element. |

Enable the module after entering the connection settings. Connection changes are applied after a short debounce so typing a hostname does not create a connection for every character.

On Android, open **Settings → Apps → LeviLauncher → Permissions → Microphone** and select **Allow only while using the app**. Then disable and re-enable Voice Chat (or hold Talk again) if the first attempt was denied. The microphone object is created only while capture is active; releasing Talk stops and releases it rather than merely discarding samples.

### Proximity rooms and worlds

The current protocol authenticates 3D positions but does not identify a Minecraft server, world, or dimension. **Use a different room for each world/dimension call** when players may be in different worlds. Otherwise two players at similar coordinates in different worlds could hear each other in Proximity mode. Room mode intentionally ignores positions.

## Troubleshooting

- **Microphone permission is off:** grant it to LeviLauncher, not to a separate BedrockToolsPlus app. Then retry Talk or toggle the module.
- **Stuck on Joining/Connecting:** check the relay process, hostname, UDP port, firewall, and that the address is reachable from the phone. Use the computer's LAN IP rather than `localhost` when the relay runs on another device.
- **Connected but nobody can hear:** confirm the same room and exact room key, enable the microphone permission, and check input/output and Android media volume. Authentication failures are dropped intentionally.
- **Connected participant count is 1:** no other compatible client is currently joined to that room on that relay.
- **Choppy audio:** Wi-Fi/mobile jitter, Bluetooth latency, or a congested relay can exceed the small jitter buffer. Use a closer relay and fewer simultaneous speakers.
- **Echo:** use headphones. `VOICE_COMMUNICATION` requests device echo cancellation and noise suppression, but availability depends on the Android device.
- **After changing worlds:** move to the agreed world-specific room if using Proximity mode.

## Protocol and implementation notes

- UDP relay port: `39741` by default.
- Packet magic/version: `BTVC` / `1`.
- Capture/playback: Android `AudioRecord` / `AudioTrack`, 16 kHz mono signed PCM16.
- Frame size: 320 samples / 20 ms.
- Voice encryption: Monocypher XChaCha20-Poly1305; packet headers are authenticated as additional data.
- Replay handling: per-sender monotonic sequence plus a random 128-bit session nonce prefix.
- Jitter: up to eight queued frames per speaker, starts playback after three frames, clamps mixed PCM to avoid integer overflow.
- Relay routing: endpoint-affine client IDs, no sender echo, no plaintext voice or position access.

Host-side checks:

```sh
./scripts/run_tests.sh
python3 -m unittest -v tests/test_voicechat_relay.py
```

Host tests validate the wire format, authentication/tamper rejection, PCM clamping, attenuation, relay routing and room isolation. They do not verify a physical microphone, Android audio routing, Minecraft offsets, or real-device network conditions; test those on the intended devices before a release.
