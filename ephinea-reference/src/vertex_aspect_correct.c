/*
 * ephinea.dll — Core 2D-vertex aspect-correct + recenter
 * Original: FUN_52da6e50 @ 0x52DA6E50
 *
 * Installed as a detour at engine site 0x67C4ED. Transforms every vertex
 * in a 2D draw call to correct for widescreen aspect ratio, then calls
 * the original draw function at 0x82B558.
 *
 * The transform:
 *   1. Computes whether the render target is taller (vertical letterbox) or
 *      wider (horizontal pillarbox) relative to 4:3.
 *   2. Uniformly scales both X and Y by the limiting dimension's ratio.
 *   3. Re-centers on the long axis by adding half the letterbox/pillarbox
 *      offset to every vertex.
 *   4. Calls the original engine draw function.
 *
 * Must be matched for pixel parity.
 */
void vertex_aspect_correct_transform(int* vertexBuffer,
                                     undefined4 param2,
                                     undefined4 param3,
                                     undefined4 param4)
{
    float scale;
    float offset;
    int   vertexCount;
    int   i;
    int   originalFn = 0x82B558;  /* original engine draw fn */
    int   base;

    vertexCount = vertexBuffer[3];   /* param_1[3] = number of vertices */
    base = vertexBuffer[0];          /* param_1[0] = vertex buffer base */

    /* ── Determine aspect relationship ──
     *
     * If render aspect < 4/3, the view is TALLER than stock (vertical
     * letterbox). Scale uniformly by renderW/640 and recenter Y.
     *
     * If render aspect >= 4/3, the view is WIDER (horizontal pillarbox).
     * Scale uniformly by renderH/480 and recenter X.
     */
    if (gameRenderW / gameRenderH < ASPECT_4_3) {
        /* Vertical letterbox — taller than 4:3 */
        scale = gameRenderW / STOCK_WIDTH;
        offset = gameRenderH / HALF - (STOCK_HEIGHT * scale) / HALF;
    } else {
        /* Horizontal pillarbox — wider than 4:3 */
        scale = gameRenderH / STOCK_HEIGHT;
        offset = gameRenderW / HALF - (STOCK_WIDTH * scale) / HALF;
    }

    /* ── Transform each vertex ──
     *
     * For each vertex (stride 2 floats), scale BOTH coordinates and
     * then shift the long-axis coordinate by the centering offset.
     */
    for (i = 0; i < vertexCount; i++) {
        float* x = (float*)(base + (i*2) * 4);
        float* y = (float*)(base + (i*2 + 1) * 4);

        *x *= scale;
        *y *= scale;

        if (gameRenderW / gameRenderH < ASPECT_4_3) {
            /* Vertical letterbox: shift Y to center */
            *y += offset;
        } else {
            /* Horizontal pillarbox: shift X to center */
            *x += offset;
        }
    }

    /* ── Call original engine draw ── */
    ((void (*)(int*, undefined4, undefined4, undefined4, int))originalFn)
        (vertexBuffer, param2, param3, param4, vertexBuffer[1]);
}