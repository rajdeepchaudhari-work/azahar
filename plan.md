# Web Controller Implementation Plan

A complete record of how the web controller feature was built for the Azahar (Citra fork) 3DS emulator.
Use this to re-implement on a fresh clone.

---

## What it does

- Starts an HTTP server (port 9753) when a game boots
- The status bar shows a clickable URL like `http://192.168.1.x:9753`
- Open that URL on your phone's browser → full 3DS virtual controller
- Buttons, D-pad, Circle pad, and touch screen all work
- Every widget is draggable and resizable; layout persists in `localStorage`
- EDIT / DONE / RESET buttons in the corner to manage layout

---

## Files to create (new)

### `src/input_common/web_input/web_input.h`

```cpp
// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include "common/common_types.h"

namespace Frontend {
class EmuWindow;
} // namespace Frontend

namespace InputCommon {
class Keyboard;
} // namespace InputCommon

namespace InputCommon::WebInput {

// Runs an HTTP server that serves a 3DS virtual controller page.
// The page is designed to be opened on a phone over local Wi-Fi.
// Button presses call keyboard->PressKey/ReleaseKey using the current
// profile's key bindings. Touch input calls EmuWindow::TouchPressed/Moved/Released.
class Server {
public:
    Server();
    ~Server();

    // Start listening on the given port (0.0.0.0). Returns false if bind fails.
    bool Start(Keyboard* keyboard, Frontend::EmuWindow* window, u16 port = 9753);
    void Stop();
    bool IsRunning() const;

    // Returns the bound port (useful if port 0 was requested).
    u16 GetPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace InputCommon::WebInput
```

---

### `src/input_common/web_input/web_input.cpp`

Full implementation. Key design points:

**Impl struct members:**
- `Keyboard* keyboard` — pointer set at `Start()` time
- `Frontend::EmuWindow* emu_window` — pointer set at `Start()` time
- `httplib::Server svr` — cpp-httplib server (already a dependency in externals)
- `std::thread server_thread`
- `std::atomic<bool> running{false}`
- `u16 bound_port{0}`
- `std::unordered_map<int, int> button_key_map` — NativeButton index → keyboard key code
- `int circle_up_key, circle_down_key, circle_left_key, circle_right_key`
- `bool circle_up_pressed, circle_down_pressed, circle_left_pressed, circle_right_pressed`

**LoadKeyMappings():**
- Iterates `Settings::values.current_input_profile.buttons[]`
- For each entry: parse with `Common::ParamPackage`, check `engine == "keyboard"`, extract `code`
- For circle pad: parse `profile.analogs[Settings::NativeAnalog::CirclePad]` with engine `analog_from_button`, then parse sub-packages for up/down/left/right

**Routes (SetupRoutes):**
- `GET /` → serve the HTML string `CONTROLLER_HTML`
- `POST /input` → parse JSON body, dispatch to HandleButton / HandleTouch / HandleTouchMove / HandleAnalog
- `OPTIONS /.*` → CORS preflight headers
- Pre-routing handler adds `Access-Control-Allow-Origin: *` to all responses

**HandleButton(name, pressed):**
- Static map: `"A"→NativeButton::A`, `"B"→B`, `"X"→X`, `"Y"→Y`, `"Up"→Up`, `"Down"→Down`, `"Left"→Left`, `"Right"→Right`, `"L"→L`, `"R"→R`, `"Start"→Start`, `"Select"→Select`, `"ZL"→ZL`, `"ZR"→ZR`, `"Home"→Home`
- Look up key code from `button_key_map`, call `keyboard->PressKey(code)` or `ReleaseKey(code)`

**HandleTouch(x, y, pressed):**
- `x` and `y` are 0..1 floats (normalised)
- Multiply: `fx = unsigned(x * 320)`, `fy = unsigned(y * 240)` — 3DS bottom screen is 320×240
- Call `emu_window->TouchPressed(fx, fy)` or `emu_window->TouchReleased()`

**HandleTouchMove(x, y):**
- Same scaling, call `emu_window->TouchMoved(fx, fy)`

**HandleAnalog(ax, ay):**
- Dead zone `DEAD = 0.3f`
- Converts float axis to key press/release using press_release lambda
- `ax > DEAD` → right; `ax < -DEAD` → left; `ay > DEAD` → up; `ay < -DEAD` → down
- Releases both axis keys when within dead zone

