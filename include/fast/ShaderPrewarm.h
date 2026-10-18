#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Compiles a game area's shaders up front, while the screen is black for the area change, instead
// of the first time each one is drawn. Shader compiles are 40-100 ms each on slow GPUs/drivers
// (Switch) and can't be cached there, so this is where they're hidden.
//
// Which shaders an area needs is learned by playing: every combiner bound in an area is appended
// to a list file once, as "<area> <id0> <id1>". Shaders seen before the first area (title, file
// select) or in several areas form a global set compiled at boot.
//
// Which area follows which is learned too ("@ next <from> <to>"), so entering an area also queues
// its likely next areas as low-priority background work that never holds anything up.
namespace Fast::ShaderPrewarm {

// Loads the list file and enables recording + prewarming. Off until called.
void Init(const std::string& listPath);
bool Enabled();

// Any thread. Marks `area` current for recording and queues its shaders for the next frame.
void EnterArea(const std::string& area);
// Any thread. Queues the global set for the next frame.
void RequestGlobal();
// Any thread. About to leave for `area` (its fade-out is starting): its shaders are handed out by
// TakeOne in idle time, and only after graceMs while frames are still drawn, once the fade is dark.
// Recording stays on the current area until EnterArea.
void PrepareArea(const std::string& area, int graceMs);
// Any thread. `to` was entered from `from` (an area change, or a battle started there): counted
// and persisted, and entering `from` then queues `to` as background work.
void NoteLink(const std::string& from, const std::string& to);
// Any thread. Areas likely next, most likely first (e.g. the highlighted save's area): the
// background queue becomes them, their links, then every other known area.
void Hint(const std::vector<std::string>& areas);
// Any thread. A persisted key/value (last write wins).
void Remember(const std::string& key, const std::string& value);
std::string Recall(const std::string& key);

// Window thread, between frames: one shader to compile now. `drawing` = the game is presenting
// frames, so only a prepared area's shaders (past its grace period) are given out.
// `allowBackground`: once those are done, predicted areas' shaders too.
bool TakeOne(std::pair<uint64_t, uint64_t>& out, bool drawing, bool allowBackground = false);
// Any thread: prepared or pending shaders are still queued. Background work never counts.
bool Busy();

// Render thread: a combiner was bound for drawing.
void NoteUse(uint64_t id0, uint64_t id1);
// Render thread: everything queued since the last call, and what it was queued for.
bool TakePending(std::vector<std::pair<uint64_t, uint64_t>>& out, std::string& label);

} // namespace Fast::ShaderPrewarm
