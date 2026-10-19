/*
 * OpenXWA Vulkan foveation shim (Quest).
 *
 * Loaded by SDL in place of libvulkan.so (SDL_HINT_VULKAN_LIBRARY); passes
 * everything through to the system loader except a handful of calls, to
 * give the 3D scene fixed foveated rendering with VK_EXT_fragment_density_map
 * - the mechanism Meta's own FFR uses - without changes to SDL:
 *
 *   vkCreateDevice      enables the extension and its features when the
 *                       device has them (fragmentDensityMap and
 *                       fragmentDensityMapNonSubsampledImages).
 *   vkCreateRenderPass  adds a fragment density map attachment to the render
 *                       passes that draw the 3D scene: every multisampled one,
 *                       and without MSAA every pass with colour and depth (the
 *                       scene's forward pass; the tonemap, bloom, HUD and
 *                       panel passes have no depth). SDL makes the pipelines
 *                       against the changed pass.
 *   vkCreateFramebuffer gives those passes their map: the foveated one when the
 *                       framebuffer is the VR eye size, else a full-density map
 *                       (no change).
 *
 * Attachments stay ordinary (not subsampled): the GPU shades the periphery in
 * larger fragments and writes them to every covered pixel.
 *
 * It also times the GPU work of every render pass (timestamps around
 * vkCmdBeginRenderPass / vkCmdEndRenderPass) and logs every 10 s what each
 * kind of pass costs per frame (XWA_XR_GPU_TIMING=0 turns that off).
 *
 * Environment (read when needed):
 *   XWA_XR_FOVEATION         0/off, low (default), medium, high
 *   XWA_XR_FOVEATION_1X      0 = only multisampled passes (default 1: also the
 *                            single-sample colour + depth passes)
 *   OPENXWA_FOVEATION_EYE    "w h cx_left cy_left cx_right cy_right": the eye
 *                            framebuffer size and each eye's optical centre in
 *                            [-1,1] image coordinates (+y down), set by the game
 *                            before it renders the first eye.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log[4096];

/* Logs to logcat (liblog, looked up at run time) and keeps the line for the
 * game's log. */
static void shim_log(int prio, const char* fmt, ...) {
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);
	static int (*print)(int, const char*, const char*, ...);
	static int tried;
	if (!tried) {
		void* liblog = dlopen("liblog.so", RTLD_NOW | RTLD_LOCAL);
		print = liblog ? (int (*)(int, const char*, const char*, ...))dlsym(liblog, "__android_log_print") : NULL;
		tried = 1;
	}
	if (print) {
		print(prio, "openxwa.vkshim", "%s", text);
	}
	/* Kept for the game, which forwards it to its own log
	 * (OpenXWA_VkShim_TakeLog). */
	pthread_mutex_lock(&g_log_lock);
	const size_t used = strlen(g_log), add = strlen(text);
	if (used + add + 3 < sizeof g_log) {
		g_log[used] = prio >= 5 ? '!' : ' ';
		memcpy(g_log + used + 1, text, add);
		g_log[used + 1 + add] = '\n';
		g_log[used + 2 + add] = '\0';
	}
	pthread_mutex_unlock(&g_log_lock);
}

/* Moves the messages logged so far into `out` (lines; a leading '!' marks a
 * warning). Returns their length. */
__attribute__((visibility("default"))) int OpenXWA_VkShim_TakeLog(char* out, int size) {
	pthread_mutex_lock(&g_log_lock);
	int n = (int)strlen(g_log);
	if (n >= size) {
		n = size - 1;
	}
	if (n > 0) {
		memcpy(out, g_log, (size_t)n);
	}
	out[n > 0 ? n : 0] = '\0';
	g_log[0] = '\0';
	pthread_mutex_unlock(&g_log_lock);
	return n;
}
#define LOGI(...) shim_log(4, __VA_ARGS__)
#define LOGW(...) shim_log(5, __VA_ARGS__)

#define SHIM_EXPORT __attribute__((visibility("default")))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void* g_lib;
static PFN_vkGetInstanceProcAddr g_real_gipa;
static VkInstance g_instance;

/* The device we foveate (one). */
static struct {
	VkDevice device;
	VkPhysicalDevice physical;
	uint32_t queue_family;
	int active;
	VkExtent2D texel; /* map texel size in framebuffer pixels */
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkCreateRenderPass CreateRenderPass;
	PFN_vkDestroyRenderPass DestroyRenderPass;
	PFN_vkCreateFramebuffer CreateFramebuffer;
	PFN_vkDestroyFramebuffer DestroyFramebuffer;
	PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkCreateImageView CreateImageView;
	PFN_vkDestroyImageView DestroyImageView;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
	PFN_vkGetDeviceQueue GetDeviceQueue;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkCmdEndRenderPass CmdEndRenderPass;
	PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
	PFN_vkCmdResetQueryPool CmdResetQueryPool;
	PFN_vkCreateQueryPool CreateQueryPool;
	PFN_vkDestroyQueryPool DestroyQueryPool;
	PFN_vkGetQueryPoolResults GetQueryPoolResults;
	VkPhysicalDeviceMemoryProperties memory;
} g_dev;

/* Render passes that got a map attachment. */
#define SHIM_MAX_PASSES 512
static VkRenderPass g_passes[SHIM_MAX_PASSES];
static int g_pass_count;

/* Density maps: one full-density map (any size up to its extent) and the
 * foveated one for the eye framebuffer. */
typedef struct ShimMap {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t w, h; /* texels */
} ShimMap;
static ShimMap g_full, g_eye;

/* Framebuffers that carry the foveated map, and how often a pass begins on
 * one (logged every few seconds: proof the GPU gets the map). */
#define SHIM_MAX_EYE_FBS 64
static VkFramebuffer g_eye_fbs[SHIM_MAX_EYE_FBS];
static int g_eye_fb_count;
static unsigned g_eye_begins, g_other_begins;
static double g_count_since;
static int g_eye_w, g_eye_h, g_eye_tried, g_logged_eye;

/* ---- GPU timing --------------------------------------------------------- */

static double shim_now(void);
static int fmt_is_depth(VkFormat f);

/* What a render pass is: its first attachment's format, samples, attachment
 * count; plus the framebuffer size. Passes of one kind are summed. */
typedef struct ShimPassDesc {
	VkRenderPass pass;
	VkFormat format;
	VkSampleCountFlagBits samples;
	uint32_t attachments;
	int depth_only;
} ShimPassDesc;
typedef struct ShimFbDesc {
	VkFramebuffer fb;
	VkRenderPass pass;
	uint32_t w, h;
} ShimFbDesc;
typedef struct ShimKind {
	VkFormat format;
	VkSampleCountFlagBits samples;
	uint32_t attachments, w, h;
	int depth_only, eye;
	double ns;
	unsigned count;
} ShimKind;

#define SHIM_RP_TABLE 1024
#define SHIM_FB_TABLE 4096
#define SHIM_KINDS 64
#define SHIM_QUERIES 4096 /* two per pass */
#define SHIM_OPEN 16

static struct {
	int enabled;
	VkQueryPool pool;
	float period_ns;
	uint64_t valid_mask;
	ShimPassDesc rp[SHIM_RP_TABLE];
	ShimFbDesc fb[SHIM_FB_TABLE];
	ShimKind kinds[SHIM_KINDS];
	int kind_count;
	/* Ring of query pairs: kind, recording time; -1 = free. */
	int slot_kind[SHIM_QUERIES / 2];
	double slot_time[SHIM_QUERIES / 2];
	uint32_t write, read; /* pair indices */
	uint64_t last_start;
	struct {
		VkCommandBuffer cb;
		uint32_t pair;
	} open[SHIM_OPEN];
	unsigned frames;
	double since, last_harvest;
} g_t;

