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

// Window thread, between frames: one shader to compile now. `drawing` = the game is presenting
// frames, so only a prepared area's shaders (past its grace period) are given out.
bool TakeOne(std::pair<uint64_t, uint64_t>& out, bool drawing);
// Any thread: shaders are still queued (prepared or pending), some possibly already built.
bool Busy();

// Render thread: a combiner was bound for drawing.
void NoteUse(uint64_t id0, uint64_t id1);
// Render thread: everything queued since the last call, and what it was queued for.
bool TakePending(std::vector<std::pair<uint64_t, uint64_t>>& out, std::string& label);

} // namespace Fast::ShaderPrewarm
