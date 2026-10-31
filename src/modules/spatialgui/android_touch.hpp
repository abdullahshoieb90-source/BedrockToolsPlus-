#pragma once

namespace bedrocktools::spatialgui {
// Experimental, single-pointer Android touch injection for the launcher.
// Call only on the Android UI thread, and pass coordinates in the game view's
// physical pixels (NOT normalized panel UVs or Minecraft logical UI units).
// Successful dispatch means the Java call completed; it does not prove that the
// game accepted or acted on the input. No module currently invokes this class.
class AndroidTouch {
public:
    AndroidTouch() = default;
    AndroidTouch(const AndroidTouch&) = delete;
    AndroidTouch& operator=(const AndroidTouch&) = delete;
    bool down(float x, float y);
    bool move(float x, float y);
    bool up(float x, float y);
    bool cancel();
private:
    bool send(int action, float x, float y);
    long long m_downTime = 0;
    float m_x = 0, m_y = 0;
    bool m_active = false;
};
}
