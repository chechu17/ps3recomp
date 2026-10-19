/*
 * Standalone unit test for the RSX vertex-program decompiler: the boolean
 * "branch bit" flow control (BRB / CLB).
 *
 * Build (no D3D12 dependency), from this directory:
 *   cc -std=c11 -Wall -Wextra -I../../../include \
 *       test_vp_decompiler.c ../rsx_vp_decompiler.c -o test_vp
 * Under MSVC the generated HLSL is also compiled with D3DCompile (vs_5_0):
 *   cl /nologo /I..\..\..\include test_vp_decompiler.c ..\rsx_vp_decompiler.c
 *
 * BRB jumps and CLB calls when one of the 32 bits the guest sets with
 * SET_TRANSFORM_BRANCH_BITS matches: D3[23:27] picks the bit, D3[28] says
 * whether the jump happens on a set (1) or a clear (0) bit, and the D0 CC
 * cond gates it like any other branch. The decompiler reads the bits from
 * vp_branch_bits in VPConst. The programs here are hand-assembled.
 */
#include "../rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

static int g_pass = 0, g_fail = 0;

static void check(const char* test, const char* hlsl, const char* needle)
{
    if (strstr(hlsl, needle)) { g_pass++; return; }
    g_fail++;
    printf("FAIL [%s]: missing \"%s\"\n", test, needle);
}

static void check_absent(const char* test, const char* hlsl, const char* needle)
{
    if (!strstr(hlsl, needle)) { g_pass++; return; }
    g_fail++;
    printf("FAIL [%s]: unexpected \"%s\"\n", test, needle);
}

static void compiles(const char* test, const char* hlsl)
{
#ifdef _MSC_VER
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), test, NULL, NULL, "main", "vs_5_0",
                            0, 0, &code, &err);
    if (SUCCEEDED(hr)) g_pass++;
    else {
        g_fail++;
        printf("FAIL [%s]: D3DCompile: %s\n", test,
               err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
    }
    if (code) code->lpVtbl->Release(code);
    if (err) err->lpVtbl->Release(err);
#else
    (void)test; (void)hlsl;
#endif
}

/* ---- tiny assembler (little-endian words, as the decompiler reads) ------- */
static void put_le(unsigned char* p, u32 w)
{
    p[0] = (unsigned char)w; p[1] = (unsigned char)(w >> 8);
    p[2] = (unsigned char)(w >> 16); p[3] = (unsigned char)(w >> 24);
}

static void put_instr(unsigned char* prog, int slot, u32 d0, u32 d1, u32 d2, u32 d3)
{
    unsigned char* p = prog + slot * 16;
    put_le(p, d0); put_le(p + 4, d1); put_le(p + 8, d2); put_le(p + 12, d3);
}

/* No temp / output destination anywhere. */
#define D0_NO_TMP   (0x3Fu << 15)
#define D3_NO_DST   ((0x1Fu << 2) | (0x3Fu << 7))
#define END         1u

/* Scalar flow op to slot `target`, CC cond `cond`; for BRB/CLB, branch bit
 * `bit` and polarity `on_set`. */
static void put_flow(unsigned char* prog, int slot, u32 sca_op, u32 target, u32 cond,
                     u32 bit, u32 on_set, u32 end)
{
    const u32 d0 = D0_NO_TMP | (cond << 10) | (((target >> 9) & 1u) << 23);
    const u32 d1 = sca_op << 27;
    const u32 d2 = (target >> 3) & 0x3Fu;
    const u32 d3 = D3_NO_DST | (bit << 23) | (on_set << 28) | ((target & 7u) << 29) | end;
    put_instr(prog, slot, d0, d1, d2, d3);
}

/* MOV o[dst].xyzw = src (reg_type 2 = v[idx], 3 = vp_c[idx]), swizzle xyzw. */
static void put_mov_out(unsigned char* prog, int slot, u32 dst, u32 reg_type, u32 idx, u32 end)
{
    const u32 src = 0x1B00u | reg_type;                    /* .xyzw */
    const u32 d0 = D0_NO_TMP | (7u << 10) | (1u << 30);    /* vec_result */
    u32 d1 = (1u << 22) | (src >> 9);                      /* MOV */
    if (reg_type == 3) d1 |= idx << 12; else d1 |= (idx & 15u) << 8;
    const u32 d2 = (src & 0x1FFu) << 23;
    const u32 d3 = (dst << 2) | (0x3Fu << 7) | (0xFu << 13) | end;
    put_instr(prog, slot, d0, d1, d2, d3);
}

#define OP_BRI 0x09u
#define OP_RET 0x0Cu
#define OP_BRB 0x11u
#define OP_CLB 0x12u

