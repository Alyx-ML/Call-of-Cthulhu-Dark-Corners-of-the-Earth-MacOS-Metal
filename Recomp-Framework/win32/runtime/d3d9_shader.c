// Direct3D 9 shader bytecode (vs 1.1, ps 1.1-1.4, vs/ps 2.0, 2.x, 3.0) -> Metal Shading Language.
//
// The output is a body function the device wraps per draw configuration (d3d9_metal.m):
//   vertex: static void vs_<hash>(thread float4 *v, thread VSRegs &o, constant VSConsts &k, <vertex textures>)
//   pixel:  static void ps_<hash>(thread PSRegs &in, thread PSORegs &o, constant PSConsts &k, <textures>)
// Registers become locals; constants read the constant buffer (k.c / k.i / k.b) unless the shader defines them
// (def), in which case a local holds the value. Varyings use D3D semantic names, see the VSOut struct in the
// common prelude (d3d9_msl_prelude), so any vertex shader links with any pixel shader.
//
// Shader model 1 differs from 2+: instruction tokens carry no length (the parameter count is fixed per opcode),
// relative addressing has no address token (always a0.x), pixel shaders read texture registers t# filled by texture
// instructions (1.1-1.3) or sample with texld (1.4), have destination shifts (_x2 _x4 _d2...), co-issued pairs, and
// write their color in r0.
#include "d3d9_internal.h"
#include "d3d9_prelude.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_TEMP = 0, T_INPUT = 1, T_CONST = 2, T_ADDR = 3, T_TEXTURE = 3, T_RASTOUT = 4, T_ATTROUT = 5, T_OUTPUT = 6,
       T_TEXCRDOUT = 6, T_CONSTINT = 7, T_COLOROUT = 8, T_DEPTHOUT = 9, T_SAMPLER = 10, T_CONSTBOOL = 14, T_LOOP = 15,
       T_MISC = 17, T_LABEL = 18, T_PREDICATE = 19 };
enum { U_POSITION = 0, U_BLENDWEIGHT, U_BLENDINDICES, U_NORMAL, U_PSIZE, U_TEXCOORD, U_TANGENT, U_BINORMAL,
       U_TESSFACTOR, U_POSITIONT, U_COLOR, U_FOG, U_DEPTH, U_SAMPLE };
#define OP_PHASE 0xFFFDu
#define OP_COMMENT 0xFFFEu
#define OP_END 0xFFFFu

// ---- growable string (failure is sticky: the translation reports it) ----
typedef struct { char *s; size_t n, cap; int oom; } Buf;
static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    if (b->oom) return;
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); b->oom = 1; return; }
    size_t need = b->n + (size_t)n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { va_end(ap2); b->oom = 1; return; }
            cap *= 2;
        }
        char *p = realloc(b->s, cap);
        if (!p) { va_end(ap2); b->oom = 1; return; }
        b->s = p; b->cap = cap;
    }
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->n += (size_t)n;
}

typedef struct {
    const uint32_t *t; size_t n, pc;
    int ps, major, minor;
    const ShaderVariant *var;
    ShaderInfo *info;
    Buf body;
    int indent;
    // defined constants
    uint8_t def_f[256], def_i[16], def_b[16];
    float def_fv[256][4]; int32_t def_iv[16][4]; uint32_t def_bv[16];
    // declarations
    struct { int used, usage, index; } vout[16], pin[16];
    int temps, uses_a0, uses_p0, oDepth, oColor_mask;
    uint32_t t_used;            // ps 1.1-1.3: texture registers t# (locals)
    int err;
    // co-issue: the first instruction of a pair writes a copy, flushed after the second
    int co_defer, co_pending; char co_dst[160], co_mask[5], co_var[16];
    // ps 1.x texm3x2/texm3x3 rows computed by the pad instructions
    char m3[2][32]; int m3n;
    int tmp;
} Tr;

static void fail(Tr *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Tr *x, const char *fmt, ...) {
    if (x->err) return;
    va_list ap; va_start(ap, fmt); vsnprintf(x->info->error, sizeof x->info->error, fmt, ap); va_end(ap);
    x->err = 1;
}
// bounded formatting: a truncated expression fails the translation instead of producing wrong code
static void sf(Tr *x, char *out, size_t on, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void sf(Tr *x, char *out, size_t on, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, on, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= on) { fail(x, "expression too long"); if (on) out[0] = 0; }
}
// the next token; reading past the end fails the translation and returns the end token
static uint32_t tok(Tr *x) {
    if (x->pc >= x->n) { fail(x, "truncated shader"); return OP_END; }
    return x->t[x->pc++];
}

static int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int reg_num(uint32_t t) { return (int)(t & 0x7FF); }
static int sm1(const Tr *x) { return x->major < 2; }

// parameter tokens of a shader model 1 instruction (no length field); -1: not a shader model 1 opcode
static int sm1_params(int ps, int minor, uint32_t op) {
    switch (op) {
    case 0: return 0;
    case 1: return 2;
    case 2: case 3: return 3;
    case 4: return 4;
    case 5: return 3;
    case 6: case 7: return 2;
    case 8: case 9: case 10: case 11: case 12: case 13: return 3;
    case 14: case 15: case 16: return 2;
    case 17: return 3;
    case 18: return 4;
    case 19: return 2;
    case 20: case 21: case 22: case 23: case 24: return 3;
    case 31: return 2;
    case 64: return ps && minor >= 4 ? 2 : 1;   // texcoord t# / texcrd r#, t#
    case 65: return 1;
    case 66: return ps && minor >= 4 ? 2 : 1;   // tex t# / texld r#, src
    case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 74: return 2;
    case 76: return 3;
    case 77: case 78: case 79: return 2;
    case 80: return 4;
    case 81: return 5;
    case 82: case 83: case 84: case 85: case 86: return 2;
    case 87: return 1;
    case 88: return 4;
    case 89: return 3;
    case OP_PHASE: return 0;
    default: return -1;
    }
}
// tokens after the instruction token
static int ins_len(int ps, int major, int minor, uint32_t ins) {
    uint32_t op = ins & 0xFFFF;
    if (op == OP_COMMENT) return (int)((ins >> 16) & 0x7FFF);
    if (major >= 2) return (int)((ins >> 24) & 0xF);
    return sm1_params(ps, minor, op);
}

// semantic name of a varying, shared with the device's VSOut
static const char *semantic(int usage, int index, char *buf, size_t bn) {
    switch (usage) {
    case U_TEXCOORD: if (index < 10) { snprintf(buf, bn, "TEXCOORD%d", index); return buf; } break;
    case U_COLOR: if (index < 2) { snprintf(buf, bn, "COLOR%d", index); return buf; } break;
    case U_FOG: return "FOG0";
    case U_NORMAL: return "NORMAL0";
    case U_TANGENT: return "TANGENT0";
    case U_BINORMAL: return "BINORMAL0";
    default: break;
    }
    return "TEXCOORD9";   // extra positions and other semantics share the spare varying
}

