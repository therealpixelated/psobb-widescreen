/*
 * ephinea.dll — HUD/UI quad design-anchor scaler
 * Original: FUN_52dc4f60 @ 0x52DC4F60
 *
 * Installed as a detour at engine site 0x78A300. Reads the design-anchor
 * scale from the device context (via FUN_52dd2610 → 0xACC0E8/EC), applies
 * the master scale _DAT_53175e64 (normally 1.0), conditionally applies an
 * extra scalar _DAT_5318e34c, and forwards the result to the quad renderer
 * FUN_52dc5b30.
 *
 * Two guard conditions:
 *   1. Only scales if both local_1c > 1.0 AND local_18 > 1.0 (design anchors
 *      are large enough to warrant scaling)
 *   2. If _DAT_53175ec8 < _DAT_5318e34c, applies the extra scalar
 */
void hud_quad_scaler(undefined4 param1, undefined4 param2, undefined4 param3,
                     undefined4 param4, undefined4 param5, float param6,
                     undefined4 param7)
{
    float scaleX, scaleY;
    float* scalePtr;
    float designAnchorX, designAnchorY;

    /* Read design-anchor scale from device context (0xACC0E8/EC) */
    read_design_anchor(&designAnchorX, &designAnchorY);  /* FUN_52dd2610 */

    /* Default: identity scale */
    scaleX = ONE;
    scaleY = ONE;
    scalePtr = NULL;

    /* Guard 1: Only scale if both anchors exceed identity */
    if (ONE < designAnchorX && ONE < designAnchorY) {
        scaleX = ONE * designAnchorX;
        scaleY = ONE * designAnchorY;
        scalePtr = &scaleX;
    }

    /* Guard 2: Apply extra scalar if threshold is exceeded */
    if ((double) /* _DAT_53175ec8 threshold */ < (double)g_5318e34c) {
        scaleX *= g_5318e34c;
        scaleY *= g_5318e34c;
        scalePtr = &scaleX;
    }

    /* Call original quad renderer with updated scale */
    /* param6 compared to _DAT_53175df8 for equality flag */
    quad_render(param1, param2, param3, param4, param5,
                (param6 != /* _DAT_53175df8 */ 0.0f) ? 1 : 0,
                param7, scalePtr);
}