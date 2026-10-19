// vulkan.c: the 3D view on Windows (and, later, Linux) -- Vulkan drawing a
// mesh into a child window over a window's drawing area, as metal.m does
// on macOS: the same g3_* interface, the same camera (g3.h), the same
// shaders (shaders/*.vert, *.frag; metal.m's in GLSL), so both GPUs draw
// the same picture. shaders/*.h are generated from them by glslang
// (glslangValidator -V --vn spv_NAME, 16.6.0, BSD-3-Clause), kept here so a
// build needs no shader compiler. The child window lets the mouse through to the area
// (its window procedure answers HTTRANSPARENT), so its events reach libui-ng.
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <commctrl.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "g3.h"
#include "shaders/mesh_vert.h"
#include "shaders/tri_frag.h"
#include "shaders/line_frag.h"

// Vulkan is LOADED, not linked: vulkan-1.dll comes with a GPU's driver, and
// a machine without one must still open windows and draw in 2D -- linked,
// the whole library would fail to load there. Every function is looked up
// by vkGetInstanceProcAddr; g3_attach says why when there is none.
#define VK_FUNCTIONS(X) \
	X(vkAcquireNextImageKHR) X(vkAllocateCommandBuffers) X(vkAllocateMemory) X(vkBeginCommandBuffer) \
	X(vkBindBufferMemory) X(vkBindImageMemory) X(vkCmdBeginRenderPass) X(vkCmdBindIndexBuffer) \
	X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdCopyImageToBuffer) X(vkCmdDraw) \
	X(vkCmdDrawIndexed) X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdPushConstants) \
	X(vkCmdSetScissor) X(vkCmdSetViewport) X(vkCreateBuffer) X(vkCreateCommandPool) X(vkCreateDevice) \
	X(vkCreateFence) X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) \
	X(vkCreateImageView) X(vkCreatePipelineLayout) X(vkCreateRenderPass) X(vkCreateSemaphore) \
	X(vkCreateShaderModule) X(vkCreateSwapchainKHR) X(vkCreateWin32SurfaceKHR) X(vkDestroyBuffer) \
	X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyShaderModule) \
	X(vkDestroySwapchainKHR) X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkEnumeratePhysicalDevices) \
	X(vkFreeMemory) X(vkGetBufferMemoryRequirements) X(vkGetDeviceQueue) X(vkGetImageMemoryRequirements) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetSwapchainImagesKHR) X(vkMapMemory) X(vkQueuePresentKHR) X(vkQueueSubmit) \
	X(vkResetCommandBuffer) X(vkResetFences) X(vkUnmapMemory) X(vkWaitForFences)
#define VK_DECLARE(f) static PFN_##f f;
VK_FUNCTIONS(VK_DECLARE)
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance vkCreateInstance;
const char *clotran_ui_g3_error = "";

// the loader, and the one function needed before an instance; 0 without
static int load_loader(void)
{
	if (gipa) return 1;
	HMODULE lib = LoadLibraryA("vulkan-1.dll");
	if (!lib) { clotran_ui_g3_error = "no Vulkan here (vulkan-1.dll, which a GPU driver installs, is missing)"; return 0; }
	gipa = (PFN_vkGetInstanceProcAddr) (void *) GetProcAddress(lib, "vkGetInstanceProcAddr");
	if (gipa) vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
	if (!gipa || !vkCreateInstance) { clotran_ui_g3_error = "vulkan-1.dll has no vkGetInstanceProcAddr"; return 0; }
	return 1;
}

// every other function, through the instance; 0 when one is missing
static int load_functions(VkInstance inst)
{
	int ok = 1;
#define VK_LOAD(f) f = (PFN_##f) gipa(inst, #f); if (!f) ok = 0;
	VK_FUNCTIONS(VK_LOAD)
	if (!ok) clotran_ui_g3_error = "this Vulkan lacks a function the 3D view needs";
	return ok;
}