static uint32_t shim_hash(uint64_t h, uint32_t size) {
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	return (uint32_t)(h & (size - 1));
}

static int g_rp_used, g_fb_used;

static void rp_put(const ShimPassDesc* d) {
	if (g_rp_used >= SHIM_RP_TABLE - 1) {
		return; /* keep a free slot: lookups and deletes stop at one */
	}
	uint32_t i = shim_hash((uint64_t)(uintptr_t)d->pass, SHIM_RP_TABLE);
	for (uint32_t n = 0; n < SHIM_RP_TABLE; ++n, i = (i + 1) & (SHIM_RP_TABLE - 1)) {
		if (g_t.rp[i].pass == VK_NULL_HANDLE || g_t.rp[i].pass == d->pass) {
			g_rp_used += g_t.rp[i].pass == VK_NULL_HANDLE;
			g_t.rp[i] = *d;
			return;
		}
	}
}

static const ShimPassDesc* rp_get(VkRenderPass p) {
	uint32_t i = shim_hash((uint64_t)(uintptr_t)p, SHIM_RP_TABLE);
	for (uint32_t n = 0; n < SHIM_RP_TABLE; ++n, i = (i + 1) & (SHIM_RP_TABLE - 1)) {
		if (g_t.rp[i].pass == p) {
			return &g_t.rp[i];
		}
		if (g_t.rp[i].pass == VK_NULL_HANDLE) {
			return NULL;
		}
	}
	return NULL;
}

/* Removal by backward-shift keeps the probe chains intact. */
#define SHIM_TABLE_DEL(table, size, field, key)                                                       \
	do {                                                                                              \
		uint32_t i_ = shim_hash((uint64_t)(uintptr_t)(key), (size));                                 \
		for (uint32_t n_ = 0; n_ < (size); ++n_, i_ = (i_ + 1) & ((size)-1)) {                        \
			if ((table)[i_].field == VK_NULL_HANDLE) {                                                \
				break;                                                                                \
			}                                                                                         \
			if ((table)[i_].field == (key)) {                                                         \
				uint32_t j_ = i_;                                                                     \
				for (;;) {                                                                            \
					j_ = (j_ + 1) & ((size)-1);                                                       \
					if ((table)[j_].field == VK_NULL_HANDLE) {                                        \
						break;                                                                        \
					}                                                                                 \
					const uint32_t h_ = shim_hash((uint64_t)(uintptr_t)(table)[j_].field, (size));    \
					if ((j_ > i_ && (h_ <= i_ || h_ > j_)) || (j_ < i_ && (h_ <= i_ && h_ > j_))) {   \
						(table)[i_] = (table)[j_];                                                    \
						i_ = j_;                                                                      \
					}                                                                                 \
				}                                                                                     \
				memset(&(table)[i_], 0, sizeof(table)[i_]);                                           \
				break;                                                                                \
			}                                                                                         \
		}                                                                                             \
	} while (0)

static void fb_put(const ShimFbDesc* d) {
	if (g_fb_used >= SHIM_FB_TABLE - 1) {
		return;
	}
	uint32_t i = shim_hash((uint64_t)(uintptr_t)d->fb, SHIM_FB_TABLE);
	for (uint32_t n = 0; n < SHIM_FB_TABLE; ++n, i = (i + 1) & (SHIM_FB_TABLE - 1)) {
		if (g_t.fb[i].fb == VK_NULL_HANDLE || g_t.fb[i].fb == d->fb) {
			g_fb_used += g_t.fb[i].fb == VK_NULL_HANDLE;
			g_t.fb[i] = *d;
			return;
		}
	}
}

static const ShimFbDesc* fb_get(VkFramebuffer f) {
	uint32_t i = shim_hash((uint64_t)(uintptr_t)f, SHIM_FB_TABLE);
	for (uint32_t n = 0; n < SHIM_FB_TABLE; ++n, i = (i + 1) & (SHIM_FB_TABLE - 1)) {
		if (g_t.fb[i].fb == f) {
			return &g_t.fb[i];
		}
		if (g_t.fb[i].fb == VK_NULL_HANDLE) {
			return NULL;
		}
	}
	return NULL;
}

static const char* fmt_name(VkFormat f, char* buf, size_t n) {
	switch (f) {
	case VK_FORMAT_R8G8B8A8_SRGB: return "rgba8-srgb";
	case VK_FORMAT_B8G8R8A8_SRGB: return "bgra8-srgb";
	case VK_FORMAT_R8G8B8A8_UNORM: return "rgba8";
	case VK_FORMAT_B8G8R8A8_UNORM: return "bgra8";
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "r11g11b10f";
	case VK_FORMAT_R16G16B16A16_SFLOAT: return "rgba16f";
	case VK_FORMAT_R16G16_SFLOAT: return "rg16f";
	case VK_FORMAT_R8G8_UNORM: return "rg8";
	case VK_FORMAT_R8_UNORM: return "r8";
	case VK_FORMAT_D16_UNORM: return "d16";
	case VK_FORMAT_D24_UNORM_S8_UINT: return "d24s8";
	case VK_FORMAT_D32_SFLOAT: return "d32f";
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return "d32fs8";
	default: snprintf(buf, n, "fmt%d", (int)f); return buf;
	}
}

static int t_kind(const ShimPassDesc* rp, const ShimFbDesc* fb, int eye) {
	for (int k = 0; k < g_t.kind_count; ++k) {
		const ShimKind* s = &g_t.kinds[k];
		if (s->format == rp->format && s->samples == rp->samples && s->attachments == rp->attachments &&
			s->w == fb->w && s->h == fb->h && s->depth_only == rp->depth_only) {
			return k;
		}
	}
	if (g_t.kind_count >= SHIM_KINDS) {
		return SHIM_KINDS - 1; /* lumped with the last */
	}
	ShimKind* s = &g_t.kinds[g_t.kind_count];
	memset(s, 0, sizeof *s);
	s->format = rp->format;
	s->samples = rp->samples;
	s->attachments = rp->attachments;
	s->depth_only = rp->depth_only;
	s->w = fb->w;
	s->h = fb->h;
	s->eye = eye;
	return g_t.kind_count++;
}