static void swz_str(uint32_t swz, char *b) {
    static const char c[4] = {'x', 'y', 'z', 'w'};
    for (int i = 0; i < 4; i++) b[i] = c[(swz >> (2 * i)) & 3];
    b[4] = 0;
}

// sampler type of stage / sampler s
static int stype(Tr *x, int s) {
    int t = x->info->sampler_type[s & 15];
    if (!t && x->var) t = x->var->sampler_type[s & 15];
    return t ? t : 2;
}
// a texture sample expression of sampler s at coordinate expression `c` (a float4)
static void sample(Tr *x, int s, const char *c, char *out, size_t on) {
    if (s < 0 || s > 15) { fail(x, "sampler %d", s); return; }
    int tt = stype(x, s);
    x->info->samplers |= 1u << s;
    x->info->sampler_type[s] = (uint8_t)tt;
    const char *crd = tt == 2 ? "xy" : "xyz";
    if (x->ps) sf(x, out, on, "tex%d.sample(smp%d, (%s).%s, bias(k.lod_bias[%d][%d]))", s, s, c, crd, s >> 2, s & 3);
    else sf(x, out, on, "tex%d.sample(smp%d, (%s).%s, level(0.0))", s, s, c, crd);
}

// base register expression (a float4 lvalue or rvalue), without swizzle or modifiers
static void reg_expr(Tr *x, uint32_t t, uint32_t rel, int has_rel, char *out, size_t on) {
    int type = reg_type(t), n = reg_num(t);
    char relx[64] = "";
    if (has_rel) {
        char sw[5]; swz_str((rel >> 16) & 0xFF, sw);
        if (!sm1(x) && reg_type(rel) == T_LOOP) snprintf(relx, sizeof relx, "aL");
        else { snprintf(relx, sizeof relx, "a0.%c", sm1(x) ? 'x' : sw[0]); x->uses_a0 = 1; }
    }
    switch (type) {
    case T_TEMP:
        if (n >= 32) { fail(x, "temporary register r%d", n); break; }
        sf(x, out, on, "r%d", n); if (n + 1 > x->temps) x->temps = n + 1;
        return;
    case T_INPUT:
        if (n >= 16) { fail(x, "input register v%d", n); break; }
        if (x->ps) {
            if (x->major < 3) {   // colors, clamped to [0, 1] as the rasterizer delivers them
                if (n >= 2) { fail(x, "color input v%d", n); break; }
                sf(x, out, on, "saturate(in.COLOR%d)", n);
            } else {
                char sb[32];
                sf(x, out, on, "in.%s", x->pin[n].used ? semantic(x->pin[n].usage, x->pin[n].index, sb, sizeof sb) : "TEXCOORD9");
            }
        } else if (has_rel) sf(x, out, on, "v[clamp(%s + %d, 0, 15)]", relx, n);
        else sf(x, out, on, "v[%d]", n);
        return;
    case T_CONST: {
        int lim = x->ps ? PS_CONST_F : VS_CONST_F;
        if (has_rel) {
            sf(x, out, on, "d3d_crel(k.c, %s + %d, %d)", relx, n, lim);
            x->info->max_const_f = lim;
            return;
        }
        if (n >= lim) { fail(x, "constant c%d", n); break; }
        if (x->def_f[n]) sf(x, out, on, "d%d", n);
        else { sf(x, out, on, "k.c[%d]", n); if (n + 1 > x->info->max_const_f) x->info->max_const_f = n + 1; }
        return;
    }
    case T_ADDR:   // vs: a0; ps: t# (1.1-1.3 texture registers, 1.4 / 2.x texture coordinates)
        if (x->ps) {
            if (n >= 10) { fail(x, "texture register t%d", n); break; }
            if (sm1(x) && x->minor < 4) { sf(x, out, on, "t%d", n); x->t_used |= 1u << n; }
            else sf(x, out, on, "in.TEXCOORD%d", n);
        } else { sf(x, out, on, "float4(a0)"); x->uses_a0 = 1; }
        return;
    case T_RASTOUT: sf(x, out, on, "%s", n == 0 ? "o.position" : n == 1 ? "o.FOG0" : "o.psize4"); if (n == 1) x->info->writes_fog = 1; if (n == 2) x->info->writes_psize = 1; return;
    case T_ATTROUT: if (n >= 2) { fail(x, "color output oD%d", n); break; } sf(x, out, on, "o.COLOR%d", n); return;
    case T_OUTPUT:
        if (n >= 16) { fail(x, "output register o%d", n); break; }
        if (x->major >= 3) {
            char sb[32];
            if (!x->vout[n].used) sf(x, out, on, "o.TEXCOORD9");
            else if (x->vout[n].usage == U_POSITION && x->vout[n].index == 0) sf(x, out, on, "o.position");
            else if (x->vout[n].usage == U_PSIZE) { sf(x, out, on, "o.psize4"); x->info->writes_psize = 1; }
            else {
                if (x->vout[n].usage == U_FOG) x->info->writes_fog = 1;
                sf(x, out, on, "o.%s", semantic(x->vout[n].usage, x->vout[n].index, sb, sizeof sb));
            }
        } else {
            if (n >= 10) { fail(x, "texture coordinate output oT%d", n); break; }
            sf(x, out, on, "o.TEXCOORD%d", n);
        }
        return;
    case T_CONSTINT:
        if (n >= 16) { fail(x, "integer constant i%d", n); break; }
        if (x->def_i[n]) sf(x, out, on, "float4(di%d)", n); else sf(x, out, on, "float4(k.i[%d])", n);
        return;
    case T_CONSTBOOL:
        if (n >= 16) { fail(x, "boolean constant b%d", n); break; }
        if (x->def_b[n]) sf(x, out, on, "float4(db%d)", n); else sf(x, out, on, "float4(k.b[%d] != 0)", n);
        return;
    case T_COLOROUT: if (n >= 4) { fail(x, "color output oC%d", n); break; } sf(x, out, on, "o.color%d", n); x->oColor_mask |= 1 << n; return;
    case T_DEPTHOUT: sf(x, out, on, "o.depth4"); x->oDepth = 1; return;
    case T_LOOP: sf(x, out, on, "float4(aL)"); return;
    case T_MISC: sf(x, out, on, "%s", n == 0 ? "in.vpos" : "float4(in.face ? 1.0 : -1.0)"); return;
    case T_PREDICATE: sf(x, out, on, "float4(p0)"); x->uses_p0 = 1; return;
    case T_SAMPLER: sf(x, out, on, "s%d", n); return;
    default: fail(x, "register type %d", type); break;
    }
    sf(x, out, on, "float4(0.0)");
}