typedef struct { VkBuffer b; VkDeviceMemory m; } vbuf;
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; } vimg;

typedef struct {
	HWND area, child;
	VkInstance inst; VkSurfaceKHR surf; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
	VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore acq, done;
	VkRenderPass rp_show, rp_off; VkPipelineLayout layout; VkPipeline tri, line, tri_off, line_off;
	VkSwapchainKHR sc; uint32_t nimg, W, H; VkImage imgs[8]; VkImageView views[8]; VkFramebuffer fbs[8]; vimg depth;
	vbuf tpos, tcol, tidx, lpos, lcol; int nidx, nlv;
	float bg[3];
	g3camera cam;   // the last drawn, for a repaint the system asks for
} vk;

#define FMT VK_FORMAT_B8G8R8A8_UNORM
#define DFMT VK_FORMAT_D32_SFLOAT

static uint32_t memtype(vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
	return 0;
}

static vbuf mkbuf(vk *v, VkDeviceSize n, VkBufferUsageFlags use, const void *data)
{
	vbuf r = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	if (n == 0) n = 16;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
	vkCreateBuffer(v->dev, &bi, NULL, &r.b);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(v->dev, r.b, &mr);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
		memtype(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	vkAllocateMemory(v->dev, &ai, NULL, &r.m);
	vkBindBufferMemory(v->dev, r.b, r.m, 0);
	if (data) {
		void *p;
		vkMapMemory(v->dev, r.m, 0, n, 0, &p);
		memcpy(p, data, n);
		vkUnmapMemory(v->dev, r.m);
	}
	return r;
}

static void freebuf(vk *v, vbuf *b)
{
	if (b->b) vkDestroyBuffer(v->dev, b->b, NULL);
	if (b->m) vkFreeMemory(v->dev, b->m, NULL);
	b->b = VK_NULL_HANDLE; b->m = VK_NULL_HANDLE;
}

static vimg mkimg(vk *v, uint32_t W, uint32_t H, VkFormat f, VkImageUsageFlags use, VkImageAspectFlags aspect)
{
	vimg r;
	VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, { W, H, 1 }, 1, 1,
		VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
	vkCreateImage(v->dev, &ii, NULL, &r.i);
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(v->dev, r.i, &mr);
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
		memtype(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	vkAllocateMemory(v->dev, &ai, NULL, &r.m);
	vkBindImageMemory(v->dev, r.i, r.m, 0);
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, r.i, VK_IMAGE_VIEW_TYPE_2D, f,
		{ 0, 0, 0, 0 }, { aspect, 0, 1, 0, 1 } };
	vkCreateImageView(v->dev, &vi, NULL, &r.v);
	return r;
}

static void freeimg(vk *v, vimg *m)
{
	if (m->v) vkDestroyImageView(v->dev, m->v, NULL);
	if (m->i) vkDestroyImage(v->dev, m->i, NULL);
	if (m->m) vkFreeMemory(v->dev, m->m, NULL);
	memset(m, 0, sizeof *m);
}

static VkRenderPass mkpass(vk *v, VkImageLayout final)
{
	VkAttachmentDescription at[2] = {
		{ 0, FMT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
		  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, final },
		{ 0, DFMT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
		  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cr, NULL, &dr, 0, NULL };
	VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0 };
	VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, at, 1, &sp, 1, &dep };
	VkRenderPass rp;
	vkCreateRenderPass(v->dev, &ri, NULL, &rp);
	return rp;
}

static VkShaderModule module(vk *v, const uint32_t *code, size_t n)
{
	VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, code };
	VkShaderModule m;
	vkCreateShaderModule(v->dev, &si, NULL, &m);
	return m;
}

