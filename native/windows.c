// windows.c: clotran-ui's few Win32-only pieces.
#include <windows.h>
#include <stdio.h>
#include "ui.h"

// A window's own pixels -- frame, widgets, drawing -- as Windows renders
// it (PrintWindow, which draws even a window that is covered or on
// another desktop), written to path as a 24-bit BMP.
int clotran_ui_capture(uiWindow *w, const char *path)
{
	HWND h = (HWND) uiControlHandle(uiControl(w));
	RECT r;
	if (!GetWindowRect(h, &r)) return 0;
	int W = r.right - r.left, H = r.bottom - r.top;
	HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = H;   // bottom-up, as a BMP file holds it
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits;
	HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	HGDIOBJ old = SelectObject(dc, bm);
	int ok = PrintWindow(h, dc, 2 /* PW_RENDERFULLCONTENT */) != 0;
	GdiFlush();
	if (ok) {
		int stride = ((W * 3 + 3) / 4) * 4;
		BITMAPFILEHEADER fh;
		ZeroMemory(&fh, sizeof fh);
		fh.bfType = 0x4d42;
		fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
		fh.bfSize = fh.bfOffBits + stride * H;
		FILE *f = fopen(path, "wb");
		ok = f != NULL;
		if (f) {
			fwrite(&fh, sizeof fh, 1, f);
			fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
			fwrite(bits, stride * H, 1, f);
			fclose(f);
		}
	}
	SelectObject(dc, old);
	DeleteObject(bm);
	DeleteDC(dc);
	ReleaseDC(NULL, screen);
	return ok;
}

// A new window comes to the front, above the console or editor that started
// clotran: Windows lets only the foreground thread's input set the
// foreground, so this thread shares that thread's input while it does.
void clotran_ui_front(uiWindow *w)
{
	HWND h = (HWND) uiControlHandle(uiControl(w));
	HWND fg = GetForegroundWindow();
	DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0, me = GetCurrentThreadId();
	int att = fgt && fgt != me && AttachThreadInput(fgt, me, TRUE);
	ShowWindow(h, SW_SHOW);
	BringWindowToTop(h);
	SetForegroundWindow(h);
	if (att) AttachThreadInput(fgt, me, FALSE);
}