// source operand with swizzle and modifier; `reg_add` offsets the register number (matrix rows)
static void src_from(Tr *x, uint32_t t, uint32_t rel, int reg_add, char *out, size_t on) {
    int has_rel = (t >> 13) & 1;
    uint32_t t2 = (t & ~0x7FFu) | ((t + (uint32_t)reg_add) & 0x7FFu);
    char base[200], sw[5];
    reg_expr(x, t2, rel, has_rel, base, sizeof base);
    swz_str((t >> 16) & 0xFF, sw);
    char v[240];
    if (!strcmp(sw, "xyzw")) sf(x, v, sizeof v, "%s", base); else sf(x, v, sizeof v, "(%s).%s", base, sw);
    switch ((t >> 24) & 0xF) {
    case 0: sf(x, out, on, "%s", v); break;
    case 1: sf(x, out, on, "(-%s)", v); break;
    case 2: sf(x, out, on, "(%s - 0.5)", v); break;
    case 3: sf(x, out, on, "(0.5 - %s)", v); break;
    case 4: sf(x, out, on, "(2.0 * %s - 1.0)", v); break;
    case 5: sf(x, out, on, "(1.0 - 2.0 * %s)", v); break;
    case 6: sf(x, out, on, "(1.0 - %s)", v); break;
    case 7: sf(x, out, on, "(2.0 * %s)", v); break;
    case 8: sf(x, out, on, "(-2.0 * %s)", v); break;
    case 9: sf(x, out, on, "float4((%s).xy / (%s).z, (%s).zw)", v, v, v); break;    // _dz (ps_1_4)
    case 10: sf(x, out, on, "float4((%s).xy / (%s).w, (%s).zw)", v, v, v); break;   // _dw (ps_1_4)
    case 11: sf(x, out, on, "abs(%s)", v); break;
    case 12: sf(x, out, on, "(-abs(%s))", v); break;
    case 13: sf(x, out, on, "float4(!bool4(%s))", v); break;
    default: sf(x, out, on, "%s", v); fail(x, "source modifier %u", (t >> 24) & 0xF); break;
    }
}
static void src_n(Tr *x, int reg_add, char *out, size_t on) {
    uint32_t t = tok(x), rel = 0;
    if (((t >> 13) & 1) && !sm1(x)) rel = tok(x);
    src_from(x, t, rel, reg_add, out, on);
}
static void src(Tr *x, char *out, size_t on) { src_n(x, 0, out, on); }
// the register number of the next (source) token, without consuming it
static int peek_reg(Tr *x) { return x->pc < x->n ? reg_num(x->t[x->pc]) : 0; }

typedef struct { char name[160]; char mask[5]; int sat, nmask, reg, type; float scale; int addr; } Dst;
static void dst(Tr *x, Dst *d) {
    uint32_t t = tok(x), rel = 0;
    int has_rel = (t >> 13) & 1;
    if (has_rel && !sm1(x)) rel = tok(x);
    d->type = reg_type(t); d->reg = reg_num(t);
    d->addr = !x->ps && d->type == T_ADDR;
    if (d->addr) { sf(x, d->name, sizeof d->name, "a0"); x->uses_a0 = 1; }
    else reg_expr(x, t, rel, has_rel, d->name, sizeof d->name);
    int wm = (t >> 16) & 0xF, k = 0;
    if (!wm) wm = 0xF;
    for (int i = 0; i < 4; i++) if (wm & (1 << i)) d->mask[k++] = "xyzw"[i];
    d->mask[k] = 0; d->nmask = k;
    d->sat = (t >> 20) & 1;
    // result shift (ps 1.x): _x2 _x4 _x8 _d2 _d4 _d8
    static const float shift[16] = {1, 2, 4, 8, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.125f, 0.25f, 0.5f};
    int sh = (int)((t >> 24) & 0xF);
    d->scale = shift[sh];
    if (sh >= 4 && sh <= 12) fail(x, "destination shift %d", sh);
}

