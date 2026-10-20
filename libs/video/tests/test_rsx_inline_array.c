/*
 * ps3recomp - NV4097_INLINE_ARRAY test
 *
 * Vertices pushed through the FIFO (INLINE_ARRAY words between BEGIN_END(prim)
 * and BEGIN_END(0)) must reach the backend as one draw_arrays(prim, 0, n),
 * with rsx_fetch_attrib reading each attribute out of the packed stream:
 * enabled attributes in attribute order, big-endian. Sly 2 resolves its AA
 * buffer onto the display this way, as a fullscreen QUAD_STRIP.
 *
 * Build (standalone):
 *   cc -std=c11 -I include -I libs/video libs/video/tests/test_rsx_inline_array.c \
 *      libs/video/rsx_commands.c libs/video/rsx_vertex_fetch.c -o /tmp/test_rsx_inline_array
 */
#include "rsx_commands.h"
#include "rsx_vertex_fetch.h"

#include <stdio.h>
#include <string.h>

/* rsx_commands.c reaches the guest VM (semaphore labels) and the boot
 * milestone log, rsx_vertex_fetch.c the offset resolvers; an inline draw
 * touches none of them, so stubs are enough to link. */
uint32_t ppu_hle_inject_base;
u8* vm_base;
void vm_write32(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void ps3_ms(const char* key) { (void)key; }
u32 cellGcmResolveOffset(u32 offset) { return offset; }
u32 cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }

static int g_fail;
static int g_draws;
static u32 g_prim, g_first, g_count;
static float g_pos[8][4], g_col[8][4];
static rsx_state g_st;

static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail = 1;
}

static u32 fbits(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

static void on_draw_arrays(void* ud, u32 prim, u32 first, u32 count)
{
    (void)ud;
    g_draws++;
    g_prim = prim; g_first = first; g_count = count;
    for (u32 v = 0; v < count && v < 8; v++) {
        rsx_fetch_attrib(&g_st, 0, v, g_pos[v]);
        rsx_fetch_attrib(&g_st, 3, v, g_col[v]);
    }
}

int main(void)
{
    rsx_backend be;
    memset(&be, 0, sizeof be);
    be.draw_arrays = on_draw_arrays;
    rsx_set_backend(&be);
    rsx_state_init(&g_st);

    /* attr0: float32 x2 (8 bytes), attr3: ubyte x4 (4 bytes) -> stride 12.
     * Format word: type [3:0], size [7:4], stride [15:8]; stride left at 0
     * as titles do for inline streams. */
    rsx_process_method(&g_st, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, 0x22);
    rsx_process_method(&g_st, NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x44);

    static const float xy[4][2] = { {-1, 1}, {1, 1}, {-1, -1}, {1, -1} };
    rsx_process_method(&g_st, NV4097_SET_BEGIN_END, 9);   /* QUAD_STRIP */
    for (int v = 0; v < 4; v++) {
        rsx_process_method(&g_st, NV4097_INLINE_ARRAY, fbits(xy[v][0]));
        rsx_process_method(&g_st, NV4097_INLINE_ARRAY, fbits(xy[v][1]));
        rsx_process_method(&g_st, NV4097_INLINE_ARRAY, 0xFF000000u | (u32)(v * 0x40)); /* R=255 .. A */
    }
    check(g_draws == 0, "nothing drawn before BEGIN_END(0)");
    rsx_process_method(&g_st, NV4097_SET_BEGIN_END, 0);

    check(g_draws == 1, "one draw_arrays at BEGIN_END(0)");
    check(g_prim == 9 && g_first == 0 && g_count == 4, "prim 9, first 0, 4 vertices");
    int pos_ok = 1;
    for (int v = 0; v < 4; v++)
        if (g_pos[v][0] != xy[v][0] || g_pos[v][1] != xy[v][1]) pos_ok = 0;
    check(pos_ok, "attr0 positions read from the stream");
    check(g_pos[2][2] == 0.0f && g_pos[2][3] == 1.0f,
          "components attr0 does not supply keep the constant register (z=0, w=1)");
    check(g_col[0][0] == 1.0f && g_col[3][3] > 0.75f && g_col[3][3] < 0.76f,
          "attr3 ubyte colour read at offset 8 (R of v0, A of v3)");
    check(g_st.inline_vtx == NULL, "inline stream unhooked after the draw");

    /* A group with no INLINE_ARRAY words draws nothing extra. */
    rsx_process_method(&g_st, NV4097_SET_BEGIN_END, 5);
    rsx_process_method(&g_st, NV4097_SET_BEGIN_END, 0);
    check(g_draws == 1, "an empty BEGIN/END group issues no draw");

    printf("test_rsx_inline_array: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
