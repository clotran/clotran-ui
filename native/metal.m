// metal.m: the 3D view on macOS -- a CAMetalLayer over a window's drawing
// area, drawing a mesh (triangles lit as clotran-plot's SVG surfaces are,
// and lines) under an orthographic camera turned by azimuth and elevation.
// The same g3_* interface is vulkan.c's on Windows and Linux; the scene
// and the camera are clotran_ui.c's, so both GPUs draw the same picture.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "ui.h"
#include "g3.h"

static NSString *src = @
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct U { float az; float el; float kx; float ky; float lx; float ly; float lz; float pad; };\n"
	"struct O { float4 pos [[position]]; float3 c; float3 p; };\n"
	"vertex O vs(const device packed_float3 *pos [[buffer(0)]], const device packed_float3 *col [[buffer(1)]],\n"
	"            constant U &u [[buffer(2)]], uint vid [[vertex_id]]) {\n"
	"  float3 q = pos[vid];\n"
	"  float X = q.x * cos(u.az) - q.y * sin(u.az);\n"
	"  float Y = q.x * sin(u.az) + q.y * cos(u.az);\n"
	"  float up = Y * sin(u.el) + q.z * cos(u.el);\n"
	"  float to = q.z * sin(u.el) - Y * cos(u.el);\n"
	"  O o; o.pos = float4(X * u.kx, up * u.ky, 0.5 - 0.4 * to, 1.0); o.c = col[vid]; o.p = q; return o;\n"
	"}\n"
	"fragment float4 fs_tri(O in [[stage_in]], constant U &u [[buffer(0)]]) {\n"
	"  float3 n = cross(dfdx(in.p), dfdy(in.p));\n"
	"  float l = length(n);\n"
	"  float c = 1.0;\n"
	"  if (l > 0.0) { n = n / l; if (n.z < 0.0) n = -n; c = max(0.0, dot(n, float3(u.lx, u.ly, u.lz))); }\n"
	"  return float4(in.c * (0.35 + 0.65 * c), 1.0);\n"
	"}\n"
	"fragment float4 fs_line(O in [[stage_in]]) { return float4(in.c, 1.0); }\n";

typedef struct {
	id<MTLDevice> dev;
	id<MTLCommandQueue> q;
	id<MTLRenderPipelineState> tri, line;
	id<MTLDepthStencilState> depth;
	CAMetalLayer *layer;
	NSView *view;
	id<MTLBuffer> tpos, tcol, tidx, lpos, lcol;
	int nidx, nlv;
	float bg[3];
	const char *grab;   // when set, the next frame shown is also written here (g3_capture)
	int grabbed;
} m3;

static id<MTLRenderPipelineState> pipeline(m3 *m, id<MTLLibrary> lib, NSString *frag)
{
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction = [lib newFunctionWithName:@"vs"];
	d.fragmentFunction = [lib newFunctionWithName:frag];
	d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	NSError *e = nil;
	id<MTLRenderPipelineState> p = [m->dev newRenderPipelineStateWithDescriptor:d error:&e];
	[d release];
	return p;
}

void *g3_attach(uiArea *a)
{
	@autoreleasepool {
		m3 *m = calloc(1, sizeof *m);
		m->dev = MTLCreateSystemDefaultDevice();
		if (!m->dev) { free(m); return NULL; }
		m->q = [m->dev newCommandQueue];
		NSError *e = nil;
		id<MTLLibrary> lib = [m->dev newLibraryWithSource:src options:nil error:&e];
		if (!lib) { NSLog(@"clotran-ui metal: %@", e); free(m); return NULL; }
		m->tri = pipeline(m, lib, @"fs_tri");
		m->line = pipeline(m, lib, @"fs_line");
		MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
		dd.depthCompareFunction = MTLCompareFunctionLess;
		dd.depthWriteEnabled = YES;
		m->depth = [m->dev newDepthStencilStateWithDescriptor:dd];
		[dd release];
		m->view = (NSView *) uiControlHandle(uiControl(a));
		[m->view setWantsLayer:YES];
		m->layer = [CAMetalLayer new];
		m->layer.device = m->dev;
		m->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		m->layer.frame = m->view.bounds;
		m->layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
		m->layer.framebufferOnly = NO;   // so a shown frame can be read back (g3_capture)
		m->layer.contentsScale = m->view.window ? m->view.window.backingScaleFactor : 2.0;
		[m->view.layer addSublayer:m->layer];
		return m;
	}
}

static id<MTLBuffer> buf(m3 *m, const void *p, size_t n)
{
	return n ? [m->dev newBufferWithBytes:p length:n options:MTLResourceStorageModeShared] : nil;
}