int main(void)
{
    static char hlsl[256 * 1024];
    unsigned char prog[16 * 16];

    /* The bits sit right after vp_posoffset; every backend fills them there. */
    if (RSX_VP_CB_BRANCH_BITS_OFFSET == 514u * 16u && RSX_VP_CB_BYTES == 8240u) g_pass++;
    else { g_fail++; printf("FAIL [layout]: branch bits offset/size\n"); }

    /* Test 1: the shape every BRB on the Sly discs has -- cond=7, polarity
     * 0 ("skip this block unless bit N is set") -- plus a CLB on a set bit
     * into a subroutine past the main RET.
     *   0: BRB !b5 -> 2
     *   1: MOV o[1] = c[0]
     *   2: CLB  b2 -> 5
     *   3: MOV o[0] = v[0]
     *   4: RET              (empty call stack: program ends)
     *   5: MOV o[2] = c[1]
     *   6: RET, end */
    memset(prog, 0, sizeof prog);
    put_flow(prog, 0, OP_BRB, 2, 7, 5, 0, 0);
    put_mov_out(prog, 1, 1, 3, 0, 0);
    put_flow(prog, 2, OP_CLB, 5, 7, 2, 1, 0);
    put_mov_out(prog, 3, 0, 2, 0, 0);
    put_flow(prog, 4, OP_RET, 0, 7, 0, 0, 0);
    put_mov_out(prog, 5, 2, 3, 1, 0);
    put_flow(prog, 6, OP_RET, 0, 7, 0, 0, END);
    rsx_vp_set_branch_base(0);
    int n = rsx_vp_decompile(prog, sizeof prog, hlsl, sizeof hlsl);
    if (n == 7) g_pass++;
    else { g_fail++; printf("FAIL [brb/clb]: %d instructions, want 7\n", n); }
    check("brb/clb", hlsl, "uint vp_branch_bits;");
    check("brb/clb", hlsl, "_pc = ((vp_branch_bits & 32u) == 0u) ? 2 : 1;");
    check("brb/clb", hlsl,
          "if (((vp_branch_bits & 4u) != 0u)) { _cs[_csp] = 3; _csp = min(_csp + 1, 7); _pc = 5; } else _pc = 3;");
    check("brb/clb", hlsl, "if (_pc == 5) {");
    check_absent("brb/clb", hlsl, "TODO");
    compiles("brb/clb", hlsl);

    /* Test 2: a CC cond still gates the jump, ANDed with the bit test.
     *   0: BRB b31 -> 2 if cc0.x == 0
     *   1: MOV o[1] = c[0]
     *   2: MOV o[0] = v[0], end */
    memset(prog, 0, sizeof prog);
    put_flow(prog, 0, OP_BRB, 2, 2, 31, 1, 0);
    put_mov_out(prog, 1, 1, 3, 0, 0);
    put_mov_out(prog, 2, 0, 2, 0, END);
    n = rsx_vp_decompile(prog, sizeof prog, hlsl, sizeof hlsl);
    check("brb cc", hlsl,
          "_pc = ((cc[0].x == 0.0) && ((vp_branch_bits & 2147483648u) != 0u)) ? 2 : 1;");
    check_absent("brb cc", hlsl, "TODO");
    compiles("brb cc", hlsl);

    /* Test 3: the branch base still applies (targets are absolute slots).
     * Program loaded at slot 100; BRB !b0 -> 102. */
    memset(prog, 0, sizeof prog);
    put_flow(prog, 0, OP_BRB, 102, 7, 0, 0, 0);
    put_mov_out(prog, 1, 1, 3, 0, 0);
    put_mov_out(prog, 2, 0, 2, 0, END);
    rsx_vp_set_branch_base(100);
    n = rsx_vp_decompile(prog, sizeof prog, hlsl, sizeof hlsl);
    rsx_vp_set_branch_base(0);
    check("brb base", hlsl, "_pc = ((vp_branch_bits & 1u) == 0u) ? 2 : 1;");
    compiles("brb base", hlsl);

    /* Test 4: BRI is unchanged by all this. */
    memset(prog, 0, sizeof prog);
    put_flow(prog, 0, OP_BRI, 2, 7, 0, 0, 0);
    put_mov_out(prog, 1, 1, 3, 0, 0);
    put_mov_out(prog, 2, 0, 2, 0, END);
    n = rsx_vp_decompile(prog, sizeof prog, hlsl, sizeof hlsl);
    check("bri", hlsl, "_pc = true ? 2 : 1;");
    check_absent("bri", hlsl, "vp_branch_bits &");
    compiles("bri", hlsl);

    printf("test_vp_decompiler: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
