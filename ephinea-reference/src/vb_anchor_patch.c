/*
 * ephinea.dll — Per-device vertex-buffer anchor patch
 * Original: FUN_52dac110 @ 0x52DAC110
 *
 * Guarded by renderH != 480 AND device != NULL AND device[4] != NULL.
 * Writes gameRenderW to 9 device-context offsets, then runs a loop of
 * 20 additional delta writes (first 8 += (gameRenderW-640), last 12
 * += (gameRenderH-480)).
 *
 * Detour wiring at engine site ~0x408C88 is NOT verified in the decompile
 * — only the handler body is confirmed.
 */
void vb_anchor_patch(void* device)
{
    int* ctx;  /* direct3D device context (device[4]) */
    int  i;

    if (gameRenderH == STOCK_HEIGHT || device == NULL)
        return;

    ctx = *(int**)((int*)device + 1);  /* iVar1 = *(int*)(device + 4) */
    if (ctx == NULL)
        return;

    /* ── Write gameRenderW to 9 device-context registers ──
     * These are vertex-buffer viewport/scissor registers at fixed
     * byte offsets from the context base. */
    ctx[0x24/4]   = (int)gameRenderW;
    ctx[0x40/4]   = (int)gameRenderW;
    ctx[0x2498/4] = (int)gameRenderW;
    ctx[0x251C/4] = (int)gameRenderW;
    ctx[0x25A0/4] = (int)gameRenderW;
    ctx[0x2664/4] = (int)gameRenderW;
    ctx[0x26E8/4] = (int)gameRenderW;
    ctx[0x3608/4] = (int)gameRenderW;
    ctx[0x368C/4] = (int)gameRenderW;

    /* ── 20 additional delta writes via offset table ──
     * Each entry in the table at 0x53146308 gives a byte offset into
     * the device context. First 8 entries get (gameRenderW - 640),
     * remaining 12 get (gameRenderH - 480). */
    for (i = 0; i < 0x14; i++) {
        int* offsetTable = (int*)0x53146308;  /* 20 entries × 4 bytes */
        int  devOffset   = offsetTable[i];
        float* target    = (float*)((char*)ctx + devOffset);

        if (i < 8) {
            *target += (gameRenderW - STOCK_WIDTH);
        } else {
            *target += (gameRenderH - STOCK_HEIGHT);
        }
    }
}