static void t_setup(VkPhysicalDevice phys) {
	const char* env = getenv("XWA_XR_GPU_TIMING");
	memset(&g_t, 0, sizeof g_t);
	for (int i = 0; i < SHIM_QUERIES / 2; ++i) {
		g_t.slot_kind[i] = -1;
	}
	if ((env && env[0] == '0') || !g_dev.CmdWriteTimestamp || !g_dev.CmdResetQueryPool || !g_dev.CreateQueryPool ||
		!g_dev.GetQueryPoolResults || !g_dev.CmdEndRenderPass || !g_dev.DestroyQueryPool || !g_dev.GetDeviceQueue ||
		!g_dev.AllocateCommandBuffers || !g_dev.BeginCommandBuffer || !g_dev.EndCommandBuffer ||
		!g_dev.QueueSubmit || !g_dev.QueueWaitIdle || !g_dev.DestroyCommandPool) {
		return;
	}
	PFN_vkGetPhysicalDeviceProperties props =
		(PFN_vkGetPhysicalDeviceProperties)g_real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
	PFN_vkGetPhysicalDeviceQueueFamilyProperties qprops = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_real_gipa(
		g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
	if (!props || !qprops) {
		return;
	}
	VkPhysicalDeviceProperties p;
	props(phys, &p);
	uint32_t nq = 0;
	qprops(phys, &nq, NULL);
	VkQueueFamilyProperties q[16];
	nq = nq > 16 ? 16 : nq;
	qprops(phys, &nq, q);
	const uint32_t bits = g_dev.queue_family < nq ? q[g_dev.queue_family].timestampValidBits : 0;
	if (bits == 0 || p.limits.timestampPeriod <= 0.0f) {
		LOGW("GPU timing: no timestamps on this queue");
		return;
	}
	const VkQueryPoolCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = SHIM_QUERIES,
	};
	if (g_dev.CreateQueryPool(g_dev.device, &qci, NULL, &g_t.pool) != VK_SUCCESS) {
		LOGW("GPU timing: no query pool");
		return;
	}
	/* Every query starts reset (read before its first use, it must say "not
	 * ready", not garbage). */
	{
		int reset_ok = 0;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cb = VK_NULL_HANDLE;
		const VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
											  .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
											  .queueFamilyIndex = g_dev.queue_family };
		if (g_dev.CreateCommandPool && g_dev.CreateCommandPool(g_dev.device, &pci, NULL, &pool) == VK_SUCCESS) {
			const VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
													  .commandPool = pool,
													  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
													  .commandBufferCount = 1 };
			const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
												  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
			VkQueue queue = VK_NULL_HANDLE;
			g_dev.GetDeviceQueue(g_dev.device, g_dev.queue_family, 0, &queue);
			if (queue && g_dev.AllocateCommandBuffers(g_dev.device, &cai, &cb) == VK_SUCCESS &&
				g_dev.BeginCommandBuffer(cb, &bi) == VK_SUCCESS) {
				g_dev.CmdResetQueryPool(cb, g_t.pool, 0, SHIM_QUERIES);
				const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
										  .commandBufferCount = 1,
										  .pCommandBuffers = &cb };
				reset_ok = g_dev.EndCommandBuffer(cb) == VK_SUCCESS &&
						   g_dev.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
						   g_dev.QueueWaitIdle(queue) == VK_SUCCESS;
			}
			g_dev.DestroyCommandPool(g_dev.device, pool, NULL);
		}
		if (!reset_ok) {
			LOGW("GPU timing: could not reset the query pool");
			g_dev.DestroyQueryPool(g_dev.device, g_t.pool, NULL);
			g_t.pool = VK_NULL_HANDLE;
			return;
		}
	}
	g_t.period_ns = p.limits.timestampPeriod;
	g_t.valid_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
	g_t.enabled = 1;
	LOGI("GPU timing on (%u-bit timestamps, %.2f ns per tick); per-pass costs every 10 s", bits,
		 (double)g_t.period_ns);
}

/* Collects finished pairs in order (call with g_lock held). */
static void t_harvest(double now) {
	while (g_t.read != g_t.write) {
		const uint32_t pair = g_t.read;
		if (g_t.slot_kind[pair] < 0) {
			g_t.read = (pair + 1) % (SHIM_QUERIES / 2);
			continue;
		}
		uint64_t data[4];
		const VkResult r = g_dev.GetQueryPoolResults(g_dev.device, g_t.pool, pair * 2, 2, sizeof data, data,
													 2 * sizeof(uint64_t),
													 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		const uint64_t start = data[0] & g_t.valid_mask, end = data[2] & g_t.valid_mask;
		/* An old lap's values look available until the reset runs: they are
		 * from a lap ago (about half a second), so accept only times that are
		 * at most 100 ms behind the newest seen (command buffers may run in
		 * another order than they were recorded). */
		const uint64_t tol = (uint64_t)(100.0e6 / (double)g_t.period_ns);
		const int ready = (r == VK_SUCCESS || r == VK_NOT_READY) && data[1] && data[3] && end >= start &&
						  start + tol >= g_t.last_start;
		if (!ready) {
			if (now - g_t.slot_time[pair] < 2.0) {
				return; /* not done yet */
			}
		} else {
			ShimKind* k = &g_t.kinds[g_t.slot_kind[pair]];
			k->ns += (double)(end - start) * (double)g_t.period_ns;
			k->count++;
			if (start > g_t.last_start) {
				g_t.last_start = start;
			}
		}
		g_t.slot_kind[pair] = -1;
		g_t.read = (pair + 1) % (SHIM_QUERIES / 2);
	}
}

static void t_report(double now) {
	if (g_t.since == 0.0) {
		g_t.since = now;
		return;
	}
	if (now - g_t.since < 10.0) {
		return;
	}
	const unsigned frames = g_t.frames ? g_t.frames : 1;
	int order[SHIM_KINDS];
	double total = 0.0;
	for (int k = 0; k < g_t.kind_count; ++k) {
		order[k] = k;
		total += g_t.kinds[k].ns;
	}
	for (int a = 1; a < g_t.kind_count; ++a) {
		for (int b = a; b > 0 && g_t.kinds[order[b]].ns > g_t.kinds[order[b - 1]].ns; --b) {
			const int t = order[b];
			order[b] = order[b - 1];
			order[b - 1] = t;
		}
	}
	LOGI("GPU per frame over %.0f s (%u frames, %.0f fps): %.2f ms in render passes", now - g_t.since, g_t.frames,
		 g_t.frames / (now - g_t.since), total / 1.0e6 / frames);
	for (int i = 0; i < g_t.kind_count && i < 10; ++i) {
		const ShimKind* k = &g_t.kinds[order[i]];
		if (k->count == 0) {
			continue;
		}
		char buf[16];
		LOGI("  %5.2f ms  %4.1f passes  %ux%u %s%s msaa%u%s", k->ns / 1.0e6 / frames, (double)k->count / frames, k->w,
			 k->h, k->depth_only ? "depth" : fmt_name(k->format, buf, sizeof buf),
			 k->attachments > 1 && !k->depth_only ? "+" : "", (unsigned)k->samples, k->eye ? " (foveated eye)" : "");
	}
	for (int k = 0; k < g_t.kind_count; ++k) {
		g_t.kinds[k].ns = 0.0;
		g_t.kinds[k].count = 0;
	}
	g_t.frames = 0;
	g_t.since = now;
}

static void t_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, int eye) {
	const ShimFbDesc* fb = fb_get(info->framebuffer);
	const ShimPassDesc* rp = fb ? rp_get(fb->pass) : NULL;
	if (!rp) {
		return;
	}
	int o = -1;
	for (int i = 0; i < SHIM_OPEN; ++i) {
		if (g_t.open[i].cb == cb || (o < 0 && g_t.open[i].cb == VK_NULL_HANDLE)) {
			o = i;
			if (g_t.open[i].cb == cb) {
				break;
			}
		}
	}
	if (o < 0) {
		return;
	}
	const uint32_t pair = g_t.write;
	const uint32_t next = (pair + 1) % (SHIM_QUERIES / 2);
	if (next == g_t.read) {
		/* Ring full (results not coming back): drop the oldest. */
		g_t.slot_kind[g_t.read] = -1;
		g_t.read = (g_t.read + 1) % (SHIM_QUERIES / 2);
	}
	g_t.write = next;
	g_t.slot_kind[pair] = t_kind(rp, fb, eye);
	g_t.slot_time[pair] = shim_now();
	g_t.open[o].cb = cb;
	g_t.open[o].pair = pair;
	g_dev.CmdResetQueryPool(cb, g_t.pool, pair * 2, 2);
	g_dev.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_t.pool, pair * 2);
}

static void t_end(VkCommandBuffer cb) {
	for (int i = 0; i < SHIM_OPEN; ++i) {
		if (g_t.open[i].cb == cb) {
			g_dev.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_t.pool, g_t.open[i].pair * 2 + 1);
			g_t.open[i].cb = VK_NULL_HANDLE;
			return;
		}
	}
}

/* ---- Helpers ------------------------------------------------------------ */

static int shim_level(void) {
	const char* v = getenv("XWA_XR_FOVEATION");
	if (!v || !v[0]) {
		return 1;
	}
	if (v[0] == '0' || strcmp(v, "off") == 0) {
		return 0;
	}
	if (strcmp(v, "low") == 0 || v[0] == '1') {
		return 1;
	}
	if (strcmp(v, "high") == 0 || v[0] == '3') {
		return 3;
	}
	if (strcmp(v, "medium") == 0 || v[0] == '2') {
		return 2;
	}
	if (strcmp(v, "test") == 0 || v[0] == '4') {
		return 4; /* deliberately extreme, to see that it works */
	}
	return 1;
}