static VkPipeline mkpipe(vk *v, VkRenderPass rp, VkShaderModule vs, VkShaderModule fs, VkPrimitiveTopology topo)
{
	VkPipelineShaderStageCreateInfo st[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
	VkVertexInputBindingDescription vb[2] = { { 0, 12, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 12, VK_VERTEX_INPUT_RATE_VERTEX } };
	VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0 } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 2, vb, 2, va };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, VK_FALSE };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
		VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE,
		VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState ba = { VK_FALSE };
	ba.colorWriteMask = 0xf;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
	VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms,
		&ds, &cb, &dy, v->layout, rp, 0, VK_NULL_HANDLE, -1 };
	VkPipeline p;
	vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
	return p;
}

static int show(vk *v, const g3camera *c, const char *grab);

// The child window Vulkan draws in: transparent to the mouse, so the area
// under it gets every event; it paints nothing itself but the last picture.
static LRESULT CALLBACK childproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
	vk *v = (vk *) GetWindowLongPtrW(hw, GWLP_USERDATA);
	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint(hw, &ps);
		EndPaint(hw, &ps);
		if (v && v->sc) show(v, &v->cam, NULL);
		return 0;
	}
	}
	return DefWindowProcW(hw, msg, wp, lp);
}

void *g3_attach(uiArea *a)
{
	vk *v = calloc(1, sizeof *v);
	v->area = (HWND) uiControlHandle(uiControl(a));
	RECT r;
	GetClientRect(v->area, &r);
	static int registered;
	if (!registered) {
		WNDCLASSW wc = { 0, childproc, 0, 0, GetModuleHandle(NULL), NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"clotranVulkan" };
		RegisterClassW(&wc);
		registered = 1;
	}
	// the area does not paint over its child
	SetWindowLongPtrW(v->area, GWL_STYLE, GetWindowLongPtrW(v->area, GWL_STYLE) | WS_CLIPCHILDREN);
	v->child = CreateWindowExW(0, L"clotranVulkan", L"", WS_CHILD | WS_VISIBLE, 0, 0, r.right, r.bottom,
	                           v->area, NULL, GetModuleHandle(NULL), NULL);
	SetWindowLongPtrW(v->child, GWLP_USERDATA, (LONG_PTR) v);
	if (!load_loader()) goto fail;
	const char *ext[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "clotran-ui", 1, "clotran-ui", 1, VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, ext };
	if (vkCreateInstance(&ii, NULL, &v->inst) != VK_SUCCESS) { clotran_ui_g3_error = "Vulkan would not start (vkCreateInstance)"; goto fail; }
	if (!load_functions(v->inst)) goto fail;
	VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0, GetModuleHandle(NULL), v->child };
	if (vkCreateWin32SurfaceKHR(v->inst, &si, NULL, &v->surf) != VK_SUCCESS) goto fail;
	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	vkEnumeratePhysicalDevices(v->inst, &n, pds);
	for (uint32_t i = 0; i < n && !v->pd; i++) {
		uint32_t nq = 16;
		VkQueueFamilyProperties qp[16];
		vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
		for (uint32_t k = 0; k < nq; k++) {
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], k, v->surf, &present);
			if ((qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { v->pd = pds[i]; v->qf = k; break; }
		}
	}
	if (!v->pd) { clotran_ui_g3_error = "no GPU here can draw to a window with Vulkan"; goto fail; }
	float prio = 1;
	VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, v->qf, 1, &prio };
	const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 1, dext, NULL };
	if (vkCreateDevice(v->pd, &di, NULL, &v->dev) != VK_SUCCESS) goto fail;
	vkGetDeviceQueue(v->dev, v->qf, 0, &v->q);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, v->qf };
	vkCreateCommandPool(v->dev, &pci, NULL, &v->pool);
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, v->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
	vkAllocateCommandBuffers(v->dev, &cai, &v->cb);
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
	vkCreateFence(v->dev, &fi, NULL, &v->fence);
	VkSemaphoreCreateInfo smi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
	vkCreateSemaphore(v->dev, &smi, NULL, &v->acq);
	vkCreateSemaphore(v->dev, &smi, NULL, &v->done);
	v->rp_show = mkpass(v, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	v->rp_off = mkpass(v, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
	VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pr };
	vkCreatePipelineLayout(v->dev, &li, NULL, &v->layout);
	VkShaderModule vs = module(v, spv_mesh_vert, sizeof spv_mesh_vert);
	VkShaderModule ft = module(v, spv_tri_frag, sizeof spv_tri_frag);
	VkShaderModule fl = module(v, spv_line_frag, sizeof spv_line_frag);
	v->tri = mkpipe(v, v->rp_show, vs, ft, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	v->line = mkpipe(v, v->rp_show, vs, fl, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	v->tri_off = mkpipe(v, v->rp_off, vs, ft, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	v->line_off = mkpipe(v, v->rp_off, vs, fl, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	vkDestroyShaderModule(v->dev, vs, NULL);
	vkDestroyShaderModule(v->dev, ft, NULL);
	vkDestroyShaderModule(v->dev, fl, NULL);
	return v;
fail:
	if (v->child) DestroyWindow(v->child);
	free(v);
	return NULL;
}

void g3_mesh(void *h, const g3scene *s)
{
	vk *v = h;
	if (!v) return;
	vkDeviceWaitIdle(v->dev);
	freebuf(v, &v->tpos); freebuf(v, &v->tcol); freebuf(v, &v->tidx); freebuf(v, &v->lpos); freebuf(v, &v->lcol);
	v->tpos = mkbuf(v, 12 * (VkDeviceSize) s->nv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s->pos);
	v->tcol = mkbuf(v, 12 * (VkDeviceSize) s->nv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s->col);
	v->tidx = mkbuf(v, 12 * (VkDeviceSize) s->nt, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, s->idx);
	v->lpos = mkbuf(v, 12 * (VkDeviceSize) s->nlv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s->lpos);
	v->lcol = mkbuf(v, 12 * (VkDeviceSize) s->nlv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s->lcol);
	v->nidx = 3 * s->nt;
	v->nlv = s->nlv;
	memcpy(v->bg, s->bg, sizeof v->bg);
}

static void drop_swapchain(vk *v)
{
	for (uint32_t i = 0; i < v->nimg; i++) {
		vkDestroyFramebuffer(v->dev, v->fbs[i], NULL);
		vkDestroyImageView(v->dev, v->views[i], NULL);
	}
	freeimg(v, &v->depth);
	if (v->sc) vkDestroySwapchainKHR(v->dev, v->sc, NULL);
	v->sc = VK_NULL_HANDLE;
	v->nimg = 0;
}

// the swapchain for the area's size now; 0 when the area has none
static int swapchain(vk *v)
{
	RECT r;
	GetClientRect(v->area, &r);
	uint32_t W = r.right, H = r.bottom;
	if (W == 0 || H == 0) return 0;
	MoveWindow(v->child, 0, 0, W, H, FALSE);
	if (v->sc && W == v->W && H == v->H) return 1;
	vkDeviceWaitIdle(v->dev);
	drop_swapchain(v);
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->pd, v->surf, &caps);
	if (caps.currentExtent.width != 0xffffffff) { W = caps.currentExtent.width; H = caps.currentExtent.height; }
	if (W == 0 || H == 0) return 0;
	uint32_t want = caps.minImageCount + 1;
	if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
	VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, v->surf, want, FMT,
		VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, { W, H }, 1,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
		caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE };
	if (vkCreateSwapchainKHR(v->dev, &ci, NULL, &v->sc) != VK_SUCCESS) { v->sc = VK_NULL_HANDLE; return 0; }
	v->nimg = 8;
	vkGetSwapchainImagesKHR(v->dev, v->sc, &v->nimg, v->imgs);
	v->depth = mkimg(v, W, H, DFMT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	for (uint32_t i = 0; i < v->nimg; i++) {
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->imgs[i], VK_IMAGE_VIEW_TYPE_2D, FMT,
			{ 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		vkCreateImageView(v->dev, &vi, NULL, &v->views[i]);
		VkImageView at[2] = { v->views[i], v->depth.v };
		VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, v->rp_show, 2, at, W, H, 1 };
		vkCreateFramebuffer(v->dev, &fi, NULL, &v->fbs[i]);
	}
	v->W = W; v->H = H;
	return 1;
}

static void record(vk *v, VkRenderPass rp, VkFramebuffer fb, VkPipeline tri, VkPipeline line, const g3camera *c, uint32_t W, uint32_t H)
{
	float u[8];
	g3_uniforms(c, (int) W, (int) H, u);
	VkClearValue cl[2];
	cl[0].color.float32[0] = v->bg[0]; cl[0].color.float32[1] = v->bg[1]; cl[0].color.float32[2] = v->bg[2]; cl[0].color.float32[3] = 1;
	cl[1].depthStencil.depth = 1; cl[1].depthStencil.stencil = 0;
	VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, { { 0, 0 }, { W, H } }, 2, cl };
	vkCmdBeginRenderPass(v->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport vp = { 0, 0, (float) W, (float) H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	vkCmdSetViewport(v->cb, 0, 1, &vp);
	vkCmdSetScissor(v->cb, 0, 1, &sc);
	VkDeviceSize off[2] = { 0, 0 };
	vkCmdPushConstants(v->cb, v->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof u, u);
	if (v->nidx) {
		VkBuffer bs[2] = { v->tpos.b, v->tcol.b };
		vkCmdBindPipeline(v->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tri);
		vkCmdBindVertexBuffers(v->cb, 0, 2, bs, off);
		vkCmdBindIndexBuffer(v->cb, v->tidx.b, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(v->cb, v->nidx, 1, 0, 0, 0);
	}
	if (v->nlv) {
		VkBuffer bs[2] = { v->lpos.b, v->lcol.b };
		vkCmdBindPipeline(v->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, line);
		vkCmdBindVertexBuffers(v->cb, 0, 2, bs, off);
		vkCmdDraw(v->cb, v->nlv, 1, 0, 0);
	}
	vkCmdEndRenderPass(v->cb);
}

static void barrier(vk *v, VkImage img, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
		from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(v->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// the image's pixels, BGRA, copied out (then the image is in layout `after`)
static vbuf readback(vk *v, VkImage img, uint32_t W, uint32_t H)
{
	vbuf out = mkbuf(v, 4 * (VkDeviceSize) W * H, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);
	VkBufferImageCopy cp = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
	vkCmdCopyImageToBuffer(v->cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.b, 1, &cp);
	return out;
}

static void submit_wait(vk *v, int with_semaphores)
{
	VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, with_semaphores ? 1 : 0, &v->acq, &ws, 1, &v->cb,
		with_semaphores ? 1 : 0, &v->done };
	vkResetFences(v->dev, 1, &v->fence);
	vkQueueSubmit(v->q, 1, &si, v->fence);
	vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, UINT64_MAX);
}

// pixels (BGRA, top-down) written to path as a 24-bit BMP
static int write_bmp(vk *v, vbuf *px, uint32_t W, uint32_t H, const char *path)
{
	unsigned char *p;
	vkMapMemory(v->dev, px->m, 0, 4 * (VkDeviceSize) W * H, 0, (void **) &p);
	FILE *f = fopen(path, "wb");
	int ok = f != NULL;
	if (f) {
		int stride = ((W * 3 + 3) / 4) * 4;
		BITMAPFILEHEADER fh = { 0x4d42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
		BITMAPINFOHEADER ih = { sizeof ih, (LONG) W, (LONG) H, 1, 24, BI_RGB };
		fh.bfSize = fh.bfOffBits + stride * H;
		fwrite(&fh, sizeof fh, 1, f);
		fwrite(&ih, sizeof ih, 1, f);
		unsigned char *row = calloc(stride, 1);
		for (int y = (int) H - 1; y >= 0; y--) {
			for (uint32_t x = 0; x < W; x++) memcpy(row + 3 * x, p + 4 * ((size_t) y * W + x), 3);
			fwrite(row, stride, 1, f);
		}
		free(row);
		fclose(f);
	}
	vkUnmapMemory(v->dev, px->m);
	return ok;
}

static int show(vk *v, const g3camera *c, const char *grab)
{
	if (!swapchain(v)) return 0;
	uint32_t i;
	VkResult r = vkAcquireNextImageKHR(v->dev, v->sc, UINT64_MAX, v->acq, VK_NULL_HANDLE, &i);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) { v->W = 0; return 0; }
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	vkResetCommandBuffer(v->cb, 0);
	vkBeginCommandBuffer(v->cb, &bi);
	record(v, v->rp_show, v->fbs[i], v->tri, v->line, c, v->W, v->H);
	vbuf px = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	if (grab) {   // the frame about to be shown, read back
		barrier(v, v->imgs[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		px = readback(v, v->imgs[i], v->W, v->H);
		barrier(v, v->imgs[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	vkEndCommandBuffer(v->cb);
	submit_wait(v, 1);
	VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &v->done, 1, &v->sc, &i, NULL };
	r = vkQueuePresentKHR(v->q, &pi);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) v->W = 0;
	int ok = 1;
	if (grab) { ok = write_bmp(v, &px, v->W ? v->W : 1, v->H ? v->H : 1, grab); freebuf(v, &px); }
	return ok;
}

void g3_render(void *h, const g3camera *c)
{
	vk *v = h;
	if (!v) return;
	v->cam = *c;
	show(v, c, NULL);
}

int g3_capture(void *h, const g3camera *c, const char *path)
{
	vk *v = h;
	if (!v) return 0;
	return show(v, c, path);
}

// The view drawn offscreen at W by H and written as a 24-bit BMP.
int g3_snapshot(void *h, const g3camera *c, int W, int H, const char *path)
{
	vk *v = h;
	if (!v) return 0;
	vimg col = mkimg(v, W, H, FMT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	vimg dep = mkimg(v, W, H, DFMT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	VkImageView at[2] = { col.v, dep.v };
	VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, v->rp_off, 2, at, (uint32_t) W, (uint32_t) H, 1 };
	VkFramebuffer fb;
	vkCreateFramebuffer(v->dev, &fi, NULL, &fb);
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	vkResetCommandBuffer(v->cb, 0);
	vkBeginCommandBuffer(v->cb, &bi);
	record(v, v->rp_off, fb, v->tri_off, v->line_off, c, W, H);
	vbuf px = readback(v, col.i, W, H);
	vkEndCommandBuffer(v->cb);
	submit_wait(v, 0);
	int ok = write_bmp(v, &px, W, H, path);
	freebuf(v, &px);
	vkDestroyFramebuffer(v->dev, fb, NULL);
	freeimg(v, &col);
	freeimg(v, &dep);
	return ok;
}

// libui-ng gives an area no wheel: the window is subclassed, and a wheel
// over it (WM_MOUSEWHEEL bubbles up from the focused control) zooms
extern void clotran_ui_on_scroll(void *native_window, double dy);
static LRESULT CALLBACK wheel(HWND hw, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
	if (msg == WM_MOUSEWHEEL) {
		clotran_ui_on_scroll((void *) hw, -(double) GET_WHEEL_DELTA_WPARAM(wp) / 40.0);
		return 0;
	}
	return DefSubclassProc(hw, msg, wp, lp);
}

void clotran_ui_watch_scroll_win(void *window_hwnd)
{
	SetWindowSubclass((HWND) window_hwnd, wheel, 1, 0);
}