**ReleaseAll():**
- Called on Stop() — releases every key in `button_key_map`, all circle pad direction keys, calls `emu_window->TouchReleased()`

**Server::Start():**
1. If already running, call Stop() first
2. Set `impl->keyboard` and `impl->emu_window`
3. Call `LoadKeyMappings()` and `SetupRoutes()`
4. `svr.bind_to_port("0.0.0.0", port)` — returns false on failure
5. Set `impl->running = true`
6. Spawn `server_thread` that calls `svr.listen_after_bind()`

**Server::Stop():**
1. Return early if `!impl->running`
2. Call `ReleaseAll()`
3. Call `svr.stop()`
4. Join `server_thread`
5. Set `impl->running = false`

**HTML page — `CONTROLLER_HTML` (raw string literal):**
- `touch-action: none` on `*` to prevent scroll interference
- `body`: `100vw × 100dvh`, `overflow: hidden`, `position: relative`
- EDIT/DONE button: fixed bottom-right, `z-index:300`, toggles `.ed` class on all `.w` elements
- RESET button: fixed bottom-left, hidden unless in edit mode, restores `DEFS` layout
- Widget class `.w`: `position:absolute; transform-origin:top left`
- Widget class `.w.ed`: dashed outline, shows `.rh` (resize handle)
- `.rh`: absolutely positioned `bottom:-15px; right:-15px`, 30×30 circle, `cursor:nwse-resize`

**Widgets (each is a `<div class="w" id="wX">...</div>`):**
| id  | Contents              | Default position `{x%, y%, scale}` |
|-----|-----------------------|--------------------------------------|
| wl  | L shoulder button     | `{1, 2, 1}`                          |
| wr  | R shoulder button     | `{88, 2, 1}`                         |
| wd  | D-pad                 | `{1, 24, 1}`                         |
| wc  | Circle pad            | `{2, 57, 1}`                         |
| wt  | Touch screen          | `{21, 8, 1.2}`                       |
| wf  | Face buttons (A/B/X/Y)| `{79, 24, 1}`                        |
| wb  | Select + Start        | `{40, 87, 1}`                        |

**Layout persistence (`localStorage` key: `ctr3ds`):**
- On load: `JSON.parse(localStorage.getItem('ctr3ds') || 'null') || JSON.parse(JSON.stringify(DEFS))`
- `applyLayout()`: sets `el.style.left = p.x+'%'`, `el.style.top = p.y+'%'`, `el.style.transform = 'scale('+p.s+')'`
- Saved after every drag end and resize end

**Drag logic (per widget):**
- `pointerdown` on widget (not resize handle): `setPointerCapture`, record `sx,sy,slx,sly`
- `pointermove`: `layout[id].x = slx + (e.clientX-sx)/innerWidth*100`; same for y; update `style`
- `pointerup`: save layout