static int pass_known(VkRenderPass p) {
	for (int i = 0; i < g_pass_count; ++i) {
		if (g_passes[i] == p) {
			return 1;
		}
	}
	return 0;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < g_dev.memory.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (g_dev.memory.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

static void map_destroy(ShimMap* m) {
	if (m->view) {
		g_dev.DestroyImageView(g_dev.device, m->view, NULL);
	}
	if (m->image) {
		g_dev.DestroyImage(g_dev.device, m->image, NULL);
	}
	if (m->memory) {
		g_dev.FreeMemory(g_dev.device, m->memory, NULL);
	}
	memset(m, 0, sizeof *m);
}

/* Creates an R8G8 density map of w x h texels with `texels` (2 bytes each),
 * uploads it and leaves it in FRAGMENT_DENSITY_MAP_OPTIMAL. Synchronous: one
 * submission on the device's queue, waited for. */
static int map_create(ShimMap* m, uint32_t w, uint32_t h, const uint8_t* texels) {
	VkResult r;
	memset(m, 0, sizeof *m);
	const VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8_UNORM,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if ((r = g_dev.CreateImage(g_dev.device, &ici, NULL, &m->image)) != VK_SUCCESS) {
		LOGW("density map image: %d", r);
		return 0;
	}
	VkMemoryRequirements req;
	g_dev.GetImageMemoryRequirements(g_dev.device, m->image, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size };
	mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX) {
		mai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
	}
	if (g_dev.AllocateMemory(g_dev.device, &mai, NULL, &m->memory) != VK_SUCCESS ||
		g_dev.BindImageMemory(g_dev.device, m->image, m->memory, 0) != VK_SUCCESS) {
		map_destroy(m);
		return 0;
	}
	const VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = m->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (g_dev.CreateImageView(g_dev.device, &vci, NULL, &m->view) != VK_SUCCESS) {
		map_destroy(m);
		return 0;
	}
	/* Staging buffer. */
	const VkDeviceSize bytes = (VkDeviceSize)w * h * 2;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	const VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	int ok = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	if (g_dev.CreateBuffer(g_dev.device, &bci, NULL, &staging) == VK_SUCCESS) {
		VkMemoryRequirements breq;
		g_dev.GetBufferMemoryRequirements(g_dev.device, staging, &breq);
		VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size };
		bmai.memoryTypeIndex = memory_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																	 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		void* ptr = NULL;
		if (bmai.memoryTypeIndex != UINT32_MAX &&
			g_dev.AllocateMemory(g_dev.device, &bmai, NULL, &staging_mem) == VK_SUCCESS &&
			g_dev.BindBufferMemory(g_dev.device, staging, staging_mem, 0) == VK_SUCCESS &&
			g_dev.MapMemory(g_dev.device, staging_mem, 0, bytes, 0, &ptr) == VK_SUCCESS) {
			memcpy(ptr, texels, (size_t)bytes);
			g_dev.UnmapMemory(g_dev.device, staging_mem);
			const VkCommandPoolCreateInfo pci = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = g_dev.queue_family,
			};
			VkCommandBuffer cb = VK_NULL_HANDLE;
			const VkCommandBufferAllocateInfo cai = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandBufferCount = 1,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			};
			VkCommandBufferAllocateInfo cai2 = cai;
			if (g_dev.CreateCommandPool(g_dev.device, &pci, NULL, &pool) == VK_SUCCESS) {
				cai2.commandPool = pool;
			}
			if (pool && g_dev.AllocateCommandBuffers(g_dev.device, &cai2, &cb) == VK_SUCCESS) {
				const VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
													  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
				g_dev.BeginCommandBuffer(cb, &bi);
				VkImageMemoryBarrier b = {
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = 0,
					.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = m->image,
					.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				};
				g_dev.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
										 NULL, 0, NULL, 1, &b);
				const VkBufferImageCopy copy = {
					.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.imageExtent = { w, h, 1 },
				};
				g_dev.CmdCopyBufferToImage(cb, staging, m->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
				b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				b.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
				g_dev.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
										 VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, NULL, 0, NULL, 1, &b);
				g_dev.EndCommandBuffer(cb);
				VkQueue queue = VK_NULL_HANDLE;
				g_dev.GetDeviceQueue(g_dev.device, g_dev.queue_family, 0, &queue);
				const VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
										  .commandBufferCount = 1,
										  .pCommandBuffers = &cb };
				ok = queue && g_dev.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
					 g_dev.QueueWaitIdle(queue) == VK_SUCCESS;
			}
		}
	}
	if (pool) {
		g_dev.DestroyCommandPool(g_dev.device, pool, NULL);
	}
	if (staging) {
		g_dev.DestroyBuffer(g_dev.device, staging, NULL);
	}
	if (staging_mem) {
		g_dev.FreeMemory(g_dev.device, staging_mem, NULL);
	}
	if (!ok) {
		LOGW("density map upload failed");
		map_destroy(m);
		return 0;
	}
	m->w = w;
	m->h = h;
	return 1;
}

/* Density (255 = every pixel, 128 = every 2nd, 64 = every 4th) of each zone
 * by level: zone 0 is the centre. Values are R = horizontal, G = vertical. */
static void level_zones(int level, float radius[3], uint8_t dens[4][2]) {
	static const float k_r[5][3] = {
		{ 9.0f, 9.0f, 9.0f },    /* off (unused) */
		{ 0.62f, 0.85f, 9.0f },  /* low */
		{ 0.52f, 0.72f, 0.92f }, /* medium */
		{ 0.42f, 0.60f, 0.80f }, /* high */
		{ 0.18f, 0.30f, 0.45f }, /* test: blocky beyond a small centre */
	};
	static const uint8_t k_d[5][4][2] = {
		{ { 255, 255 }, { 255, 255 }, { 255, 255 }, { 255, 255 } },
		{ { 255, 255 }, { 128, 255 }, { 128, 128 }, { 128, 128 } },
		{ { 255, 255 }, { 128, 255 }, { 128, 128 }, { 64, 128 } },
		{ { 255, 255 }, { 128, 128 }, { 64, 128 }, { 64, 64 } },
		{ { 255, 255 }, { 64, 64 }, { 64, 64 }, { 64, 64 } },
	};
	memcpy(radius, k_r[level], sizeof k_r[level]);
	memcpy(dens, k_d[level], sizeof k_d[level]);
}

