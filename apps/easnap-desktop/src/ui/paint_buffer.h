#pragma once
#include <windows.h>

namespace easnap::ui {
// A complete paint is presented in one blit, including material and separators.
class PaintBuffer final {
public:
    PaintBuffer(HDC target, const RECT& client);
    ~PaintBuffer();
    HDC dc() const { return buffer_ ? buffer_ : target_; }
    void Present();
    bool PresentLayered(HWND window);
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;
private:
    HDC target_{}, buffer_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
    RECT client_{};
    bool presented_{};
};

inline PaintBuffer::PaintBuffer(HDC target,const RECT& client) : target_(target),client_(client) {
    if(!target || client.right<=0 || client.bottom<=0) { return; }
    buffer_=::CreateCompatibleDC(target);
    bitmap_=::CreateCompatibleBitmap(target,client.right,client.bottom);
    if(buffer_ && bitmap_) { previous_=::SelectObject(buffer_,bitmap_); }
    else { if(buffer_) { ::DeleteDC(buffer_); buffer_=nullptr; } }
}
inline void PaintBuffer::Present() {
    if(buffer_ && !presented_) {
        ::BitBlt(target_,client_.left,client_.top,client_.right-client_.left,client_.bottom-client_.top,buffer_,client_.left,client_.top,SRCCOPY);
    }
    presented_=true;
}
inline bool PaintBuffer::PresentLayered(HWND window) {
    // Supply the complete opaque bitmap to the compositor in one operation.
    // A layered child must not reuse pixels from its composited parent.
    presented_=true;
    if(!buffer_) { ::SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    SIZE size{client_.right-client_.left,client_.bottom-client_.top};
    POINT source{client_.left,client_.top};
    return ::UpdateLayeredWindow(window,nullptr,nullptr,&size,buffer_,&source,
                                0,nullptr,ULW_OPAQUE)!=FALSE;
}
inline PaintBuffer::~PaintBuffer() {
    Present();
    if(previous_) { ::SelectObject(buffer_,previous_); }
    if(bitmap_) { ::DeleteObject(bitmap_); }
    if(buffer_) { ::DeleteDC(buffer_); }
}

} // namespace easnap::ui