static void line(Tr *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(Tr *x, const char *fmt, ...) {
    char tmp[4096]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { fail(x, "statement too long"); return; }
    bput(&x->body, "%*s%s\n", 4 + 4 * x->indent, "", tmp);
}

// predication: the predicate token comes after the destination and before the sources (SM 2.x / 3)
static int pred_tok(Tr *x, uint32_t ins, char *p, size_t pn) {
    if (sm1(x) || !((ins >> 28) & 1)) return 0;
    uint32_t t = tok(x);
    char sw[5]; swz_str((t >> 16) & 0xFF, sw);
    int neg = ((t >> 24) & 0xF) == 13;
    sf(x, p, pn, "%sp0.%s", neg ? "!" : "", sw);
    x->uses_p0 = 1;
    return 1;
}

// write `expr` (a float4 expression) to the destination, honouring shift, saturate, mask, predication and co-issue
static void assign(Tr *x, Dst *d, const char *expr, const char *pred) {
    char ev[3000], es[3100];
    if (d->scale != 1.0f) sf(x, ev, sizeof ev, "(float4(%s) * %.9g)", expr, (double)d->scale);
    else sf(x, ev, sizeof ev, "float4(%s)", expr);
    const char *e = ev;
    if (d->sat) { sf(x, es, sizeof es, "saturate(%s)", ev); e = es; }
    if (x->err) return;
    if (d->addr) {   // mov / mova a0: round to the nearest integer
        line(x, "{ int4 t_ = int4(floor(%s + 0.5)); a0.%s = t_.%s; }", e, d->mask, d->mask);
        return;
    }
    if (pred && *pred) {
        line(x, "{ float4 t_ = %s; bool4 pr_ = bool4(%s); %s.%s = select(%s.%s, t_.%s, pr_.%s); }",
             e, pred, d->name, d->mask, d->name, d->mask, d->mask, d->mask);
        return;
    }
    if (x->co_defer) {   // first of a co-issued pair: the second must read the registers before this write
        snprintf(x->co_var, sizeof x->co_var, "co%d_", x->tmp++);
        snprintf(x->co_dst, sizeof x->co_dst, "%s", d->name);
        snprintf(x->co_mask, sizeof x->co_mask, "%s", d->mask);
        line(x, "float4 %s = (%s);", x->co_var, e);
        x->co_pending = 1;
        return;
    }
    if (d->nmask == 4) line(x, "%s = %s;", d->name, e);
    else line(x, "%s.%s = (%s).%s;", d->name, d->mask, e, d->mask);
}

static const char *cmp_op(int c) {
    switch (c) { case 1: return ">"; case 2: return "=="; case 3: return ">="; case 4: return "<"; case 5: return "!="; case 6: return "<="; default: return "=="; }
}

static void decl(Tr *x) {
    uint32_t u = tok(x), d = tok(x);
    int type = reg_type(d), n = reg_num(d);
    int usage = (int)(u & 0x1F), index = (int)((u >> 16) & 0xF);
    if (n >= 16) { fail(x, "declared register %d", n); return; }
    if (type == T_SAMPLER) {
        int tt = (int)((u >> 27) & 0xF);
        x->info->samplers |= 1u << n;
        x->info->sampler_type[n] = (uint8_t)(tt ? tt : 2);
    } else if (type == T_INPUT) {
        if (x->ps) { x->pin[n].used = 1; x->pin[n].usage = usage; x->pin[n].index = index; }
        else { x->info->in[n].used = 1; x->info->in[n].usage = (uint8_t)usage; x->info->in[n].index = (uint8_t)index; }
    } else if (type == T_OUTPUT && !x->ps) {
        x->vout[n].used = 1; x->vout[n].usage = usage; x->vout[n].index = index;
        x->info->out[n].used = 1; x->info->out[n].usage = (uint8_t)usage; x->info->out[n].index = (uint8_t)index;
        x->info->out[n].mask = (uint8_t)((d >> 16) & 0xF);
    }
}

// first pass: declarations and constant definitions, needed to name registers before the code uses them
static void prepass(Tr *x) {
    size_t pc = 1;
    while (pc < x->n && !x->err) {
        uint32_t ins = x->t[pc], op = ins & 0xFFFF;
        if (op == OP_END) break;
        int len = ins_len(x->ps, x->major, x->minor, ins);
        if (len < 0) { fail(x, "opcode %u", op); return; }
        if (pc + 1 + (size_t)len > x->n) { fail(x, "truncated shader"); return; }
        if (op != OP_COMMENT) {
            if (op == 31) { x->pc = pc + 1; decl(x); }
            else if (op == 81) {   // def c#, f, f, f, f
                int r = reg_num(x->t[pc + 1]);
                if (r < 256) { x->def_f[r] = 1; memcpy(x->def_fv[r], &x->t[pc + 2], 16); }
            } else if (op == 48) { int r = reg_num(x->t[pc + 1]); if (r < 16) { x->def_i[r] = 1; memcpy(x->def_iv[r], &x->t[pc + 2], 16); } }
            else if (op == 47) { int r = reg_num(x->t[pc + 1]); if (r < 16) { x->def_b[r] = 1; x->def_bv[r] = x->t[pc + 2]; } }
        }
        pc += 1 + (size_t)len;
    }
}

// ---- shader model 1 texture instructions (pixel shaders) ----
// `stage` is the destination register number; `c` is a float4 coordinate expression
static void tex_write(Tr *x, Dst *d, int stage, const char *c) {
    char e[1200];
    sample(x, stage, c, e, sizeof e);
    assign(x, d, e, NULL);
}
// texture coordinate set n of the interpolated inputs
static void tc(Tr *x, int n, char *out, size_t on) {
    if (n < 0 || n > 9) { fail(x, "texture coordinate %d", n); return; }
    sf(x, out, on, "in.TEXCOORD%d", n);
}

static void emit_sm1_tex(Tr *x, uint32_t op) {
    Dst d; char a[600], b[600], c[1200], e[2000];
    switch (op) {
    case 64:   // texcoord t# (1.1-1.3: clamped coordinate as a color) / texcrd r#, t# (1.4)
        dst(x, &d);
        if (x->minor >= 4) { src(x, a, sizeof a); assign(x, &d, a, NULL); }
        else { tc(x, d.reg, a, sizeof a); sf(x, e, sizeof e, "float4(saturate((%s).xyz), 1.0)", a); assign(x, &d, e, NULL); }
        break;
    case 66:   // tex t# (1.1-1.3) / texld r#, src (1.4)
        dst(x, &d);
        if (x->minor >= 4) { src(x, a, sizeof a); tex_write(x, &d, d.reg, a); break; }
        tc(x, d.reg, a, sizeof a);
        if (d.reg < 8 && x->var && x->var->projected[d.reg]) {   // D3DTTFF_PROJECTED
            int k = x->var->projected[d.reg];
            if (k < 1 || k > 4) { fail(x, "projection component %d", k); break; }
            sf(x, c, sizeof c, "float4((%s).xyz / (%s).%c, 1.0)", a, a, "xyzw"[k - 1]);
            tex_write(x, &d, d.reg, c);
        } else tex_write(x, &d, d.reg, a);
        break;
    case 67: case 68: {   // texbem / texbeml t(m), t(n): bump-mapped coordinates
        dst(x, &d); src(x, b, sizeof b);
        int m = d.reg;
        if (m >= 8) { fail(x, "texbem stage %d", m); break; }
        tc(x, m, a, sizeof a);
        sf(x, c, sizeof c, "float4((%s).x + k.bumpenv[%d].x * (%s).x + k.bumpenv[%d].z * (%s).y, "
                           "(%s).y + k.bumpenv[%d].y * (%s).x + k.bumpenv[%d].w * (%s).y, 0.0, 1.0)",
           a, m, b, m, b, a, m, b, m, b);
        char s[1600]; sample(x, m, c, s, sizeof s);
        if (op == 68) sf(x, e, sizeof e, "(%s) * saturate((%s).z * k.bumplum[%d].x + k.bumplum[%d].y)", s, b, m, m);
        else sf(x, e, sizeof e, "%s", s);
        assign(x, &d, e, NULL);
        break;
    }
    case 69: case 70: case 82:   // texreg2ar / texreg2gb / texreg2rgb t(m), t(n)
        dst(x, &d); src(x, b, sizeof b);
        sf(x, c, sizeof c, op == 69 ? "float4((%s).w, (%s).x, 0.0, 1.0)" : op == 70 ? "float4((%s).y, (%s).z, 0.0, 1.0)" : "float4((%s).xyz, 1.0)%.0s", b, b);
        tex_write(x, &d, d.reg, c);
        break;
    case 71: case 73: {   // texm3x2pad / texm3x3pad t(m), t(n): one row of the matrix
        dst(x, &d); src(x, b, sizeof b);
        if (x->m3n >= (op == 71 ? 1 : 2)) { fail(x, "texm3x pad sequence"); break; }
        tc(x, d.reg, a, sizeof a);
        snprintf(x->m3[x->m3n], sizeof x->m3[0], "m3_%d_", x->tmp++);
        line(x, "float %s = dot((%s).xyz, (%s).xyz);", x->m3[x->m3n], a, b);
        x->m3n++;
        break;
    }
    case 72: case 84:   // texm3x2tex / texm3x2depth t(m), t(n)
        dst(x, &d); src(x, b, sizeof b);
        if (x->m3n != 1) { fail(x, "texm3x2 without its pad"); break; }
        tc(x, d.reg, a, sizeof a);
        if (op == 72) { sf(x, c, sizeof c, "float4(%s, dot((%s).xyz, (%s).xyz), 0.0, 1.0)", x->m3[0], a, b); tex_write(x, &d, d.reg, c); }
        else { line(x, "{ float w_ = dot((%s).xyz, (%s).xyz); o.depth4 = float4(w_ == 0.0 ? 1.0 : saturate(%s / w_)); }", a, b, x->m3[0]); x->oDepth = 1; }
        x->m3n = 0;
        break;
    case 74: case 86: case 76: case 77: {   // texm3x3tex / texm3x3 / texm3x3spec / texm3x3vspec t(m), t(n) [, c#]
        dst(x, &d); src(x, b, sizeof b);
        char cs[600] = "";
        if (op == 76) src(x, cs, sizeof cs);
        if (x->m3n != 2) { fail(x, "texm3x3 without its pads"); break; }
        tc(x, d.reg, a, sizeof a);
        char nv[400]; sf(x, nv, sizeof nv, "float3(%s, %s, dot((%s).xyz, (%s).xyz))", x->m3[0], x->m3[1], a, b);
        if (op == 86) { sf(x, e, sizeof e, "float4(%s, 1.0)", nv); assign(x, &d, e, NULL); }
        else if (op == 74) { sf(x, c, sizeof c, "float4(%s, 1.0)", nv); tex_write(x, &d, d.reg, c); }
        else {
            char ev[600];
            if (op == 76) sf(x, ev, sizeof ev, "(%s).xyz", cs);
            else {   // eye vector from the w of the three coordinate sets
                char t0[64], t1[64];
                tc(x, d.reg - 2, t0, sizeof t0); tc(x, d.reg - 1, t1, sizeof t1);
                sf(x, ev, sizeof ev, "float3((%s).w, (%s).w, (%s).w)", t0, t1, a);
            }
            int id = x->tmp++;
            line(x, "float3 n%d_ = %s; float3 e%d_ = %s;", id, nv, id, ev);
            sf(x, c, sizeof c, "float4(2.0 * dot(n%d_, e%d_) / dot(n%d_, n%d_) * n%d_ - e%d_, 1.0)", id, id, id, id, id, id);
            tex_write(x, &d, d.reg, c);
        }
        x->m3n = 0;
        break;
    }
    case 83: case 85:   // texdp3tex / texdp3 t(m), t(n)
        dst(x, &d); src(x, b, sizeof b);
        tc(x, d.reg, a, sizeof a);
        if (op == 83) { sf(x, c, sizeof c, "float4(dot((%s).xyz, (%s).xyz), 0.0, 0.0, 1.0)", a, b); tex_write(x, &d, d.reg, c); }
        else { sf(x, e, sizeof e, "float4(dot((%s).xyz, (%s).xyz))", a, b); assign(x, &d, e, NULL); }
        break;
    case 87:   // texdepth r5 (1.4): depth = r5.r / r5.g
        dst(x, &d);
        line(x, "o.depth4 = float4(%s.y == 0.0 ? 1.0 : saturate(%s.x / %s.y));", d.name, d.name, d.name);
        x->oDepth = 1;
        break;
    case 89: {   // bem r(d).xy, src0, src1 (1.4 phase 1)
        dst(x, &d); src(x, a, sizeof a); src(x, b, sizeof b);
        int m = d.reg;
        if (m >= 8) { fail(x, "bem stage %d", m); break; }
        sf(x, e, sizeof e, "float4((%s).x + k.bumpenv[%d].x * (%s).x + k.bumpenv[%d].z * (%s).y, "
                           "(%s).y + k.bumpenv[%d].y * (%s).x + k.bumpenv[%d].w * (%s).y, 0.0, 0.0)",
           a, m, b, m, b, a, m, b, m, b);
        assign(x, &d, e, NULL);
        break;
    }
    case 65: {   // texkill: shader model 1 tests the texture coordinates (1.4: or a temporary), xyz
        uint32_t t = tok(x);
        int type = reg_type(t), n = reg_num(t);
        if (type == T_TEXTURE) { tc(x, n, a, sizeof a); line(x, "if (any((%s).xyz < 0.0)) discard_fragment();", a); }
        else if (type == T_TEMP && n < 32) { if (n + 1 > x->temps) x->temps = n + 1; line(x, "if (any(r%d.xyz < 0.0)) discard_fragment();", n); }
        else fail(x, "texkill register type %d", type);
        break;
    }
    default: fail(x, "ps_1_x opcode %u", op); break;
    }
}

static void emit(Tr *x) {
    x->pc = 1;
    while (x->pc < x->n && !x->err) {
        uint32_t ins = x->t[x->pc];
        uint32_t op = ins & 0xFFFF;
        if (op == OP_END) break;
        int len = ins_len(x->ps, x->major, x->minor, ins);
        if (len < 0) { fail(x, "opcode %u", op); break; }
        size_t next = x->pc + 1 + (size_t)len;
        if (next > x->n) { fail(x, "truncated shader"); break; }
        if (op == OP_COMMENT) { x->pc = next; continue; }
        // co-issue (ps 1.x): this instruction's write waits until the next (co-issued) one has read its sources
        x->co_defer = sm1(x) && x->ps && next < x->n && ((x->t[next] >> 30) & 1);
        if (x->co_defer && x->co_pending) { fail(x, "co-issued instruction followed by another co-issue"); break; }
        uint32_t ctl = (ins >> 16) & 0xFF;
        x->pc++;
        Dst d; char a[600], b[600], c[600], p[64] = "", e[2400];
        int is_tex1 = x->ps && sm1(x) && ((op >= 64 && op <= 87) || op == 89);
        if (is_tex1 && op != 78 && op != 79 && op != 80 && op != 81) { emit_sm1_tex(x, op); }
        else switch (op) {
        case 0: case OP_PHASE: break;
        case 31: case 81: case 48: case 47: x->pc = next; break;   // dcl / def*: handled in the prepass
        case 1:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); assign(x, &d, a, p); break;
        case 2:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "%s + %s", a, b); assign(x, &d, e, p); break;
        case 3:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "%s - %s", a, b); assign(x, &d, e, p); break;
        case 4:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); sf(x, e, sizeof e, "fma(%s, %s, %s)", a, b, c); assign(x, &d, e, p); break;
        case 5:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "%s * %s", a, b); assign(x, &d, e, p); break;
        case 6:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "d3d_rcp((%s).x)", a); assign(x, &d, e, p); break;
        case 7:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "d3d_rsq((%s).x)", a); assign(x, &d, e, p); break;
        case 8:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "dot((%s).xyz, (%s).xyz)", a, b); assign(x, &d, e, p); break;
        case 9:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "dot(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 10: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "min(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 11: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "max(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 12: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "float4(%s < %s)", a, b); assign(x, &d, e, p); break;
        case 13: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "float4(%s >= %s)", a, b); assign(x, &d, e, p); break;
        case 14: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "exp2((%s).x)", a); assign(x, &d, e, p); break;
        case 78: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a);
                 if (sm1(x)) sf(x, e, sizeof e, "d3d_expp1((%s).x)", a); else sf(x, e, sizeof e, "exp2((%s).x)", a);
                 assign(x, &d, e, p); break;
        case 15: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "d3d_log((%s).x)", a); assign(x, &d, e, p); break;
        case 79: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a);
                 if (sm1(x)) sf(x, e, sizeof e, "d3d_logp1((%s).x)", a); else sf(x, e, sizeof e, "d3d_log((%s).x)", a);
                 assign(x, &d, e, p); break;
        case 16: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "d3d_lit(%s)", a); assign(x, &d, e, p); break;
        case 17: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); assign(x, &d, e, p); break;
        case 18: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); sf(x, e, sizeof e, "mix(%s, %s, %s)", c, b, a); assign(x, &d, e, p); break;
        case 19: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "fract(%s)", a); assign(x, &d, e, p); break;
        case 20: case 21: case 22: case 23: case 24: {   // m4x4 m4x3 m3x4 m3x3 m3x2: rows are consecutive registers
            dst(x, &d); pred_tok(x, ins, p, sizeof p);
            src(x, a, sizeof a);
            int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2;
            int dim = (op == 20 || op == 21) ? 4 : 3;
            if (x->pc >= x->n) { fail(x, "truncated shader"); break; }
            uint32_t mtok = x->t[x->pc], mrel = (!sm1(x) && ((mtok >> 13) & 1) && x->pc + 1 < x->n) ? x->t[x->pc + 1] : 0;
            char comp[4][600];
            for (int r = 0; r < 4; r++) {
                char row[500];
                if (r < rows) {
                    src_from(x, mtok, mrel, r, row, sizeof row);
                    sf(x, comp[r], sizeof comp[r], dim == 4 ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", a, row);
                } else sf(x, comp[r], sizeof comp[r], "0.0");
            }
            x->pc += 1 + ((!sm1(x) && ((mtok >> 13) & 1)) ? 1 : 0);   // the matrix operand (and its address token)
            sf(x, e, sizeof e, "float4(%s, %s, %s, %s)", comp[0], comp[1], comp[2], comp[3]);
            assign(x, &d, e, p);
            break;
        }
        case 32: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "d3d_pow((%s).x, (%s).x)", a, b); assign(x, &d, e, p); break;
        case 33: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); sf(x, e, sizeof e, "float4(cross((%s).xyz, (%s).xyz), 0.0)", a, b); assign(x, &d, e, p); break;
        case 34: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); if (x->major < 3) { src(x, b, sizeof b); src(x, c, sizeof c); } sf(x, e, sizeof e, "sign(%s)", a); assign(x, &d, e, p); break;
        case 35: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "abs(%s)", a); assign(x, &d, e, p); break;
        case 36: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "d3d_nrm(%s)", a); assign(x, &d, e, p); break;
        case 37: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); if (x->major < 3) { src(x, b, sizeof b); src(x, c, sizeof c); }
                 sf(x, e, sizeof e, "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", a, a); assign(x, &d, e, p); break;
        case 46: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); x->uses_a0 = 1;
                 line(x, "{ int4 t_ = int4(floor(%s + 0.5)); a0.%s = t_.%s; }", a, d.mask, d.mask); break;
        case 88: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); sf(x, e, sizeof e, "select(%s, %s, %s >= 0.0)", c, b, a); assign(x, &d, e, p); break;
        case 80: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); sf(x, e, sizeof e, "select(%s, %s, %s > 0.5)", c, b, a); assign(x, &d, e, p); break;
        case 90: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); sf(x, e, sizeof e, "dot((%s).xy, (%s).xy) + (%s).x", a, b, c); assign(x, &d, e, p); break;
        case 91: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "dfdx(%s)", a); assign(x, &d, e, p); break;
        case 92: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); sf(x, e, sizeof e, "dfdy(%s)", a); assign(x, &d, e, p); break;
        case 65: {   // texkill (2.0+): the operand is encoded as a destination; any masked component < 0 discards
            Dst k; dst(x, &k);
            line(x, "if (any((%s).%s < 0.0)) discard_fragment();", k.name, k.mask);
            break;
        }
        case 66: case 93: case 95: {   // texld / texldp / texldb, texldd, texldl
            dst(x, &d); pred_tok(x, ins, p, sizeof p);
            src(x, a, sizeof a);
            int s = peek_reg(x);
            src(x, b, sizeof b);
            char g1[600] = "", g2[600] = "";
            if (op == 93) { src(x, g1, sizeof g1); src(x, g2, sizeof g2); }
            if (s > 15) { fail(x, "sampler s%d", s); break; }
            int tt = stype(x, s);
            x->info->samplers |= 1u << s;
            x->info->sampler_type[s] = (uint8_t)tt;
            const char *crd = tt == 2 ? "xy" : "xyz";
            char coord[700];
            if (op == 66 && ctl == 1) sf(x, coord, sizeof coord, "((%s).%s / (%s).w)", a, crd, a);
            else sf(x, coord, sizeof coord, "(%s).%s", a, crd);
            if (op == 95) sf(x, e, sizeof e, "tex%d.sample(smp%d, %s, level((%s).w))", s, s, coord, a);
            else if (!x->ps) sf(x, e, sizeof e, "tex%d.sample(smp%d, %s, level(0.0))", s, s, coord);
            else if (op == 66 && ctl == 2) sf(x, e, sizeof e, "tex%d.sample(smp%d, %s, bias((%s).w + k.lod_bias[%d][%d]))", s, s, coord, a, s >> 2, s & 3);
            else if (op == 93) sf(x, e, sizeof e, tt == 2 ? "tex%d.sample(smp%d, %s, gradient2d((%s).xy, (%s).xy))" : tt == 3 ? "tex%d.sample(smp%d, %s, gradientcube((%s).xyz, (%s).xyz))" : "tex%d.sample(smp%d, %s, gradient3d((%s).xyz, (%s).xyz))", s, s, coord, g1, g2);
            else sf(x, e, sizeof e, "tex%d.sample(smp%d, %s, bias(k.lod_bias[%d][%d]))", s, s, coord, s >> 2, s & 3);
            assign(x, &d, e, p);
            break;
        }
        case 94: {   // setp_comp dst, src0, src1
            dst(x, &d); src(x, a, sizeof a); src(x, b, sizeof b); x->uses_p0 = 1;
            line(x, "{ bool4 t_ = (%s %s %s); p0.%s = t_.%s; }", a, cmp_op((int)ctl), b, d.mask, d.mask);
            break;
        }
        // flow control
        case 40: src(x, a, sizeof a); line(x, "if (bool((%s).x)) {", a); x->indent++; break;
        case 41: src(x, a, sizeof a); src(x, b, sizeof b); line(x, "if ((%s).x %s (%s).x) {", a, cmp_op((int)ctl), b); x->indent++; break;
        case 42: if (x->indent < 1) { fail(x, "else outside if"); break; } x->indent--; line(x, "} else {"); x->indent++; break;
        case 43: if (x->indent < 1) { fail(x, "endif outside if"); break; } x->indent--; line(x, "}"); break;
        case 27: {   // loop aL, i#
            tok(x);   // aL
            int n = reg_num(tok(x));
            if (n >= 16) { fail(x, "loop counter i%d", n); break; }
            char iv[32]; snprintf(iv, sizeof iv, x->def_i[n] ? "di%d" : "k.i[%d]", n);
            line(x, "{ int aL_save_ = aL; aL = %s.y; for (int it_ = 0; it_ < min(%s.x, 255); it_++, aL += %s.z) {", iv, iv, iv); x->indent++;
            break;
        }
        case 29: if (x->indent < 1) { fail(x, "endloop outside loop"); break; } x->indent--; line(x, "} aL = aL_save_; }"); break;
        case 38: {   // rep i#
            int n = reg_num(tok(x));
            if (n >= 16) { fail(x, "rep counter i%d", n); break; }
            char iv[32]; snprintf(iv, sizeof iv, x->def_i[n] ? "di%d" : "k.i[%d]", n);
            int id = x->tmp++;
            line(x, "for (int rp%d_ = 0; rp%d_ < min(%s.x, 255); rp%d_++) {", id, id, iv, id); x->indent++;
            break;
        }
        case 39: if (x->indent < 1) { fail(x, "endrep outside rep"); break; } x->indent--; line(x, "}"); break;
        case 44: line(x, "break;"); break;
        case 45: src(x, a, sizeof a); src(x, b, sizeof b); line(x, "if ((%s).x %s (%s).x) break;", a, cmp_op((int)ctl), b); break;
        case 96: src(x, a, sizeof a); line(x, "if (bool((%s).x)) break;", a); break;
        case 28: line(x, "return;"); break;
        case 25: case 26: case 30: fail(x, "subroutines (call/label) are not supported"); break;
        default: fail(x, "opcode %u", op); break;
        }
        if (x->err) break;
        if (x->pc != next) { fail(x, "opcode %u: %zu parameter tokens, expected %d", op, x->pc - (next - (size_t)len), len); break; }
        // the second instruction of a co-issued pair has run: commit the first one's write
        if (x->co_pending && !x->co_defer) {
            line(x, "%s.%s = %s.%s;", x->co_dst, x->co_mask, x->co_var, x->co_mask);
            x->co_pending = 0;
        }
    }
    if (!x->err && x->co_pending) fail(x, "co-issued pair without its second instruction");
}