/* The foveated map for the eye framebuffer; NULL when not available. */
static ShimMap* eye_map(uint32_t fb_w, uint32_t fb_h) {
	const char* env = getenv("OPENXWA_FOVEATION_EYE");
	int w = 0, h = 0;
	float cxl = 0, cyl = 0, cxr = 0, cyr = 0;
	if (!env || sscanf(env, "%d %d %f %f %f %f", &w, &h, &cxl, &cyl, &cxr, &cyr) != 6 || (uint32_t)w != fb_w ||
		(uint32_t)h != fb_h) {
		return NULL;
	}
	if (g_eye.view && (int)fb_w == g_eye_w && (int)fb_h == g_eye_h) {
		return &g_eye;
	}
	if (g_eye_tried && (int)fb_w == g_eye_w && (int)fb_h == g_eye_h) {
		return NULL;
	}
	const int level = shim_level();
	g_eye_tried = 1;
	g_eye_w = (int)fb_w;
	g_eye_h = (int)fb_h;
	if (level == 0) {
		return NULL;
	}
	if (g_eye.view) {
		map_destroy(&g_eye);
	}
	const uint32_t tw = (fb_w + g_dev.texel.width - 1) / g_dev.texel.width;
	const uint32_t th = (fb_h + g_dev.texel.height - 1) / g_dev.texel.height;
	uint8_t* texels = (uint8_t*)malloc((size_t)tw * th * 2);
	if (!texels) {
		return NULL;
	}
	float radius[3];
	uint8_t dens[4][2];
	level_zones(level, radius, dens);
	/* Both eyes share the framebuffer (rendered in turn), so the full-detail
	 * zone spans both optical centres. */
	const float cx = 0.5f * (cxl + cxr), half_x = 0.5f * fabsf(cxl - cxr);
	const float cy = 0.5f * (cyl + cyr), half_y = 0.5f * fabsf(cyl - cyr);
	int counts[4] = { 0, 0, 0, 0 };
	for (uint32_t j = 0; j < th; ++j) {
		for (uint32_t i = 0; i < tw; ++i) {
			const float px = ((float)i + 0.5f) * (float)g_dev.texel.width / (float)fb_w * 2.0f - 1.0f;
			const float py = ((float)j + 0.5f) * (float)g_dev.texel.height / (float)fb_h * 2.0f - 1.0f;
			const float dx = fmaxf(0.0f, fabsf(px - cx) - half_x);
			/* The lenses blur less vertically than across: a slightly taller zone. */
			const float dy = fmaxf(0.0f, fabsf(py - cy) - half_y) * 0.85f;
			const float r = sqrtf(dx * dx + dy * dy);
			int zone = 0;
			while (zone < 3 && r > radius[zone]) {
				zone++;
			}
			counts[zone]++;
			texels[(j * tw + i) * 2 + 0] = dens[zone][0];
			texels[(j * tw + i) * 2 + 1] = dens[zone][1];
		}
	}
	const int ok = map_create(&g_eye, tw, th, texels);
	free(texels);
	if (ok) {
		const float n = (float)(tw * th);
		LOGI("foveation level %d on %ux%u eye framebuffers (%ux%u map of %ux%u px texels): zones %.0f/%.0f/%.0f/%.0f %%",
			 level, fb_w, fb_h, tw, th, g_dev.texel.width, g_dev.texel.height, 100.0f * counts[0] / n,
			 100.0f * counts[1] / n, 100.0f * counts[2] / n, 100.0f * counts[3] / n);
		return &g_eye;
	}
	return NULL;
}

static ShimMap* full_map(uint32_t fb_w, uint32_t fb_h) {
	const uint32_t tw = (fb_w + g_dev.texel.width - 1) / g_dev.texel.width;
	const uint32_t th = (fb_h + g_dev.texel.height - 1) / g_dev.texel.height;
	if (g_full.view && g_full.w >= tw && g_full.h >= th) {
		return &g_full;
	}
	const uint32_t w = tw > 256 ? tw : 256, h = th > 256 ? th : 256;
	uint8_t* texels = (uint8_t*)malloc((size_t)w * h * 2);
	if (!texels) {
		return NULL;
	}
	memset(texels, 255, (size_t)w * h * 2);
	/* The old map may still be referenced by framebuffers: keep it alive. */
	ShimMap fresh;
	const int ok = map_create(&fresh, w, h, texels);
	free(texels);
	if (!ok) {
		return NULL;
	}
	g_full = fresh;
	return &g_full;
}

/* ---- Hooks -------------------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateInstance(const VkInstanceCreateInfo* ci,
														  const VkAllocationCallbacks* a, VkInstance* out) {
	PFN_vkCreateInstance real = (PFN_vkCreateInstance)g_real_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	const VkResult r = real(ci, a, out);
	if (r == VK_SUCCESS) {
		g_instance = *out;
	}
	LOGI("vkCreateInstance: %d (API %u.%u)", r, ci && ci->pApplicationInfo ? VK_API_VERSION_MAJOR(ci->pApplicationInfo->apiVersion) : 0,
		 ci && ci->pApplicationInfo ? VK_API_VERSION_MINOR(ci->pApplicationInfo->apiVersion) : 0);
	return r;
}

#define DEV_LOAD(name) g_dev.name = (PFN_vk##name)g_dev.gdpa(g_dev.device, "vk" #name)

static void dev_load(VkDevice device, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci);
typedef struct ShimAstcStorage {
	VkPhysicalDeviceFeatures features;
	VkPhysicalDeviceFeatures2 features2;
} ShimAstcStorage;
static int shim_add_astc(VkPhysicalDevice phys, VkDeviceCreateInfo* mine, const void** chain_head,
						 ShimAstcStorage* storage);
static void shim_astc_result(int enabled);
static VkResult dev_plain(PFN_vkCreateDevice real, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
						  const VkAllocationCallbacks* a, VkDevice* out);

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
														const VkAllocationCallbacks* a, VkDevice* out) {
	PFN_vkCreateDevice real = (PFN_vkCreateDevice)g_real_gipa(g_instance, "vkCreateDevice");
	LOGI("vkCreateDevice (instance %s, level %d)", g_instance ? "known" : "UNKNOWN", shim_level());
	if (!real) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (g_dev.device) {
		return real(phys, ci, a, out);
	}
	if (shim_level() == 0) {
		return dev_plain(real, phys, ci, a, out);
	}
	/* Does the device have it? */
	PFN_vkEnumerateDeviceExtensionProperties enum_ext =
		(PFN_vkEnumerateDeviceExtensionProperties)g_real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
	/* The KHR entry points first: SDL enables that instance extension, while
	 * the core 1.1 names may be unusable on a Vulkan 1.0 instance. */
	PFN_vkGetPhysicalDeviceFeatures2 features2 =
		(PFN_vkGetPhysicalDeviceFeatures2)g_real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
	PFN_vkGetPhysicalDeviceProperties2 props2 =
		(PFN_vkGetPhysicalDeviceProperties2)g_real_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
	if (!features2 || !props2) {
		LOGW("no vkGetPhysicalDevice*2KHR: foveation off");
		return dev_plain(real, phys, ci, a, out);
	}
	PFN_vkGetPhysicalDeviceMemoryProperties memprops =
		(PFN_vkGetPhysicalDeviceMemoryProperties)g_real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
	int has_ext = 0, already = 0;
	uint32_t n = 0;
	if (enum_ext && enum_ext(phys, NULL, &n, NULL) == VK_SUCCESS && n) {
		VkExtensionProperties* e = (VkExtensionProperties*)calloc(n, sizeof *e);
		if (e && enum_ext(phys, NULL, &n, e) == VK_SUCCESS) {
			for (uint32_t i = 0; i < n; ++i) {
				has_ext |= strcmp(e[i].extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0;
			}
		}
		free(e);
	}
	for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
		already |= strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0;
	}
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT feat = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT };
	VkPhysicalDeviceFragmentDensityMapPropertiesEXT prop = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT };
	if (has_ext && features2 && props2 && memprops && !already) {
		VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat };
		VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &prop };
		features2(phys, &f2);
		props2(phys, &p2);
	}
	LOGI("fragment density map: extension %d, feature %u, non-subsampled images %u, texel %ux%u..%ux%u", has_ext,
		 feat.fragmentDensityMap, feat.fragmentDensityMapNonSubsampledImages, prop.minFragmentDensityTexelSize.width,
		 prop.minFragmentDensityTexelSize.height, prop.maxFragmentDensityTexelSize.width,
		 prop.maxFragmentDensityTexelSize.height);
	if (!has_ext || already || !feat.fragmentDensityMap || !feat.fragmentDensityMapNonSubsampledImages ||
		prop.maxFragmentDensityTexelSize.width == 0) {
		LOGW("foveation unavailable on this device: rendering without it");
		return dev_plain(real, phys, ci, a, out);
	}

	/* The application's request plus the extension and its features. */
	VkDeviceCreateInfo mine = *ci;
	const char** names = (const char**)calloc(ci->enabledExtensionCount + 1, sizeof *names);
	if (!names) {
		return dev_plain(real, phys, ci, a, out);
	}
	memcpy(names, ci->ppEnabledExtensionNames, ci->enabledExtensionCount * sizeof *names);
	names[ci->enabledExtensionCount] = VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
	mine.enabledExtensionCount = ci->enabledExtensionCount + 1;
	mine.ppEnabledExtensionNames = names;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT enable = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,
		.pNext = (void*)ci->pNext,
		.fragmentDensityMap = VK_TRUE,
		.fragmentDensityMapNonSubsampledImages = VK_TRUE,
	};
	mine.pNext = &enable;
	ShimAstcStorage astc_storage;
	const int astc = shim_add_astc(phys, &mine, (const void**)&enable.pNext, &astc_storage);
	VkResult r = real(phys, &mine, a, out);
	free(names);
	if (r == VK_SUCCESS) {
		shim_astc_result(astc);
	}
	if (r != VK_SUCCESS) {
		LOGW("device with fragment density map failed (%d): rendering without it", r);
		return dev_plain(real, phys, ci, a, out);
	}

	dev_load(*out, phys, ci);
	/* Map texel: the GPU derives it from the framebuffer / map size ratio
	 * (rounded up to a power of two, clamped to [min, max]), so a map built
	 * for 32 px texels is read at 32 px - fine enough for smooth zones (the
	 * Quest reports 16..256; the maximum gave a 6x7 map of 256 px blocks). */
	g_dev.texel.width = 32;
	g_dev.texel.height = 32;
	if (g_dev.texel.width < prop.minFragmentDensityTexelSize.width) {
		g_dev.texel.width = prop.minFragmentDensityTexelSize.width;
	}
	if (g_dev.texel.height < prop.minFragmentDensityTexelSize.height) {
		g_dev.texel.height = prop.minFragmentDensityTexelSize.height;
	}
	if (g_dev.texel.width > prop.maxFragmentDensityTexelSize.width) {
		g_dev.texel.width = prop.maxFragmentDensityTexelSize.width;
	}
	if (g_dev.texel.height > prop.maxFragmentDensityTexelSize.height) {
		g_dev.texel.height = prop.maxFragmentDensityTexelSize.height;
	}
	memprops(phys, &g_dev.memory);
	g_dev.active = g_dev.CreateRenderPass && g_dev.CreateFramebuffer && g_dev.CreateImage && g_dev.QueueSubmit &&
				   g_dev.CmdCopyBufferToImage && g_dev.GetDeviceQueue;
	LOGI("foveation %s (map texel %ux%u)", g_dev.active ? "ready" : "unavailable", g_dev.texel.width,
		 g_dev.texel.height);
	return VK_SUCCESS;
}