void g3_mesh(void *h, const g3scene *s)
{
	m3 *m = h;
	if (!m) return;
	[m->tpos release]; [m->tcol release]; [m->tidx release]; [m->lpos release]; [m->lcol release];
	m->tpos = buf(m, s->pos, sizeof(float) * 3 * s->nv);
	m->tcol = buf(m, s->col, sizeof(float) * 3 * s->nv);
	m->tidx = buf(m, s->idx, sizeof(uint32_t) * 3 * s->nt);
	m->nidx = 3 * s->nt;
	m->lpos = buf(m, s->lpos, sizeof(float) * 3 * s->nlv);
	m->lcol = buf(m, s->lcol, sizeof(float) * 3 * s->nlv);
	m->nlv = s->nlv;
	memcpy(m->bg, s->bg, sizeof m->bg);
}

static id<MTLCommandBuffer> encode(m3 *m, id<MTLTexture> target, id<MTLTexture> depth, const g3camera *c, int W, int H)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = target;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(m->bg[0], m->bg[1], m->bg[2], 1);
	rp.depthAttachment.texture = depth;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.clearDepth = 1.0;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	float u[8];
	g3_uniforms(c, W, H, u);
	id<MTLCommandBuffer> cb = [m->q commandBuffer];
	id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
	[en setDepthStencilState:m->depth];
	if (m->nidx) {
		[en setRenderPipelineState:m->tri];
		[en setVertexBuffer:m->tpos offset:0 atIndex:0];
		[en setVertexBuffer:m->tcol offset:0 atIndex:1];
		[en setVertexBytes:u length:sizeof u atIndex:2];
		[en setFragmentBytes:u length:sizeof u atIndex:0];
		[en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:m->nidx indexType:MTLIndexTypeUInt32
		              indexBuffer:m->tidx indexBufferOffset:0];
	}
	if (m->nlv) {
		[en setRenderPipelineState:m->line];
		[en setVertexBuffer:m->lpos offset:0 atIndex:0];
		[en setVertexBuffer:m->lcol offset:0 atIndex:1];
		[en setVertexBytes:u length:sizeof u atIndex:2];
		[en drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:m->nlv];
	}
	[en endEncoding];
	return cb;
}

static id<MTLTexture> depth_for(m3 *m, int W, int H)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
	                                                                             width:W height:H mipmapped:NO];
	d.usage = MTLTextureUsageRenderTarget;
	d.storageMode = MTLStorageModePrivate;
	return [m->dev newTextureWithDescriptor:d];
}

// A BGRA texture's pixels written to path as PNG.
static int write_png(id<MTLTexture> t, int W, int H, const char *path)
{
	size_t stride = 4 * W;
	unsigned char *px = malloc(stride * H);
	[t getBytes:px bytesPerRow:stride fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
	NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H
	                                                              bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
	                                                             colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:stride bitsPerPixel:32];
	unsigned char *dst = [rep bitmapData];
	for (size_t i = 0; i < stride * H; i += 4) {   // BGRA to RGBA
		dst[i] = px[i + 2]; dst[i + 1] = px[i + 1]; dst[i + 2] = px[i]; dst[i + 3] = 255;
	}
	free(px);
	NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
	int ok = [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES] ? 1 : 0;
	[rep release];
	return ok;
}

// The view drawn now, on the main thread, at the area's size, into the
// layer the window shows.
void g3_render(void *h, const g3camera *c)
{
	m3 *m = h;
	if (!m) return;
	@autoreleasepool {
		CGSize sz = m->view.bounds.size;
		CGFloat k = m->layer.contentsScale;
		m->layer.frame = m->view.bounds;
		m->layer.drawableSize = CGSizeMake(sz.width * k, sz.height * k);
		if (sz.width < 1 || sz.height < 1) return;
		id<CAMetalDrawable> d = [m->layer nextDrawable];
		if (!d) return;
		int W = (int) (sz.width * k), H = (int) (sz.height * k);
		id<MTLTexture> dep = depth_for(m, W, H);
		id<MTLCommandBuffer> cb = encode(m, d.texture, dep, c, (int) sz.width, (int) sz.height);
		[cb presentDrawable:d];
		[cb commit];
		if (m->grab) {
			[cb waitUntilCompleted];
			m->grabbed = write_png(d.texture, W, H, m->grab);
			m->grab = NULL;
		}
		[dep release];
	}
}

// The frame the window shows, drawn now and read back from the drawable
// it is presented in, written as PNG: the on-screen path itself.
int g3_capture(void *h, const g3camera *c, const char *path)
{
	m3 *m = h;
	if (!m) return 0;
	m->grab = path;
	m->grabbed = 0;
	g3_render(h, c);
	m->grab = NULL;
	return m->grabbed;
}

// The view drawn offscreen at W by H and written to path as PNG.
int g3_snapshot(void *h, const g3camera *c, int W, int H, const char *path)
{
	m3 *m = h;
	if (!m) return 0;
	@autoreleasepool {
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                              width:W height:H mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> t = [m->dev newTextureWithDescriptor:td];
		id<MTLTexture> dep = depth_for(m, W, H);
		id<MTLCommandBuffer> cb = encode(m, t, dep, c, W, H);
		[cb commit];
		[cb waitUntilCompleted];
		int ok = write_png(t, W, H, path);
		[t release]; [dep release];
		return ok;
	}
}