int d3d9_translate_shader(const uint32_t *tokens, size_t ntokens, const ShaderVariant *var, ShaderInfo *info) {
    memset(info, 0, sizeof *info);
    if (!tokens || ntokens < 2) { snprintf(info->error, sizeof info->error, "empty shader"); return 0; }
    Tr *x = calloc(1, sizeof *x);
    if (!x) { snprintf(info->error, sizeof info->error, "out of memory"); return 0; }
    x->t = tokens; x->n = ntokens; x->info = info; x->var = var;
    uint32_t ver = tokens[0];
    info->version = ver;
    x->ps = (ver >> 16) == 0xFFFF; x->major = (int)((ver >> 8) & 0xFF); x->minor = (int)(ver & 0xFF);
    info->stage = x->ps;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < ntokens; i++) { h ^= tokens[i]; h *= 16777619u; }
    info->hash = h;
    int ok = 0;
    if ((ver >> 16) != 0xFFFE && (ver >> 16) != 0xFFFF) snprintf(info->error, sizeof info->error, "not a shader (0x%08x)", ver);
    else if (x->major < 1 || x->major > 3 || (x->major == 1 && (x->ps ? x->minor > 4 : x->minor > 1)))
        snprintf(info->error, sizeof info->error, "shader model %d.%d is not supported", x->major, x->minor);
    else {
        prepass(x);
        if (!x->err) emit(x);
        if (!x->err && x->ps && sm1(x)) {   // shader model 1 pixel shaders output r0
            if (x->temps < 1) x->temps = 1;
            bput(&x->body, "    o.color0 = r0;\n");
            x->oColor_mask |= 1;
        }
        if (!x->err && x->body.oom) fail(x, "out of memory");
        if (!x->err) {
            info->color_mask = (uint8_t)x->oColor_mask; info->writes_depth = (uint8_t)x->oDepth;
            Buf o = {0};
            if (x->ps) bput(&o, "static void ps_%08x(thread PSRegs &in, thread PSORegs &o, constant PSConsts &k", h);
            else bput(&o, "static void vs_%08x(thread float4 *v, thread VSRegs &o, constant VSConsts &k", h);
            for (int s = 0; s < 16; s++) if (info->samplers & (1u << s)) {
                int tt = info->sampler_type[s] ? info->sampler_type[s] : 2;
                bput(&o, ", %s<float> tex%d, sampler smp%d", tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d", s, s);
            }
            bput(&o, ") {\n");
            for (int r = 0; r < x->temps; r++) bput(&o, "    float4 r%d = float4(0.0);\n", r);
            for (int r = 0; r < 10; r++) if (x->t_used & (1u << r)) bput(&o, "    float4 t%d = in.TEXCOORD%d;\n", r, r);
            for (int r = 0; r < 256; r++) if (x->def_f[r]) bput(&o, "    const float4 d%d = float4(%.9g, %.9g, %.9g, %.9g);\n", r, x->def_fv[r][0], x->def_fv[r][1], x->def_fv[r][2], x->def_fv[r][3]);
            for (int r = 0; r < 16; r++) if (x->def_i[r]) bput(&o, "    const int4 di%d = int4(%d, %d, %d, %d);\n", r, x->def_iv[r][0], x->def_iv[r][1], x->def_iv[r][2], x->def_iv[r][3]);
            for (int r = 0; r < 16; r++) if (x->def_b[r]) bput(&o, "    const bool db%d = %s;\n", r, x->def_bv[r] ? "true" : "false");
            if (x->uses_a0) bput(&o, "    int4 a0 = int4(0);\n");
            if (x->uses_p0) bput(&o, "    bool4 p0 = bool4(false);\n");
            bput(&o, "    int aL = 0; (void)aL;\n");
            bput(&o, "%s}\n", x->body.s ? x->body.s : "");
            if (o.oom) { free(o.s); snprintf(info->error, sizeof info->error, "out of memory"); }
            else { info->msl = o.s; ok = 1; }
        }
    }
    free(x->body.s);
    free(x);
    return ok;
}