/* The device's entry points we call or hook, and the GPU timing. */
static void dev_load(VkDevice device, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci) {
	memset(&g_dev, 0, sizeof g_dev);
	g_dev.device = device;
	g_dev.physical = phys;
	g_dev.queue_family = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
	g_dev.gdpa = (PFN_vkGetDeviceProcAddr)g_real_gipa(g_instance, "vkGetDeviceProcAddr");
	DEV_LOAD(CreateRenderPass);
	DEV_LOAD(DestroyRenderPass);
	DEV_LOAD(CreateFramebuffer);
	DEV_LOAD(DestroyDevice);
	DEV_LOAD(CreateImage);
	DEV_LOAD(DestroyImage);
	DEV_LOAD(GetImageMemoryRequirements);
	DEV_LOAD(AllocateMemory);
	DEV_LOAD(FreeMemory);
	DEV_LOAD(BindImageMemory);
	DEV_LOAD(CreateImageView);
	DEV_LOAD(DestroyImageView);
	DEV_LOAD(CreateBuffer);
	DEV_LOAD(DestroyBuffer);
	DEV_LOAD(GetBufferMemoryRequirements);
	DEV_LOAD(BindBufferMemory);
	DEV_LOAD(MapMemory);
	DEV_LOAD(UnmapMemory);
	DEV_LOAD(CreateCommandPool);
	DEV_LOAD(DestroyCommandPool);
	DEV_LOAD(AllocateCommandBuffers);
	DEV_LOAD(BeginCommandBuffer);
	DEV_LOAD(EndCommandBuffer);
	DEV_LOAD(CmdPipelineBarrier);
	DEV_LOAD(CmdCopyBufferToImage);
	DEV_LOAD(GetDeviceQueue);
	DEV_LOAD(QueueSubmit);
	DEV_LOAD(QueueWaitIdle);
	DEV_LOAD(DestroyFramebuffer);
	DEV_LOAD(CmdBeginRenderPass);
	DEV_LOAD(CmdEndRenderPass);
	DEV_LOAD(CmdWriteTimestamp);
	DEV_LOAD(CmdResetQueryPool);
	DEV_LOAD(CreateQueryPool);
	DEV_LOAD(DestroyQueryPool);
	DEV_LOAD(GetQueryPoolResults);
	t_setup(phys);
}

/* A device without foveation (still timed). */
static VkResult dev_plain(PFN_vkCreateDevice real, VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
						  const VkAllocationCallbacks* a, VkDevice* out) {
	VkDeviceCreateInfo mine = *ci;
	ShimAstcStorage astc_storage;
	const int astc = shim_add_astc(phys, &mine, &mine.pNext, &astc_storage);
	VkResult r = real(phys, &mine, a, out);
	if (r != VK_SUCCESS && astc) {
		r = real(phys, ci, a, out);
		shim_astc_result(0);
	} else if (r == VK_SUCCESS) {
		shim_astc_result(astc);
	}
	if (r == VK_SUCCESS) {
		dev_load(*out, phys, ci);
	}
	return r;
}

/* ASTC textures need the textureCompressionASTC_LDR feature, which SDL
 * does not request: added to the device's features when the GPU has it
 * (in pEnabledFeatures, or the VkPhysicalDeviceFeatures2 in the chain). */
static int shim_add_astc(VkPhysicalDevice phys, VkDeviceCreateInfo* mine, const void** chain_head,
						 ShimAstcStorage* storage) {
	PFN_vkGetPhysicalDeviceFeatures features =
		(PFN_vkGetPhysicalDeviceFeatures)g_real_gipa(g_instance, "vkGetPhysicalDeviceFeatures");
	VkPhysicalDeviceFeatures have;
	if (!features) {
		return 0;
	}
	memset(&have, 0, sizeof have);
	features(phys, &have);
	if (!have.textureCompressionASTC_LDR) {
		return 0;
	}
	if (mine->pEnabledFeatures) {
		storage->features = *mine->pEnabledFeatures;
		storage->features.textureCompressionASTC_LDR = VK_TRUE;
		mine->pEnabledFeatures = &storage->features;
		return 1;
	}
	/* Features in a VkPhysicalDeviceFeatures2: a copy of it replaces the
	 * original at the head of the chain we own (the caller's structures are
	 * left as they are). Deeper in the chain: no ASTC. */
	const VkBaseInStructure* head = (const VkBaseInStructure*)*chain_head;
	if (head && head->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
		storage->features2 = *(const VkPhysicalDeviceFeatures2*)head;
		storage->features2.features.textureCompressionASTC_LDR = VK_TRUE;
		*chain_head = &storage->features2;
		return 1;
	}
	for (const VkBaseInStructure* n = head; n; n = n->pNext) {
		if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
			return 0;
		}
	}
	memset(&storage->features, 0, sizeof storage->features);
	storage->features.textureCompressionASTC_LDR = VK_TRUE;
	mine->pEnabledFeatures = &storage->features;
	return 1;
}

