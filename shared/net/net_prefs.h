#pragma once

// Process-wide network preferences shared by agent + viewer.
//
// Right now the only knob is force-TCP-video: when set, both sides skip UDP
// video plane setup on the next session, so the encoded H.264 stream travels
// over the existing TCP relay path. Used as a 5-second diagnostic when a
// customer's UDP path drops fragmented IDR packets and the viewer shows a
// black screen — see the tray menu's "强制 TCP" toggle.
//
// The initial value is picked up from RD_DISABLE_UDP=1 on first read (kept
// for parity with the pre-existing env-var kill-switch). The GUI toggle
// overrides it via SetForceTcpVideo; changes only affect NEW sessions —
// the caller is responsible for telling the user to reconnect.
namespace net_prefs {

bool ForceTcpVideo();
void SetForceTcpVideo(bool on);

} // namespace net_prefs
