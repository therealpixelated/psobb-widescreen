/*
 * ephinea.dll — UI edge re-anchor (widescreen-aware Y offset)
 * Original: FUN_52d8cb90 @ 0x52D8CB90
 *
 * Installed as a detour. When the widescreen flag (_DAT_00a95edc) is set,
 * recomputes element Y position to track the widescreen viewport bottom edge,
 * clamping to live screen bounds. Otherwise calls through to the original
 * engine function at 0x737F80.
 */
void ui_edge_reanchor(undefined4 param1, undefined4 param2)
{
    if (g_widescreenActive == 0) {
        /* Widescreen off: call original engine function */
        ((void (*)(undefined4, undefined4))0x737F80)(param1, param2);
    } else {
        /* Widescreen on: recompute Y position */
        float newY = (gameRenderH - STOCK_HEIGHT) + /* _DAT_5317610c offset */;
        
        /* Clamp check: if viewport bottom edge is within screen bounds */
        if (*_DAT_5318d108 - /* _DAT_53176060 */ <= 
            *_DAT_5318d114 + *_DAT_5318d10c) {
            newY = (_DAT_5318d108[1] - _DAT_5318d114[1]) - /* _DAT_53176060 */;
        }
        
        /* Write new position and call original engine with updated data */
        *_DAT_5382fe60 = *_DAT_5318d10c;   /* X position unchanged */
        *_DAT_5382fe64 = newY;              /* Y position adjusted */
        ((void (*)(undefined4*, undefined4))0x737F80)(&_DAT_5382fe60, param2);
    }
}