size_t d3d9_shader_length(const uint32_t *t, size_t max) {
    if (max < 1) return 0;
    uint32_t ver = t[0];
    int ps = (ver >> 16) == 0xFFFF, major = (int)((ver >> 8) & 0xFF), minor = (int)(ver & 0xFF);
    size_t pc = 1;
    while (pc < max) {
        uint32_t ins = t[pc];
        if ((ins & 0xFFFF) == OP_END) return pc + 1;
        int len = ins_len(ps, major, minor, ins);
        if (len < 0) return 0;
        pc += 1 + (size_t)len;
    }
    return 0;
}

// The constant table of the compiler (a comment block "CTAB": D3DXSHADER_CONSTANTTABLE, then the
// D3DXSHADER_CONSTANTINFO entries of 20 bytes; the offsets count from the start of the table, after "CTAB")
int d3d9_shader_const_reg(const uint32_t *t, size_t n, const char *name, uint32_t *reg, uint32_t *count) {
    if (n < 1) return 0;
    uint32_t ver = t[0];
    int ps = (ver >> 16) == 0xFFFF, major = (int)((ver >> 8) & 0xFF), minor = (int)(ver & 0xFF);
    size_t nl = strlen(name);
    for (size_t pc = 1; pc < n; ) {
        uint32_t ins = t[pc];
        if ((ins & 0xFFFF) == OP_END) break;
        int len = ins_len(ps, major, minor, ins);
        if (len < 0 || (size_t)len >= n - pc) return 0;
        if ((ins & 0xFFFF) == OP_COMMENT && len >= 8 && t[pc + 1] == 0x42415443u) {   // "CTAB"
            const uint8_t *tab = (const uint8_t *)(t + pc + 2);
            size_t tn = ((size_t)len - 1) * 4;
            uint32_t h[7];
            memcpy(h, tab, sizeof h);   // Size, Creator, Version, Constants, ConstantInfo, Flags, Target
            if (h[4] > tn || h[3] > (tn - h[4]) / 20) return 0;
            for (uint32_t k = 0; k < h[3]; k++) {
                const uint8_t *ci = tab + h[4] + (size_t)k * 20;
                uint32_t no; uint16_t set, idx, cnt;
                memcpy(&no, ci, 4); memcpy(&set, ci + 4, 2); memcpy(&idx, ci + 6, 2); memcpy(&cnt, ci + 8, 2);
                if (set != 2 || no >= tn || tn - no <= nl) continue;   // D3DXRS_FLOAT4; the name and its 0 must fit
                if (memcmp(tab + no, name, nl) || tab[no + nl]) continue;
                *reg = idx; *count = cnt;
                return 1;
            }
        }
        pc += 1 + (size_t)len;
    }
    return 0;
}