/* Tells the game whether ASTC textures may be used (AERON_TEXTURE_ASTC). */
/* Set once a device was created through the shim (some runtimes create it
 * with the system loader's vkCreateDevice instead: no foveation, no ASTC). */
static volatile int g_device_seen;

static void shim_astc_result(int enabled) {
	g_device_seen = 1;
	LOGI("ASTC texture compression %s", enabled ? "enabled" : "unavailable");
	if (!enabled) {
		setenv("AERON_TEXTURE_ASTC", "0", 1);
	}
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyDevice(VkDevice device, const VkAllocationCallbacks* a) {
	if (device == g_dev.device && g_dev.DestroyDevice) {
		PFN_vkDestroyDevice real = g_dev.DestroyDevice;
		pthread_mutex_lock(&g_lock);
		map_destroy(&g_eye);
		map_destroy(&g_full);
		if (g_t.pool && g_dev.DestroyQueryPool) {
			g_dev.DestroyQueryPool(device, g_t.pool, NULL);
		}
		memset(&g_t, 0, sizeof g_t);
		g_rp_used = g_fb_used = 0;
		g_pass_count = 0;
		g_eye_tried = 0;
		g_eye_fb_count = 0;
		memset(&g_dev, 0, sizeof g_dev);
		pthread_mutex_unlock(&g_lock);
		real(device, a);
		return;
	}
	PFN_vkDestroyDevice real = (PFN_vkDestroyDevice)g_real_gipa(g_instance, "vkDestroyDevice");
	real(device, a);
}

static VkResult shim_CreateRenderPassFdm(VkDevice device, const VkRenderPassCreateInfo* ci,
										 const VkAllocationCallbacks* a, VkRenderPass* out);

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* ci,
															const VkAllocationCallbacks* a, VkRenderPass* out) {
	const VkResult r = shim_CreateRenderPassFdm(device, ci, a, out);
	if (r == VK_SUCCESS && device == g_dev.device && g_t.enabled && ci->attachmentCount > 0) {
		ShimPassDesc d;
		memset(&d, 0, sizeof d);
		d.pass = *out;
		d.attachments = ci->attachmentCount;
		d.depth_only = 1;
		/* The first colour attachment describes the pass. */
		for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
			const VkFormat f = ci->pAttachments[i].format;
			if (!fmt_is_depth(f)) {
				d.format = f;
				d.samples = ci->pAttachments[i].samples;
				d.depth_only = 0;
				break;
			}
			d.format = f;
			d.samples = ci->pAttachments[i].samples;
		}
		pthread_mutex_lock(&g_lock);
		rp_put(&d);
		pthread_mutex_unlock(&g_lock);
	}
	return r;
}

static int fmt_is_depth(VkFormat f) {
	return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
		   f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT ||
		   f == VK_FORMAT_S8_UINT;
}

/* 1 for the passes that draw the 3D scene: multisampled, or (MSAA off)
 * colour plus depth. */
static int pass_is_scene(const VkRenderPassCreateInfo* ci) {
	int msaa = 0, color = 0, depth = 0;
	for (uint32_t i = 0; i < ci->attachmentCount; ++i) {
		msaa |= ci->pAttachments[i].samples != VK_SAMPLE_COUNT_1_BIT;
		if (fmt_is_depth(ci->pAttachments[i].format)) {
			depth = 1;
		} else {
			color = 1;
		}
	}
	if (msaa) {
		return 1;
	}
	static int single = -1;
	if (single < 0) {
		const char* v = getenv("XWA_XR_FOVEATION_1X");
		single = !(v && (v[0] == '0' || strcmp(v, "off") == 0));
	}
	/* One subpass only: the density map then covers exactly what it draws. */
	return single && color && depth && ci->subpassCount == 1;
}

static VkResult shim_CreateRenderPassFdm(VkDevice device, const VkRenderPassCreateInfo* ci,
										 const VkAllocationCallbacks* a, VkRenderPass* out) {
	if (device != g_dev.device || !g_dev.active || !pass_is_scene(ci) || g_pass_count >= SHIM_MAX_PASSES) {
		PFN_vkCreateRenderPass real = device == g_dev.device && g_dev.CreateRenderPass
										  ? g_dev.CreateRenderPass
										  : (PFN_vkCreateRenderPass)g_real_gipa(g_instance, "vkCreateRenderPass");
		return real(device, ci, a, out);
	}
	VkAttachmentDescription att[16];
	if (ci->attachmentCount >= 16) {
		return g_dev.CreateRenderPass(device, ci, a, out);
	}
	memcpy(att, ci->pAttachments, ci->attachmentCount * sizeof *att);
	att[ci->attachmentCount] = (VkAttachmentDescription) {
		.format = VK_FORMAT_R8G8_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
		.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
	};
	VkRenderPassFragmentDensityMapCreateInfoEXT fdm = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT,
		.pNext = ci->pNext,
		.fragmentDensityMapAttachment = { ci->attachmentCount, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT },
	};
	VkRenderPassCreateInfo mine = *ci;
	mine.pNext = &fdm;
	mine.attachmentCount = ci->attachmentCount + 1;
	mine.pAttachments = att;
	const VkResult r = g_dev.CreateRenderPass(device, &mine, a, out);
	if (r != VK_SUCCESS) {
		LOGW("render pass with density map failed (%d): plain pass", r);
		return g_dev.CreateRenderPass(device, ci, a, out);
	}
	pthread_mutex_lock(&g_lock);
	g_passes[g_pass_count++] = *out;
	pthread_mutex_unlock(&g_lock);
	static int logged;
	if (logged < 4) {
		logged++;
		LOGI("scene render pass %d (%s): density map attachment added (%u attachments)", logged,
			 ci->pAttachments[0].samples != VK_SAMPLE_COUNT_1_BIT ? "multisampled" : "single-sample",
			 mine.attachmentCount);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyRenderPass(VkDevice device, VkRenderPass pass,
														 const VkAllocationCallbacks* a) {
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_pass_count; ++i) {
		if (g_passes[i] == pass) {
			g_passes[i] = g_passes[--g_pass_count];
			break;
		}
	}
	if (pass != VK_NULL_HANDLE && rp_get(pass)) {
		SHIM_TABLE_DEL(g_t.rp, SHIM_RP_TABLE, pass, pass);
		g_rp_used--;
	}
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyRenderPass real = device == g_dev.device && g_dev.DestroyRenderPass
									   ? g_dev.DestroyRenderPass
									   : (PFN_vkDestroyRenderPass)g_real_gipa(g_instance, "vkDestroyRenderPass");
	real(device, pass, a);
}

static VkResult shim_CreateFramebufferFdm(VkDevice device, const VkFramebufferCreateInfo* ci,
										  const VkAllocationCallbacks* a, VkFramebuffer* out);

static VKAPI_ATTR VkResult VKAPI_CALL shim_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* ci,
															 const VkAllocationCallbacks* a, VkFramebuffer* out) {
	const VkResult r = shim_CreateFramebufferFdm(device, ci, a, out);
	if (r == VK_SUCCESS && device == g_dev.device && g_t.enabled) {
		const ShimFbDesc d = { *out, ci->renderPass, ci->width, ci->height };
		pthread_mutex_lock(&g_lock);
		fb_put(&d);
		pthread_mutex_unlock(&g_lock);
	}
	return r;
}