**Resize logic (per widget's `.rh` handle):**
- `pointerdown` on `.rh`: record initial distance from widget origin and initial scale
- `pointermove`: compute new distance, `s = Math.max(0.35, Math.min(3, rs0 * d / rd0))`; update `style.transform` and `layout[id].s`
- `pointerup`: save

**Input sending:**
- All inputs use `fetch('/input', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(data), keepalive:true }).catch(()=>{})`
- `{type:'button', name:'A', pressed:true/false}`
- `{type:'touch', x:0..1, y:0..1, pressed:true/false}`
- `{type:'touch_move', x:0..1, y:0..1}` — sent on `pointermove` over touch screen
- `{type:'analog', name:'circle', x:-1..1, y:-1..1}`

**Touch screen widget HTML:**
```html
<div class="w" id="wt">
  <div id="tlbl">TOUCH SCREEN</div>
  <div id="tp">
    <div id="td"></div>
  </div>
  <div class="rh">⤡</div>
</div>
```
CSS: `#tp { width:320px; height:240px; background:#0a0a0a; cursor:crosshair; }`
`#td` is the finger-position dot (16×16 circle, `display:none` until touch)

**Circle pad HTML:**
```html
<div class="w" id="wc">
  <div id="cp"><div id="cpt"></div></div>
  <div class="rh">⤡</div>
</div>
```
`#cp`: 96×96 circle, `#cpt`: 52×52 inner thumb nub moved to show axis position

---

## Files to modify (existing)

### `src/input_common/CMakeLists.txt`

Add to the sources list:
```cmake
web_input/web_input.cpp
web_input/web_input.h
```

In `target_link_libraries` for `input_common`, ensure `httplib` and `json-headers` are linked:
```cmake
target_link_libraries(input_common PUBLIC citra_core PRIVATE citra_common ${Boost_LIBRARIES} httplib json-headers)
```
(`httplib` and `json-headers` are already in `externals/` — just need to be listed here.)

---

### `src/input_common/main.h`

Add forward declaration and function declaration:
```cpp
namespace InputCommon::WebInput {
class Server;
} // namespace InputCommon::WebInput
```
And in the `InputCommon` namespace:
```cpp
/// Gets the web input server (created during Init, must call Start to activate it).
WebInput::Server* GetWebInputServer();
```

---

### `src/input_common/main.cpp`

Add include at top:
```cpp
#include "input_common/web_input/web_input.h"
```

Add global variable:
```cpp
static std::unique_ptr<WebInput::Server> web_input_server;
```

In `Init()`, after `udp = CemuhookUDP::Init();`:
```cpp
web_input_server = std::make_unique<WebInput::Server>();
```

In `Shutdown()`, after `udp.reset();`:
```cpp
web_input_server.reset();
```

Add function at the bottom (before the `Polling` namespace):
```cpp
WebInput::Server* GetWebInputServer() {
    return web_input_server.get();
}
```

---

### `src/citra_qt/citra_qt.h`

Add to private methods:
```cpp
void StartWebController();
void StopWebController();
```

---

### `src/citra_qt/citra_qt.cpp`

Add include near the other input_common includes:
```cpp
#include "input_common/web_input/web_input.h"
```

In `BootGame()`, near the end after game starts successfully, call:
```cpp
StartWebController();
```

In `ShutdownGame()`, near the top (before emulation stops), call:
```cpp
StopWebController();
```

Add the two new methods:
```cpp
void GMainWindow::StartWebController() {
    auto* server = InputCommon::GetWebInputServer();
    if (!server)
        return;

    constexpr u16 WEB_CONTROLLER_PORT = 9753;
    if (!server->Start(InputCommon::GetKeyboard(), render_window, WEB_CONTROLLER_PORT)) {
        LOG_WARNING(Frontend, "Web controller: failed to start server on port {}",
                    WEB_CONTROLLER_PORT);
        return;
    }

    // Find the best local IPv4 address to show the user.
    QString local_ip = QStringLiteral("localhost");
    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : ifaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack))
            continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                local_ip = entry.ip().toString();
                break;
            }
        }
        if (local_ip != QStringLiteral("localhost"))
            break;
    }

    const QString url =
        QStringLiteral("http://%1:%2").arg(local_ip).arg(server->GetPort());
    LOG_INFO(Frontend, "Web controller started: {}", url.toStdString());

    message_label->setText(
        tr("Web Controller: <a href=\"%1\">%1</a>  (open on your phone)").arg(url));
    message_label->setOpenExternalLinks(true);
}

void GMainWindow::StopWebController() {
    auto* server = InputCommon::GetWebInputServer();
    if (server && server->IsRunning()) {
        server->Stop();
        LOG_INFO(Frontend, "Web controller stopped");
    }
    if (message_label)
        message_label->clear();
}
```

`QNetworkInterface` requires `#include <QNetworkInterface>` — add it with the other Qt headers.
`Qt6::Network` must be linked in `src/citra_qt/CMakeLists.txt` (it already is in the base project).

---

## Dependencies already available in the repo

| Library | Path | CMake target |
|---------|------|--------------|
| cpp-httplib | `externals/cpp-httplib/` | `httplib` |
| nlohmann/json | `externals/json/` | `json-headers` |
| Qt6::Network | system Qt | `Qt6::Network` |

No new submodules or downloads required.

---

## Build

```bash
# From repo root — standard Azahar build procedure
cmake -B build -DCMAKE_BUILD_TYPE=Release   # (or whatever flags you normally use)
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

The binary ends up at `build/bin/Release/azahar.app/Contents/MacOS/azahar` on macOS.

---

## How to use

1. Launch the app and start a game
2. Look at the bottom status bar — it shows `Web Controller: http://192.168.x.x:9753`
3. Open that URL on your phone (must be on same Wi-Fi)
4. Play using the on-screen controller
5. Tap **EDIT** to drag/resize widgets; tap **DONE** when finished; layout is saved automatically

---

---

## Part 2 — Encrypted .3ds / .cci Game Support

### Background

`.3ds` and `.cci` files are NCSD containers. Each NCSD holds up to 8 NCCH partitions. The main
game partition (index 0) is often AES-CTR encrypted with retail keys. Currently, `ncch_container.cpp`
immediately returns `ErrorEncrypted` when `ncch_header.no_crypto == 0`.

The **complete decryption logic already exists** in `src/core/hle/service/am/am.cpp` in the
`NCCHCryptoFile` class — it is used when installing encrypted CIA files. The task is to extract
that same pattern into `NCCHContainer` so that loading a `.3ds` file decrypts sections on the fly
as they are read into memory.

**Critical safety rule:** decrypt only in-memory buffers **after** the file read, always on the
same thread that called `Load()`/`LoadSectionExeFS()`. Do **not** modify the file on disk, do
**not** use callbacks or background threads, do **not** touch bootmanager or the renderer.

---

### How NCCH encryption works

```
KeyY (primary)   = first 16 bytes of ncch_header.signature
KeyY (secondary) = same as primary, unless seed_crypto flag is set
                   (seed crypto: SHA256(KeyY_primary || seed)[0:16])

primary NormalKey   = scramble(KeyX[0x2C], KeyY_primary)
secondary NormalKey = scramble(KeyX[secondary_key_slot], KeyY_secondary)
  secondary_key_slot values:
    0  → slot 0x2C  (NCCHSecure1)
    1  → slot 0x25  (NCCHSecure2)
   10  → slot 0x18  (NCCHSecure3)
   11  → slot 0x1B  (NCCHSecure4)

Special case: fixed_key flag → both keys are all-zeros (homebrew/prototype)

CTR derivation depends on ncch_header.version:
  version 0 or 2 (most retail games):
    base[0:8]  = reverse(partition_id)
    base[8]    = section_index (1=ExHeader, 2=ExeFS, 3=RomFS)
    base[9:16] = 0x00

  version 1 (older titles):
    base[0:8]  = partition_id (not reversed)
    base[8:12] = 0x00
    base[12:16] = big-endian u32 of section file offset
      ExHeader offset = 0x200
      ExeFS offset    = ncch_header.exefs_offset * 0x200
      RomFS offset    = ncch_header.romfs_offset * 0x200

Decryption key usage per section:
  ExHeader            → primary key + exheader_ctr
  ExeFS header        → primary key + exefs_ctr
  ExeFS "icon"/"banner" sections → primary key + exefs_ctr (seek to offset within stream)
  ExeFS all other sections → secondary key + exefs_ctr (seek to offset within stream)
  RomFS               → secondary key + romfs_ctr
```

The scrambler formula (hardware emulated in software):
```
NormalKey = ROL128( ADD128( XOR128( ROL128(KeyX, 2), KeyY ), generator_constant ), 87 )
```
This is already implemented in `src/core/hw/aes/key.cpp` via `SetKeyY()` + `GetNormalKey()`.

---

### Keys must exist first (`aes_keys.txt`)

The KeyX values for slots 0x2C, 0x25, 0x18, 0x1B are not built into the emulator by default
(they are console-specific secrets). The user must place an `aes_keys.txt` file in the Azahar
user data directory (`~/.local/share/azahar-emu/` on Linux, `~/Library/Application Support/Azahar/`
on macOS, `%APPDATA%\Azahar\` on Windows).

`HW::AES::InitKeys()` (in `key.cpp`) reads this file. It is called automatically — just ensure
it has been called before attempting to derive keys.

Example `aes_keys.txt` lines (values are dumped from a real 3DS):
```
slot0x2CKeyX=<32 hex chars>
slot0x25KeyX=<32 hex chars>
slot0x18KeyX=<32 hex chars>
slot0x1BKeyX=<32 hex chars>
```

If `IsNormalKeyAvailable(slot)` returns false after `SetKeyY`, the KeyX is missing — log an error
and return `ErrorEncrypted` rather than crashing.

---

### Changes to `src/core/file_sys/ncch_container.h`

Add private members to `NCCHContainer`:

```cpp
#include <array>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include "core/hw/aes/key.h"

// Inside class NCCHContainer private section:
bool is_encrypted = false;

using AESKey = std::array<u8, 16>;
using AESCTR = std::array<u8, 16>;

AESKey primary_key{};
AESKey secondary_key{};
AESCTR exheader_ctr{};
AESCTR exefs_ctr{};
AESCTR romfs_ctr{};

// Called once after LoadHeader() confirms encryption; returns false if keys are unavailable.
bool SetupDecryption();

// Decrypt an in-memory buffer that was read from a given section.
// `offset_in_section` is bytes from the start of the section's CTR stream.
enum class NCCHSection { ExHeader, ExeFS, RomFSPrimary, RomFSSecondary };
void DecryptBuffer(std::vector<u8>& buf, NCCHSection section, std::size_t offset_in_section = 0);
```

---

### `SetupDecryption()` implementation (add to `ncch_container.cpp`)

```cpp
bool NCCHContainer::SetupDecryption() {
    using namespace HW::AES;
    InitKeys();

    constexpr int kBlockSize = 0x200;

    if (ncch_header.fixed_key) {
        primary_key.fill(0);
        secondary_key.fill(0);
    } else {
        AESKey key_y_primary, key_y_secondary;
        std::copy(ncch_header.signature, ncch_header.signature + 16, key_y_primary.begin());

        if (!ncch_header.seed_crypto) {
            key_y_secondary = key_y_primary;
        } else {
            auto opt = FileSys::GetSeed(ncch_header.program_id);
            if (!opt) {
                LOG_ERROR(Service_FS, "Seed for {:016X} not found", ncch_header.program_id);
                return false;
            }
            std::array<u8, 32> input;
            std::memcpy(input.data(), key_y_primary.data(), 16);
            std::memcpy(input.data() + 16, opt->data(), 16);
            CryptoPP::SHA256 sha;
            std::array<u8, CryptoPP::SHA256::DIGESTSIZE> hash;
            sha.CalculateDigest(hash.data(), input.data(), 32);
            std::memcpy(key_y_secondary.data(), hash.data(), 16);
        }

        SetKeyY(KeySlotID::NCCHSecure1, key_y_primary);
        if (!IsNormalKeyAvailable(KeySlotID::NCCHSecure1)) {
            LOG_ERROR(Service_FS, "Encrypted NCCH: slot 0x2C KeyX missing");
            return false;
        }
        primary_key = GetNormalKey(KeySlotID::NCCHSecure1);

        KeySlotID secondary_slot = KeySlotID::NCCHSecure1;
        switch (ncch_header.secondary_key_slot) {
        case 0:  secondary_slot = KeySlotID::NCCHSecure1; break;
        case 1:  secondary_slot = KeySlotID::NCCHSecure2; break;
        case 10: secondary_slot = KeySlotID::NCCHSecure3; break;
        case 11: secondary_slot = KeySlotID::NCCHSecure4; break;
        default:
            LOG_ERROR(Service_FS, "Unknown secondary key slot {}", ncch_header.secondary_key_slot);
            return false;
        }
        SetKeyY(secondary_slot, key_y_secondary);
        if (!IsNormalKeyAvailable(secondary_slot)) {
            LOG_ERROR(Service_FS, "Encrypted NCCH: secondary KeyX missing for slot {:02X}",
                      static_cast<int>(secondary_slot));
            return false;
        }
        secondary_key = GetNormalKey(secondary_slot);
    }

    // CTR derivation
    if (ncch_header.version == 0 || ncch_header.version == 2) {
        std::reverse_copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                          exheader_ctr.begin());
        exheader_ctr.fill(0); // zero bytes 8..15 then set section byte
        std::reverse_copy(ncch_header.partition_id, ncch_header.partition_id + 8,
                          exheader_ctr.begin());
        std::fill(exheader_ctr.begin() + 8, exheader_ctr.end(), 0);
        exefs_ctr = romfs_ctr = exheader_ctr;
        exheader_ctr[8] = 1;
        exefs_ctr[8]    = 2;
        romfs_ctr[8]    = 3;
    } else if (ncch_header.version == 1) {
        std::copy(ncch_header.partition_id, ncch_header.partition_id + 8, exheader_ctr.begin());
        std::fill(exheader_ctr.begin() + 8, exheader_ctr.end(), 0);
        exefs_ctr = romfs_ctr = exheader_ctr;

        auto u32be = [](u32 v) -> std::array<u8, 4> {
            return {u8(v >> 24), u8((v >> 16) & 0xFF), u8((v >> 8) & 0xFF), u8(v & 0xFF)};
        };
        auto off_exh  = u32be(0x200);
        auto off_exfs = u32be(ncch_header.exefs_offset * kBlockSize);
        auto off_rfs  = u32be(ncch_header.romfs_offset * kBlockSize);
        std::copy(off_exh.begin(),  off_exh.end(),  exheader_ctr.begin() + 12);
        std::copy(off_exfs.begin(), off_exfs.end(), exefs_ctr.begin()    + 12);
        std::copy(off_rfs.begin(),  off_rfs.end(),  romfs_ctr.begin()    + 12);
    } else {
        LOG_ERROR(Service_FS, "Unknown NCCH version {}", static_cast<u16>(ncch_header.version));
        return false;
    }

    is_encrypted = true;
    return true;
}
```

---

### `DecryptBuffer()` implementation

```cpp
void NCCHContainer::DecryptBuffer(std::vector<u8>& buf, NCCHSection section,
                                  std::size_t offset_in_section) {
    if (buf.empty()) return;

    const AESKey* key = nullptr;
    const AESCTR* ctr = nullptr;

    switch (section) {
    case NCCHSection::ExHeader:
        key = &primary_key; ctr = &exheader_ctr; break;
    case NCCHSection::ExeFS:
        key = &primary_key; ctr = &exefs_ctr; break;
    case NCCHSection::RomFSPrimary:
        key = &primary_key; ctr = &exefs_ctr; break;
    case NCCHSection::RomFSSecondary:
        key = &secondary_key; ctr = &romfs_ctr; break;
    }

    CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption d(key->data(), key->size(), ctr->data());
    if (offset_in_section != 0)
        d.Seek(offset_in_section);
    d.ProcessData(buf.data(), buf.data(), buf.size());
}
```

---

### Changes to `NCCHContainer::LoadHeader()` (in `ncch_container.cpp`)

Replace the early return:
```cpp
// BEFORE:
if (!ncch_header.no_crypto) {
    return Loader::ResultStatus::ErrorEncrypted;
}
```

With:
```cpp
if (!ncch_header.no_crypto) {
    if (!SetupDecryption()) {
        return Loader::ResultStatus::ErrorEncrypted;
    }
}
```

---

### Changes to `NCCHContainer::Load()` (in `ncch_container.cpp`)

Replace the early return at line ~281:
```cpp
// BEFORE:
if (!ncch_header.no_crypto) {
    return Loader::ResultStatus::ErrorEncrypted;
}
```

With:
```cpp
if (!ncch_header.no_crypto && !is_encrypted) {
    if (!SetupDecryption()) {
        return Loader::ResultStatus::ErrorEncrypted;
    }
}
```

Then, immediately after reading the ExHeader into `exheader_header`, decrypt it:
```cpp
// After: file->ReadBytes(&exheader_header, sizeof(ExHeader_Header))
if (is_encrypted) {
    std::vector<u8> exh_buf(sizeof(ExHeader_Header));
    std::memcpy(exh_buf.data(), &exheader_header, sizeof(ExHeader_Header));
    DecryptBuffer(exh_buf, NCCHSection::ExHeader, 0);
    std::memcpy(&exheader_header, exh_buf.data(), sizeof(ExHeader_Header));
}
```

Then, after reading the ExeFS header into `exefs_header`, decrypt it:
```cpp
// After reading exefs_header from file
if (is_encrypted) {
    std::vector<u8> exfs_buf(sizeof(ExeFs_Header));
    std::memcpy(exfs_buf.data(), &exefs_header, sizeof(ExeFs_Header));
    DecryptBuffer(exfs_buf, NCCHSection::ExeFS, 0);
    std::memcpy(&exefs_header, exfs_buf.data(), sizeof(ExeFs_Header));
}
```

---

### Changes to `NCCHContainer::LoadSectionExeFS()`

After reading a section's raw bytes into `buffer`, decrypt the section data.
Determine which key to use based on the section name:

```cpp
// After: file->ReadBytes(buffer.data(), section_size)
if (is_encrypted) {
    // "icon" and "banner" use primary key; everything else (.code, etc.) uses secondary key
    bool use_primary = (strncmp(section_header.name, "icon",   4) == 0 ||
                        strncmp(section_header.name, "banner", 6) == 0);
    // offset_in_stream = sizeof(ExeFs_Header) + section_header.offset
    std::size_t stream_offset = sizeof(ExeFs_Header) + section_header.offset;
    DecryptBuffer(buffer,
                  use_primary ? NCCHSection::ExeFS : NCCHSection::RomFSSecondary,
                  stream_offset);
}
```

---

### Changes to `NCCHContainer::ReadRomFS()`

RomFS is large so it must **not** be decrypted into memory all at once. Instead, wrap it in a
decrypting reader. The existing `RomFSReader` interface is in `src/core/file_sys/romfs_reader.h`.

Create a new class `DecryptedRomFSReader` that wraps a plain file reader and decrypts on read:

```cpp
// New file: src/core/file_sys/decrypted_romfs_reader.h / .cpp
class DecryptedRomFSReader : public RomFSReader {
public:
    DecryptedRomFSReader(std::shared_ptr<RomFSReader> base,
                         const std::array<u8,16>& key,
                         const std::array<u8,16>& ctr,
                         std::size_t stream_offset)
        : base(std::move(base)), key(key), ctr(ctr), stream_offset(stream_offset) {}

    std::size_t GetSize() const override { return base->GetSize(); }
    bool IsGood() const override { return base->IsGood(); }

    std::size_t ReadFile(std::size_t offset, std::size_t length, u8* buffer) override {
        std::size_t read = base->ReadFile(offset, length, buffer);
        if (read > 0) {
            CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption d(key.data(), key.size(), ctr.data());
            d.Seek(stream_offset + offset);
            d.ProcessData(buffer, buffer, read);
        }
        return read;
    }

private:
    std::shared_ptr<RomFSReader> base;
    std::array<u8,16> key;
    std::array<u8,16> ctr;
    std::size_t stream_offset;
};
```

In `ReadRomFS()`, after creating the base `romfs_file`, wrap it:
```cpp
if (is_encrypted) {
    // stream_offset = romfs section start relative to start of CTR stream
    // For version 0/2: the CTR stream starts at offset 0 of the NCCH, but the
    // CTR is independent per section (romfs_ctr already encodes this), so stream_offset = 0.
    // For version 1: stream_offset = ncch_header.romfs_offset * 0x200
    std::size_t romfs_stream_offset = 0;
    if (ncch_header.version == 1) {
        romfs_stream_offset = ncch_header.romfs_offset * 0x200;
    }
    romfs_file = std::make_shared<DecryptedRomFSReader>(
        std::move(romfs_file), secondary_key, romfs_ctr, romfs_stream_offset);
}
```

---

### Required includes to add to `ncch_container.cpp`

```cpp
#include <array>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <cryptopp/sha.h>
#include "core/file_sys/seed_db.h"
#include "core/hw/aes/key.h"
```

CryptoPP is already linked (`externals/cryptopp`). `seed_db.h` is already in the same directory.

---

### Summary of files to modify for encrypted .3ds support

| File | Change |
|------|--------|
| `src/core/file_sys/ncch_container.h` | Add 5 private members + 2 private methods |
| `src/core/file_sys/ncch_container.cpp` | Add `SetupDecryption()` + `DecryptBuffer()`, patch `LoadHeader()`, `Load()`, `LoadSectionExeFS()`, `ReadRomFS()` |
| `src/core/file_sys/decrypted_romfs_reader.h` (new) | `DecryptedRomFSReader` wrapper class |

No other files need to change.

---

### Why the previous attempt crashed

The prior implementation attempted to decrypt sections inside the renderer/screenshot path or
touched bootmanager — that introduced race conditions and use-after-free with QImage shallow copies
on Apple Silicon (PAC pointer authentication failures). The approach above decrypts **only at file
read time**, on the same thread that calls `Load()`/`LoadSectionExeFS()`, with no shared state,
no background threads, and no callbacks. It is safe.

---

## What NOT to add (lessons learned)

- **Do not stream the 3DS bottom screen to the web page.** Sending screenshots from the render thread to the HTTP server introduced race conditions and use-after-free crashes on Apple Silicon (PAC pointer authentication failures). The touch screen widget is just a black click area — the game reacts to touch coordinates only; the player doesn't need to see the bottom screen mirrored.
- **Do not touch `ncch_container.cpp/h`** — AES-CTR decryption for encrypted ROMs was added and reverted; it caused heap corruption that crashed the emulator. Keep those files at upstream state.
- **Do not modify `bootmanager.cpp/h`** — no screenshot request mechanism needed. Keep at upstream state.
