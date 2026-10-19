// windows_snap.cpp: a window's display list drawn offscreen on Windows --
// Direct2D in software into a memory bitmap, through libui-ng's own
// uiDrawContext, so the very drawing code a window uses -- and written as
// a 24-bit BMP. It needs no display: it works in a session nobody sees.
#include "windows/uipriv_windows.hpp"
#include "windows/draw.hpp"
#include <stdio.h>

extern "C" int clotran_ui_snapshot_win(int W, int H, const char *path,
                                       void (*draw)(uiDrawContext *, void *), void *arg)
{
	HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof bi);
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = -H;   // top-down while drawing
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	HGDIOBJ old = SelectObject(dc, bm);
	D2D1_RENDER_TARGET_PROPERTIES props;
	ZeroMemory(&props, sizeof props);
	props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
	props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
	props.dpiX = 96; props.dpiY = 96;   // a DIP is a pixel, as the area's are at 100%
	props.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
	ID2D1DCRenderTarget *rt = NULL;
	int ok = 0;
	RECT r = { 0, 0, W, H };
	if (d2dfactory->CreateDCRenderTarget(&props, &rt) == S_OK && rt->BindDC(dc, &r) == S_OK) {
		rt->BeginDraw();
		uiDrawContext *c = newContext(rt);
		draw(c, arg);
		freeContext(c);
		ok = rt->EndDraw() == S_OK;
	}
	if (rt) rt->Release();
	GdiFlush();
	if (ok) {
		FILE *f = fopen(path, "wb");
		ok = f != NULL;
		if (f) {
			int stride = ((W * 3 + 3) / 4) * 4;
			BITMAPFILEHEADER fh;
			BITMAPINFOHEADER ih = bi.bmiHeader;
			ZeroMemory(&fh, sizeof fh);
			ih.biBitCount = 24;
			ih.biHeight = H;   // a BMP file is bottom-up
			fh.bfType = 0x4d42;
			fh.bfOffBits = sizeof fh + sizeof ih;
			fh.bfSize = fh.bfOffBits + stride * H;
			fwrite(&fh, sizeof fh, 1, f);
			fwrite(&ih, sizeof ih, 1, f);
			unsigned char *row = (unsigned char *) calloc(stride, 1);
			for (int y = H - 1; y >= 0; y--) {
				const unsigned char *p = (const unsigned char *) bits + (size_t) y * W * 4;
				for (int x = 0; x < W; x++) { row[3 * x] = p[4 * x]; row[3 * x + 1] = p[4 * x + 1]; row[3 * x + 2] = p[4 * x + 2]; }
				fwrite(row, stride, 1, f);
			}
			free(row);
			fclose(f);
		}
	}
	SelectObject(dc, old);
	DeleteObject(bm);
	DeleteDC(dc);
	ReleaseDC(NULL, screen);
	return ok;
}