static VkResult shim_CreateFramebufferFdm(VkDevice device, const VkFramebufferCreateInfo* ci,
										  const VkAllocationCallbacks* a, VkFramebuffer* out) {
	pthread_mutex_lock(&g_lock);
	const int ours = device == g_dev.device && g_dev.active && pass_known(ci->renderPass);
	ShimMap* map = NULL;
	if (ours) {
		map = eye_map(ci->width, ci->height);
		if (!map) {
			map = full_map(ci->width, ci->height);
		} else if (!g_logged_eye) {
			g_logged_eye = 1;
		}
	}
	pthread_mutex_unlock(&g_lock);
	if (!ours) {
		PFN_vkCreateFramebuffer real = device == g_dev.device && g_dev.CreateFramebuffer
										   ? g_dev.CreateFramebuffer
										   : (PFN_vkCreateFramebuffer)g_real_gipa(g_instance, "vkCreateFramebuffer");
		return real(device, ci, a, out);
	}
	if (!map || ci->attachmentCount >= 16) {
		LOGW("no density map for a %ux%u framebuffer", ci->width, ci->height);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkImageView views[16];
	memcpy(views, ci->pAttachments, ci->attachmentCount * sizeof *views);
	views[ci->attachmentCount] = map->view;
	VkFramebufferCreateInfo mine = *ci;
	mine.attachmentCount = ci->attachmentCount + 1;
	mine.pAttachments = views;
	static int logged;
	if (logged < 6) {
		logged++;
		LOGI("framebuffer %ux%u for a scene pass: %s density map", ci->width, ci->height,
			 map == &g_eye ? "foveated" : "full");
	}
	const VkResult r = g_dev.CreateFramebuffer(device, &mine, a, out);
	if (r == VK_SUCCESS && map == &g_eye) {
		pthread_mutex_lock(&g_lock);
		if (g_eye_fb_count < SHIM_MAX_EYE_FBS) {
			g_eye_fbs[g_eye_fb_count++] = *out;
		}
		pthread_mutex_unlock(&g_lock);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL shim_DestroyFramebuffer(VkDevice device, VkFramebuffer fb,
														  const VkAllocationCallbacks* a) {
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_eye_fb_count; ++i) {
		if (g_eye_fbs[i] == fb) {
			g_eye_fbs[i] = g_eye_fbs[--g_eye_fb_count];
			break;
		}
	}
	if (fb != VK_NULL_HANDLE && fb_get(fb)) {
		SHIM_TABLE_DEL(g_t.fb, SHIM_FB_TABLE, fb, fb);
		g_fb_used--;
	}
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyFramebuffer real = device == g_dev.device && g_dev.DestroyFramebuffer
										? g_dev.DestroyFramebuffer
										: (PFN_vkDestroyFramebuffer)g_real_gipa(g_instance, "vkDestroyFramebuffer");
	real(device, fb, a);
}

static double shim_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info,
														  VkSubpassContents contents) {
	int eye = 0;
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_eye_fb_count; ++i) {
		if (g_eye_fbs[i] == info->framebuffer) {
			eye = 1;
			break;
		}
	}
	if (eye) {
		g_eye_begins++;
	} else {
		g_other_begins++;
	}
	const double now = shim_now();
	if (g_count_since == 0.0) {
		g_count_since = now;
	} else if (now - g_count_since >= 10.0) {
		LOGI("last %.0f s: %u render passes on the foveated eye framebuffers, %u other passes", now - g_count_since,
			 g_eye_begins, g_other_begins);
		g_eye_begins = g_other_begins = 0;
		g_count_since = now;
	}
	if (g_t.enabled) {
		if (now - g_t.last_harvest > 0.05) {
			t_harvest(now);
			t_report(now);
			g_t.last_harvest = now;
		}
		t_begin(cb, info, eye);
	}
	pthread_mutex_unlock(&g_lock);
	g_dev.CmdBeginRenderPass(cb, info, contents);
}

static VKAPI_ATTR void VKAPI_CALL shim_CmdEndRenderPass(VkCommandBuffer cb) {
	g_dev.CmdEndRenderPass(cb);
	if (g_t.enabled) {
		pthread_mutex_lock(&g_lock);
		t_end(cb);
		pthread_mutex_unlock(&g_lock);
	}
}

/* ---- Dispatch ----------------------------------------------------------- */

static PFN_vkVoidFunction shim_device_hook(const char* name) {
	if (strcmp(name, "vkCreateRenderPass") == 0) {
		return (PFN_vkVoidFunction)shim_CreateRenderPass;
	}
	if (strcmp(name, "vkDestroyRenderPass") == 0) {
		return (PFN_vkVoidFunction)shim_DestroyRenderPass;
	}
	if (strcmp(name, "vkCreateFramebuffer") == 0) {
		return (PFN_vkVoidFunction)shim_CreateFramebuffer;
	}
	if (strcmp(name, "vkDestroyFramebuffer") == 0) {
		return (PFN_vkVoidFunction)shim_DestroyFramebuffer;
	}
	if (strcmp(name, "vkCmdBeginRenderPass") == 0 && g_dev.CmdBeginRenderPass) {
		return (PFN_vkVoidFunction)shim_CmdBeginRenderPass;
	}
	if (strcmp(name, "vkCmdEndRenderPass") == 0 && g_dev.CmdEndRenderPass && g_t.enabled) {
		return (PFN_vkVoidFunction)shim_CmdEndRenderPass;
	}
	if (strcmp(name, "vkDestroyDevice") == 0) {
		return (PFN_vkVoidFunction)shim_DestroyDevice;
	}
	return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char* name);

static int shim_load(void) {
	if (g_real_gipa) {
		return 1;
	}
	g_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!g_lib) {
		LOGW("libvulkan.so: %s", dlerror());
		return 0;
	}
	g_real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
	LOGI("loaded in front of libvulkan.so (%s)", g_real_gipa ? "ok" : "no vkGetInstanceProcAddr");
	return g_real_gipa != NULL;
}

SHIM_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
	if (!shim_load() || !name) {
		return NULL;
	}
	if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
		return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
	}
	if (strcmp(name, "vkCreateInstance") == 0) {
		return (PFN_vkVoidFunction)shim_CreateInstance;
	}
	if (instance && g_instance == VK_NULL_HANDLE) {
		g_instance = instance;
	}
	if (strcmp(name, "vkCreateDevice") == 0) {
		return (PFN_vkVoidFunction)shim_CreateDevice;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	}
	PFN_vkVoidFunction hook = shim_device_hook(name);
	if (hook) {
		return hook;
	}
	return g_real_gipa(instance, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL shim_GetDeviceProcAddr(VkDevice device, const char* name) {
	if (!name) {
		return NULL;
	}
	if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
		return (PFN_vkVoidFunction)shim_GetDeviceProcAddr;
	}
	PFN_vkVoidFunction hook = shim_device_hook(name);
	if (hook) {
		return hook;
	}
	PFN_vkGetDeviceProcAddr real = (PFN_vkGetDeviceProcAddr)g_real_gipa(g_instance, "vkGetDeviceProcAddr");
	return real ? real(device, name) : NULL;
}

/* Global entry points some loaders look up directly. */
SHIM_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
																				   VkExtensionProperties* props) {
	if (!shim_load()) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	PFN_vkEnumerateInstanceExtensionProperties real =
		(PFN_vkEnumerateInstanceExtensionProperties)g_real_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
	return real ? real(layer, count, props) : VK_ERROR_INITIALIZATION_FAILED;
}

SHIM_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* ci,
															const VkAllocationCallbacks* a, VkInstance* out) {
	if (!shim_load()) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return shim_CreateInstance(ci, a, out);
}

/* For the game: one call per frame it renders (the timing's per-frame figures). */
SHIM_EXPORT void OpenXWA_VkShim_FrameMark(void) {
	pthread_mutex_lock(&g_lock);
	g_t.frames++;
	pthread_mutex_unlock(&g_lock);
}

/* For the game: 1 when the eye framebuffers are being foveated. */
SHIM_EXPORT int OpenXWA_VkShim_Foveating(void) { return g_eye.view != VK_NULL_HANDLE; }

/* For the game: 1 once the Vulkan device was created through the shim. */
SHIM_EXPORT int OpenXWA_VkShim_DeviceSeen(void) { return g_device_seen; }