// ---- test entry points (tests/d3d9_shader_test.m); the device builds its own (d3d9_metal.m) ----
static void tex_params(Buf *o, const ShaderInfo *s, int call) {
    for (int i = 0; i < 16; i++) if (s->samplers & (1u << i)) {
        int tt = s->sampler_type[i] ? s->sampler_type[i] : 2;
        if (call) bput(o, ", tex%d, smp%d", i, i);
        else bput(o, ", %s<float> tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d", i, i, i, i);
    }
}

char *d3d9_vs_source(const ShaderInfo *vs, const char *vsin, const char *fetch) {
    Buf o = {0};
    bput(&o, "%s%s", d3d9_msl_prelude, vs->msl);
    bput(&o, "%s\n", vsin ? vsin : "struct VSIn { float4 dummy [[attribute(0)]]; };");
    bput(&o, "vertex VSOut vs_main(VSIn in [[stage_in]], constant VSConsts &k [[buffer(0)]]");
    tex_params(&o, vs, 0);
    bput(&o, ") {\n    float4 v[16];\n    for (int i = 0; i < 16; i++) v[i] = float4(0.0, 0.0, 0.0, 1.0);\n");
    bput(&o, "%s\n", fetch ? fetch : "    (void)in;");
    bput(&o, "    VSRegs o; vs_regs_init(o);\n    vs_%08x(v, o, k", vs->hash);
    tex_params(&o, vs, 1);
    bput(&o, ");\n    return vs_out_from(o, k.pos_fixup);\n}\n");
    if (o.oom) { free(o.s); return NULL; }
    return o.s;
}

