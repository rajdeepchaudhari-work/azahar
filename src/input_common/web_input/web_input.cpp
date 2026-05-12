// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "input_common/keyboard.h"
#include "input_common/web_input/web_input.h"

namespace InputCommon::WebInput {

// ---------------------------------------------------------------------------
// Controller HTML (served at GET /)
// ---------------------------------------------------------------------------
static const char CONTROLLER_HTML[] = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>3DS Controller</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;touch-action:none;-webkit-user-select:none;user-select:none;-webkit-tap-highlight-color:transparent}
body{background:#0f0f0f;width:100vw;height:100dvh;overflow:hidden;position:relative;font-family:-apple-system,system-ui,sans-serif}

/* ── widget base ── */
.w{position:absolute;transform-origin:top left}
.w.ed{outline:1px dashed rgba(255,255,255,0.35)}
.w.ed .rh{display:flex}
.rh{display:none;position:absolute;bottom:-16px;right:-16px;width:32px;height:32px;
    background:#fff;border-radius:50%;align-items:center;justify-content:center;
    font-size:15px;cursor:nwse-resize;z-index:10;color:#333;box-shadow:0 2px 6px #000a}

/* ── shoulder buttons ── */
#bl{width:96px;height:46px;background:linear-gradient(180deg,#909090 0%,#686868 100%);
    border-radius:10px 10px 4px 20px;display:flex;align-items:center;justify-content:center;
    color:#2a2a2a;font-weight:700;font-size:20px;letter-spacing:1px;
    box-shadow:0 5px 10px #000a,inset 0 1px 0 rgba(255,255,255,.2);
    border:2px solid #555;cursor:pointer}
#br{width:96px;height:46px;background:linear-gradient(180deg,#909090 0%,#686868 100%);
    border-radius:10px 10px 20px 4px;display:flex;align-items:center;justify-content:center;
    color:#2a2a2a;font-weight:700;font-size:20px;letter-spacing:1px;
    box-shadow:0 5px 10px #000a,inset 0 1px 0 rgba(255,255,255,.2);
    border:2px solid #555;cursor:pointer}
#bl:active,#br:active{background:linear-gradient(180deg,#a0a0a0 0%,#787878 100%)}

/* ── D-pad ── */
#dp{position:relative;width:126px;height:126px}
.da{position:absolute;width:42px;height:42px;
    background:linear-gradient(145deg,#787878 0%,#525252 100%);
    border:2px solid #404040;display:flex;align-items:center;justify-content:center;
    color:#2a2a2a;font-size:15px;cursor:pointer;
    box-shadow:0 3px 6px #000a,inset 0 1px 0 rgba(255,255,255,.15)}
.da:active{background:linear-gradient(145deg,#888 0%,#626262 100%)}
#du{top:0;left:42px;border-radius:4px 4px 0 0}
#dl{top:42px;left:0;border-radius:4px 0 0 4px}
#dc{top:42px;left:42px;width:42px;height:42px;position:absolute;
    background:#484848;pointer-events:none}
#dr{top:42px;right:0;border-radius:0 4px 4px 0}
#dd{bottom:0;left:42px;border-radius:0 0 4px 4px}

/* ── circle pad ── */
#cpw{width:108px;height:108px}
#cp{width:108px;height:108px;border-radius:50%;
    background:radial-gradient(circle at 38% 32%,#888,#3a3a3a);
    box-shadow:0 5px 14px #000,inset 0 2px 0 rgba(255,255,255,.08);
    border:3px solid #303030;position:relative;display:flex;
    align-items:center;justify-content:center;cursor:grab}
#cpt{width:58px;height:58px;border-radius:50%;
    background:radial-gradient(circle at 38% 32%,#9a9a9a,#606060);
    box-shadow:0 3px 8px #000a,inset 0 1px 0 rgba(255,255,255,.2);
    border:2px solid #505050;position:absolute;pointer-events:none;transition:none}

/* ── face buttons ── */
#fbw{position:relative;width:138px;height:138px}
.fb{position:absolute;width:54px;height:54px;border-radius:50%;
    background:linear-gradient(145deg,#888 0%,#626262 100%);
    display:flex;align-items:center;justify-content:center;
    font-weight:700;font-size:20px;color:#2a2a2a;cursor:pointer;
    box-shadow:0 4px 10px #000a,inset 0 1px 0 rgba(255,255,255,.18);
    border:2px solid #505050}
.fb:active{background:linear-gradient(145deg,#999 0%,#727272 100%)}
#bx{top:0;left:42px}
#by{top:42px;left:0}
#ba{top:42px;right:0}
#bb{bottom:0;left:42px}

/* ── select / start ── */
#ssw{display:flex;gap:14px;align-items:center}
.ss{height:28px;padding:0 14px;border-radius:14px;
    background:linear-gradient(180deg,#777 0%,#555 100%);color:#2a2a2a;
    font-size:11px;font-weight:700;letter-spacing:.5px;
    display:flex;align-items:center;justify-content:center;cursor:pointer;
    box-shadow:0 3px 7px #000a,inset 0 1px 0 rgba(255,255,255,.15);
    border:1px solid #444}
.ss:active{background:linear-gradient(180deg,#888 0%,#666 100%)}

/* ── touch screen ── */
#tpw{display:flex;flex-direction:column;align-items:center;gap:5px}
#tlbl{color:#3a3a3a;font-size:9px;letter-spacing:3px;text-transform:uppercase;font-weight:600}
#tp{width:256px;height:192px;background:#0a0a0a;cursor:crosshair;
    border:2px solid #2a2a2a;border-radius:4px;position:relative;overflow:hidden}
#td{width:18px;height:18px;border-radius:50%;background:rgba(255,255,255,.45);
    position:absolute;display:none;transform:translate(-50%,-50%);
    pointer-events:none;box-shadow:0 0 8px rgba(255,255,255,.3)}

/* ── edit overlay ── */
#edit-btn{position:fixed;bottom:14px;right:14px;z-index:300;
    background:rgba(255,255,255,.12);color:rgba(255,255,255,.8);
    border:1px solid rgba(255,255,255,.25);border-radius:22px;padding:7px 20px;
    font-size:13px;font-weight:600;cursor:pointer;backdrop-filter:blur(6px)}
#edit-btn:active{background:rgba(255,255,255,.2)}
#reset-btn{position:fixed;bottom:14px;left:14px;z-index:300;
    background:rgba(220,60,60,.25);color:rgba(255,120,120,.9);
    border:1px solid rgba(220,60,60,.35);border-radius:22px;padding:7px 20px;
    font-size:13px;font-weight:600;cursor:pointer;display:none;backdrop-filter:blur(6px)}
</style>
</head>
<body>

<!-- L shoulder -->
<div class="w" id="wl">
  <div id="bl">L</div>
  <div class="rh">&#10021;</div>
</div>

<!-- R shoulder -->
<div class="w" id="wr">
  <div id="br">R</div>
  <div class="rh">&#10021;</div>
</div>

<!-- D-pad -->
<div class="w" id="wd">
  <div id="dp">
    <div class="da" id="du">&#9650;</div>
    <div class="da" id="dl">&#9664;</div>
    <div id="dc"></div>
    <div class="da" id="dr">&#9654;</div>
    <div class="da" id="dd">&#9660;</div>
  </div>
  <div class="rh">&#10021;</div>
</div>

<!-- Circle pad -->
<div class="w" id="wc">
  <div id="cpw">
    <div id="cp"><div id="cpt"></div></div>
  </div>
  <div class="rh">&#10021;</div>
</div>

<!-- Touch screen -->
<div class="w" id="wt">
  <div id="tpw">
    <div id="tlbl">TOUCH SCREEN</div>
    <div id="tp"><div id="td"></div></div>
  </div>
  <div class="rh">&#10021;</div>
</div>

<!-- Face buttons -->
<div class="w" id="wf">
  <div id="fbw">
    <div class="fb" id="bx">X</div>
    <div class="fb" id="by">Y</div>
    <div class="fb" id="ba">A</div>
    <div class="fb" id="bb">B</div>
  </div>
  <div class="rh">&#10021;</div>
</div>

<!-- Select + Start -->
<div class="w" id="wb">
  <div id="ssw">
    <div class="ss" id="bsel">SELECT</div>
    <div class="ss" id="bsta">START</div>
  </div>
  <div class="rh">&#10021;</div>
</div>

<button id="edit-btn">EDIT</button>
<button id="reset-btn">RESET</button>

<script>
const DEFS = {
  wl:{x:1,  y:2,  s:1},
  wr:{x:82, y:2,  s:1},
  wd:{x:1,  y:24, s:1},
  wc:{x:2,  y:57, s:1},
  wt:{x:24, y:8,  s:1.15},
  wf:{x:78, y:24, s:1},
  wb:{x:38, y:88, s:1},
};

let layout = (() => {
  try { return JSON.parse(localStorage.getItem('ctr3ds')) || null; } catch(e) { return null; }
})() || JSON.parse(JSON.stringify(DEFS));

let editMode = false;

function send(data) {
  fetch('/input', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(data),
    keepalive:true
  }).catch(()=>{});
}

function applyLayout() {
  for (const id in layout) {
    const el = document.getElementById(id);
    const p  = layout[id];
    if (el) {
      el.style.left      = p.x + '%';
      el.style.top       = p.y + '%';
      el.style.transform = 'scale(' + p.s + ')';
    }
  }
}

function saveLayout() {
  localStorage.setItem('ctr3ds', JSON.stringify(layout));
}

/* ── drag + resize ── */
function makeDraggable(wid) {
  const el = document.getElementById(wid);
  const rh = el.querySelector('.rh');

  let dragActive = false, sx, sy, slx, sly;
  el.addEventListener('pointerdown', e => {
    if (!editMode || rh.contains(e.target)) return;
    e.stopPropagation();
    dragActive = true;
    el.setPointerCapture(e.pointerId);
    sx = e.clientX; sy = e.clientY;
    slx = layout[wid].x; sly = layout[wid].y;
  });
  el.addEventListener('pointermove', e => {
    if (!dragActive) return;
    layout[wid].x = slx + (e.clientX - sx) / innerWidth  * 100;
    layout[wid].y = sly + (e.clientY - sy) / innerHeight * 100;
    el.style.left = layout[wid].x + '%';
    el.style.top  = layout[wid].y + '%';
  });
  el.addEventListener('pointerup', () => { if (dragActive) { dragActive = false; saveLayout(); } });

  let resizeActive = false, rd0, rs0;
  rh.addEventListener('pointerdown', e => {
    e.stopPropagation();
    resizeActive = true;
    rh.setPointerCapture(e.pointerId);
    const r = el.getBoundingClientRect();
    rd0 = Math.hypot(e.clientX - r.left, e.clientY - r.top);
    rs0 = layout[wid].s;
  });
  rh.addEventListener('pointermove', e => {
    if (!resizeActive) return;
    const r = el.getBoundingClientRect();
    const d = Math.hypot(e.clientX - r.left, e.clientY - r.top);
    layout[wid].s = Math.max(0.35, Math.min(3, rs0 * d / rd0));
    el.style.transform = 'scale(' + layout[wid].s + ')';
  });
  rh.addEventListener('pointerup', () => { if (resizeActive) { resizeActive = false; saveLayout(); } });
}

['wl','wr','wd','wc','wt','wf','wb'].forEach(makeDraggable);
applyLayout();

/* ── edit mode ── */
const editBtn  = document.getElementById('edit-btn');
const resetBtn = document.getElementById('reset-btn');
editBtn.addEventListener('click', () => {
  editMode = !editMode;
  editBtn.textContent = editMode ? 'DONE' : 'EDIT';
  resetBtn.style.display = editMode ? 'block' : 'none';
  document.querySelectorAll('.w').forEach(w => w.classList.toggle('ed', editMode));
});
resetBtn.addEventListener('click', () => {
  layout = JSON.parse(JSON.stringify(DEFS));
  applyLayout();
  saveLayout();
});

/* ── button inputs ── */
function bindBtn(id, name) {
  const el = document.getElementById(id);
  el.addEventListener('pointerdown', e => {
    if (editMode) return;
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    send({type:'button', name, pressed:true});
  });
  ['pointerup','pointercancel'].forEach(ev =>
    el.addEventListener(ev, () => { if (!editMode) send({type:'button', name, pressed:false}); })
  );
}

bindBtn('bl',   'L');      bindBtn('br',   'R');
bindBtn('du',   'Up');     bindBtn('dd',   'Down');
bindBtn('dl',   'Left');   bindBtn('dr',   'Right');
bindBtn('bx',   'X');      bindBtn('by',   'Y');
bindBtn('ba',   'A');      bindBtn('bb',   'B');
bindBtn('bsel', 'Select'); bindBtn('bsta', 'Start');

/* ── circle pad ── */
const cp  = document.getElementById('cp');
const cpt = document.getElementById('cpt');
const CPR = 50; // radius for clamping (px)
let cpActive = false;

cp.addEventListener('pointerdown', e => {
  if (editMode) return;
  cpActive = true;
  cp.setPointerCapture(e.pointerId);
  cp.style.cursor = 'grabbing';
});
cp.addEventListener('pointermove', e => {
  if (!cpActive) return;
  const r = cp.getBoundingClientRect();
  const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
  let dx = e.clientX - cx, dy = e.clientY - cy;
  const dist = Math.hypot(dx, dy);
  if (dist > CPR) { dx = dx / dist * CPR; dy = dy / dist * CPR; }
  cpt.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
  send({type:'analog', name:'circle', x: dx / CPR, y: -dy / CPR});
});
function cpRelease() {
  if (!cpActive) return;
  cpActive = false;
  cp.style.cursor = 'grab';
  cpt.style.transform = '';
  send({type:'analog', name:'circle', x:0, y:0});
}
cp.addEventListener('pointerup',     cpRelease);
cp.addEventListener('pointercancel', cpRelease);

/* ── touch screen ── */
const tp = document.getElementById('tp');
const td = document.getElementById('td');
let tpActive = false;

tp.addEventListener('pointerdown', e => {
  if (editMode) return;
  tpActive = true;
  tp.setPointerCapture(e.pointerId);
  const r = tp.getBoundingClientRect();
  const x = (e.clientX - r.left) / r.width;
  const y = (e.clientY - r.top)  / r.height;
  td.style.left = (x * 100) + '%';
  td.style.top  = (y * 100) + '%';
  td.style.display = 'block';
  send({type:'touch', x, y, pressed:true});
});
tp.addEventListener('pointermove', e => {
  if (!tpActive) return;
  const r = tp.getBoundingClientRect();
  const x = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  const y = Math.max(0, Math.min(1, (e.clientY - r.top)  / r.height));
  td.style.left = (x * 100) + '%';
  td.style.top  = (y * 100) + '%';
  send({type:'touch_move', x, y});
});
function tpRelease() {
  if (!tpActive) return;
  tpActive = false;
  td.style.display = 'none';
  send({type:'touch', x:0, y:0, pressed:false});
}
tp.addEventListener('pointerup',     tpRelease);
tp.addEventListener('pointercancel', tpRelease);
</script>
</body>
</html>
)HTML";

// ---------------------------------------------------------------------------
// Server implementation
// ---------------------------------------------------------------------------
struct Server::Impl {
    Keyboard*              keyboard    = nullptr;
    Frontend::EmuWindow*   emu_window  = nullptr;
    httplib::Server        svr;
    std::thread            server_thread;
    std::atomic<bool>      running{false};
    u16                    bound_port{0};

    std::unordered_map<int, int> button_key_map; // NativeButton index → key code
    int circle_up_key    = -1;
    int circle_down_key  = -1;
    int circle_left_key  = -1;
    int circle_right_key = -1;
    bool circle_up_pressed    = false;
    bool circle_down_pressed  = false;
    bool circle_left_pressed  = false;
    bool circle_right_pressed = false;

    void LoadKeyMappings() {
        button_key_map.clear();
        const auto& profile = Settings::values.current_input_profile;

        for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
            Common::ParamPackage pkg{profile.buttons[i]};
            if (pkg.Get("engine", "") == "keyboard") {
                button_key_map[i] = pkg.Get("code", -1);
            }
        }

        // Circle pad via analog_from_button
        Common::ParamPackage ap{profile.analogs[Settings::NativeAnalog::CirclePad]};
        if (ap.Get("engine", "") == "analog_from_button") {
            auto parseKey = [](const std::string& s) {
                Common::ParamPackage p{s};
                return (p.Get("engine", "") == "keyboard") ? p.Get("code", -1) : -1;
            };
            circle_up_key    = parseKey(ap.Get("up",    ""));
            circle_down_key  = parseKey(ap.Get("down",  ""));
            circle_left_key  = parseKey(ap.Get("left",  ""));
            circle_right_key = parseKey(ap.Get("right", ""));
        }
    }

    void HandleButton(const std::string& name, bool pressed) {
        static const std::unordered_map<std::string, int> name_to_native = {
            {"A",      Settings::NativeButton::A},
            {"B",      Settings::NativeButton::B},
            {"X",      Settings::NativeButton::X},
            {"Y",      Settings::NativeButton::Y},
            {"Up",     Settings::NativeButton::Up},
            {"Down",   Settings::NativeButton::Down},
            {"Left",   Settings::NativeButton::Left},
            {"Right",  Settings::NativeButton::Right},
            {"L",      Settings::NativeButton::L},
            {"R",      Settings::NativeButton::R},
            {"Start",  Settings::NativeButton::Start},
            {"Select", Settings::NativeButton::Select},
            {"ZL",     Settings::NativeButton::ZL},
            {"ZR",     Settings::NativeButton::ZR},
            {"Home",   Settings::NativeButton::Home},
        };

        auto it = name_to_native.find(name);
        if (it == name_to_native.end())
            return;

        auto kit = button_key_map.find(it->second);
        if (kit == button_key_map.end() || kit->second == -1)
            return;

        if (pressed)
            keyboard->PressKey(kit->second);
        else
            keyboard->ReleaseKey(kit->second);
    }

    void HandleTouch(float x, float y, bool pressed) {
        if (pressed) {
            auto fx = static_cast<unsigned>(x * 320.0f);
            auto fy = static_cast<unsigned>(y * 240.0f);
            emu_window->TouchPressed(fx, fy);
        } else {
            emu_window->TouchReleased();
        }
    }

    void HandleTouchMove(float x, float y) {
        auto fx = static_cast<unsigned>(x * 320.0f);
        auto fy = static_cast<unsigned>(y * 240.0f);
        emu_window->TouchMoved(fx, fy);
    }

    void HandleAnalog(float ax, float ay) {
        constexpr float DEAD = 0.3f;

        auto press_release = [this](int key, bool& was_pressed, bool now_pressed) {
            if (key == -1)
                return;
            if (now_pressed && !was_pressed)
                keyboard->PressKey(key);
            else if (!now_pressed && was_pressed)
                keyboard->ReleaseKey(key);
            was_pressed = now_pressed;
        };

        press_release(circle_up_key,    circle_up_pressed,     ay >  DEAD);
        press_release(circle_down_key,  circle_down_pressed,   ay < -DEAD);
        press_release(circle_left_key,  circle_left_pressed,   ax < -DEAD);
        press_release(circle_right_key, circle_right_pressed,  ax >  DEAD);
    }

    void ReleaseAll() {
        for (auto& [btn, key] : button_key_map) {
            if (key != -1)
                keyboard->ReleaseKey(key);
        }
        auto releaseDir = [this](int key, bool& pressed) {
            if (key != -1 && pressed)
                keyboard->ReleaseKey(key);
            pressed = false;
        };
        releaseDir(circle_up_key,    circle_up_pressed);
        releaseDir(circle_down_key,  circle_down_pressed);
        releaseDir(circle_left_key,  circle_left_pressed);
        releaseDir(circle_right_key, circle_right_pressed);
        if (emu_window)
            emu_window->TouchReleased();
    }

    void SetupRoutes() {
        svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;
        });

        svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(CONTROLLER_HTML, "text/html");
        });

        svr.Post("/input", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto j = nlohmann::json::parse(req.body);
                const std::string type = j.at("type").get<std::string>();

                if (type == "button") {
                    HandleButton(j.at("name").get<std::string>(),
                                 j.at("pressed").get<bool>());
                } else if (type == "touch") {
                    HandleTouch(j.at("x").get<float>(),
                                j.at("y").get<float>(),
                                j.at("pressed").get<bool>());
                } else if (type == "touch_move") {
                    HandleTouchMove(j.at("x").get<float>(),
                                    j.at("y").get<float>());
                } else if (type == "analog") {
                    HandleAnalog(j.at("x").get<float>(),
                                 j.at("y").get<float>());
                }
            } catch (...) {}
            res.status = 204;
        });
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
Server::Server()  : impl(std::make_unique<Impl>()) {}
Server::~Server() { Stop(); }

bool Server::Start(Keyboard* keyboard, Frontend::EmuWindow* window, u16 port) {
    if (impl->running)
        Stop();

    impl->keyboard   = keyboard;
    impl->emu_window = window;

    impl->LoadKeyMappings();
    impl->SetupRoutes();

    if (!impl->svr.bind_to_port("0.0.0.0", port)) {
        LOG_ERROR(Frontend, "WebInput: failed to bind port {}", port);
        return false;
    }
    impl->bound_port = static_cast<u16>(impl->svr.bind_to_any_port("0.0.0.0", 0) == -1
                                            ? port
                                            : port);
    // Re-read the actually bound port via a helper call after bind_to_port
    // (bind_to_port doesn't return the port; we just store what was requested)
    impl->bound_port = port;

    impl->running = true;
    impl->server_thread = std::thread([this] { impl->svr.listen_after_bind(); });
    return true;
}

void Server::Stop() {
    if (!impl->running)
        return;
    impl->ReleaseAll();
    impl->svr.stop();
    if (impl->server_thread.joinable())
        impl->server_thread.join();
    impl->running = false;
}

bool Server::IsRunning() const { return impl->running.load(); }
u16  Server::GetPort()    const { return impl->bound_port; }

} // namespace InputCommon::WebInput