char *d3d9_ps_source(const ShaderInfo *ps) {
    Buf o = {0};
    bput(&o, "%s%s", d3d9_msl_prelude, ps->msl);
    bput(&o, "struct PSOutC {");
    for (int i = 0; i < 4; i++) if (ps->color_mask & (1 << i)) bput(&o, " float4 c%d [[color(%d)]];", i, i);
    if (ps->writes_depth) bput(&o, " float depth [[depth(any)]];");
    if (!ps->color_mask && !ps->writes_depth) bput(&o, " float4 c0 [[color(0)]];");
    bput(&o, " };\n");
    bput(&o, "fragment PSOutC ps_main(PSIn in [[stage_in]], bool face [[front_facing]], constant PSConsts &k [[buffer(0)]]");
    tex_params(&o, ps, 0);
    bput(&o, ") {\n    PSRegs r; ps_regs_from(r, in, face);\n    PSORegs o = {};\n    ps_%08x(r, o, k", ps->hash);
    tex_params(&o, ps, 1);
    bput(&o, ");\n");
    if ((ps->version >> 8 & 0xFF) < 3) bput(&o, "    o.color0 = d3d_fog(o.color0, in, k);\n");
    bput(&o, "    if (!d3d_alpha_pass(o.color0.w, k)) discard_fragment();\n    PSOutC res;\n");
    for (int i = 0; i < 4; i++) if (ps->color_mask & (1 << i)) bput(&o, "    res.c%d = o.color%d;\n", i, i);
    if (ps->writes_depth) bput(&o, "    res.depth = o.depth4.x;\n");
    if (!ps->color_mask && !ps->writes_depth) bput(&o, "    res.c0 = float4(0.0);\n");
    bput(&o, "    return res;\n}\n");
    if (o.oom) { free(o.s); return NULL; }
    return o.s;
}
